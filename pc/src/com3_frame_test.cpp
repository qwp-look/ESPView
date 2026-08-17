// ESPView M1-3B — PC COM3 帧管线测试工具（host-side，真实串口）。
//
// 链路：ESP32 TestPattern → ProtocolEndpoint → UART → CH340 → COM3
//       → HostUartTransport → RxHook(test-only) → ProtocolEndpoint → FrameAssembler
//
// 职责：COM3 控制器 + 测试协调器 + 统计收集 + raw-byte corruption/seq-gap 注入。
// 协议数据（HELLO/PING/帧消息）全部由 shared/protocol 的 C++ Encoder /
// ProtocolEndpoint 产生；本文件不复制任何 Packet/Message 编码逻辑。
//
// 线程模型：RX worker 线程只做 传输回调 → RxHook → ByteQueue；主线程独占
//   ProtocolEndpoint（drain + tick + sendHello），避免并发访问。
//
// 模式（--mode）：
//   full-small   小 FULL（320x240，4×16x16 rect）逐字节校验
//   full-large   单 RECT 320x240（153600B，CHUNKED）逐字节校验 + 带宽统计
//   partial      FULL 后 PARTIAL 提交；重连后 PARTIAL 无基准拒绝 + FULL 恢复
//   corruption   RX 钩子翻转目标帧 END 包 1 字节 → CRC 错误 → 帧作废 → 下一 FULL 恢复
//   seq-gap      RX 钩子丢弃目标帧 END 包 → SEQ 跳变 → 帧作废 → 下一 FULL 恢复
//   reconnect    断线（等对端超时）→ 重连 → HELLO → PARTIAL 无基准拒绝 → FULL 恢复
//
// 纯 C++17 + Win32 COM API，无 Qt / ESP-IDF。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crc32.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "serial_transport.h"

using espview::pc::HostUartTransport;
using espview::proto::CommittedFrame;
using espview::proto::EndpointConfig;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameType;
using espview::proto::HelloInfo;
using espview::proto::Message;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::RectInfo;
using espview::proto::SendStatus;
using espview::proto::SessionError;
using espview::proto::SessionState;

namespace {

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

uint16_t le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

// ---- 线程安全字节队列（RX worker → 主线程）----
class ByteQueue {
public:
    void push(const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> l(m_);
        buf_.insert(buf_.end(), d, d + n);
    }
    bool popAll(std::vector<uint8_t>& out) {
        // 必须先清空 out：swap 会把 out 中上一次的整块数据换回 buf_，
        // 若 drain 期间 RX 线程又推入任何字节，popAll 会永远交换同一块数据（粘滞死循环）。
        out.clear();
        std::lock_guard<std::mutex> l(m_);
        if (buf_.empty()) {
            return false;
        }
        out.swap(buf_);
        return true;
    }
    size_t sizeBytes() {
        std::lock_guard<std::mutex> l(m_);
        return buf_.size();
    }

private:
    std::mutex m_;
    std::vector<uint8_t> buf_;
};

// ---- RX 测试钩子（test-only）：统计 + 帧瞄准 + 注入 CRC 损坏 / 丢包 ----
// 运行在 RX worker 线程；arm/disarm/resetSeqBaseline 由主线程调用。
class RxHook {
public:
    enum class Action : uint8_t { kPass = 0, kCorruptEnd = 1, kDropEnd = 2 };

    void setAction(Action a) {
        std::lock_guard<std::mutex> l(m_);
        action_ = a;
    }
    void enableDebugDump() {
        std::lock_guard<std::mutex> l(m_);
        debugDump_ = true;
    }
    void arm() {
        std::lock_guard<std::mutex> l(m_);
        armed_ = true;
        injected_ = false;
        inTargetFrame_ = false;
    }
    void resetSeqBaseline() {
        std::lock_guard<std::mutex> l(m_);
        seqReady_ = true;
        expectedSeq_ = 0;
    }

    // 处理一个读块；输出字节（可能被修改/丢弃）进入 out。
    void process(const uint8_t* data, size_t len, ByteQueue& out) {
        std::lock_guard<std::mutex> l(m_);
        const size_t preAppend = pending_.size();
        pending_.insert(pending_.end(), data, data + len);
        if (debugDump_ && debugDumped_ < 160) {
            for (size_t i = 0; i < len && debugDumped_ < 160; ++i) {
                std::printf("%02X ", data[i]);
                ++debugDumped_;
                if (debugDumped_ % 16 == 0) {
                    std::printf("\n");
                }
            }
            if (debugDumped_ == 160) {
                std::printf("\n[dump end]\n");
            }
        }
        bool firstInCall = true;
        uint64_t completed = 0;
        while (true) {
            if (pending_.size() < espview::proto::kPacketHeaderSize) {
                break;
            }
            if (std::memcmp(pending_.data(), espview::proto::kProtocolMagic.data(), 4) != 0) {
                ++badMagic_;
                pending_.erase(pending_.begin());
                continue;
            }
            PacketHeader h;
            if (espview::proto::decodeHeader(pending_.data(), espview::proto::kPacketHeaderSize,
                                             &h) != PacketError::kNone) {
                ++protocolErrors_;
                pending_.erase(pending_.begin());
                continue;
            }
            const size_t total = espview::proto::kPacketHeaderSize + h.length;
            if (pending_.size() < total) {
                break;  // 半包：等待后续读块
            }

            if (firstInCall && preAppend > 0) {
                ++packetsSpanningReads_;  // 该包起始于上一次读块 → 跨 read 拆分证据
            }
            firstInCall = false;
            ++completed;
            if (h.type < typeCounts_.size()) {
                ++typeCounts_[h.type];
            }

            // 帧瞄准：BEGIN 记录类型/ID；armed 后第一个 FULL 帧的 END 为注入目标。
            const uint8_t* payload = pending_.data() + espview::proto::kPacketHeaderSize;
            if (h.type == static_cast<uint8_t>(MessageType::kFrameBegin) && h.length >= 3) {
                const uint16_t fid = le16(payload);
                const uint8_t ftype = payload[2];
                if (armed_ && ftype == static_cast<uint8_t>(FrameType::kFull)) {
                    inTargetFrame_ = true;
                    targetFrameId_ = fid;
                } else {
                    inTargetFrame_ = false;
                }
            }
            bool dropThis = false;
            if (inTargetFrame_ && !injected_ &&
                h.type == static_cast<uint8_t>(MessageType::kFrameEnd)) {
                if (action_ == Action::kCorruptEnd && h.length >= 1) {
                    // 翻转 END payload 末字节 → 该包 CRC 必失败。
                    pending_[espview::proto::kPacketHeaderSize + h.length - 1] ^= 0xFFu;
                } else if (action_ == Action::kDropEnd) {
                    dropThis = true;
                }
                injected_ = true;
                armed_ = false;
                inTargetFrame_ = false;
            }
            // 统计（按修改后的包字节计算 CRC：损坏注入被计入 crcErrors_）。
            const uint32_t calc = espview::proto::computePacketCrc(
                h, pending_.data() + espview::proto::kPacketHeaderSize, h.length);
            if (calc != h.crc32) {
                ++crcErrors_;
            }
            if (debugDump_ && debugPackets_ < 12) {
                std::printf("  [hook] pkt type=0x%02X seq=%u len=%u crc=%s\n",
                            static_cast<unsigned>(h.type), static_cast<unsigned>(h.seq),
                            static_cast<unsigned>(h.length),
                            calc == h.crc32 ? "OK" : "BAD");
                ++debugPackets_;
            }


            if (seqReady_) {
                if (h.seq != expectedSeq_) {
                    ++seqGaps_;
                }
                expectedSeq_ = static_cast<uint16_t>(h.seq + 1);
            }

            if (!dropThis) {
                out.push(pending_.data(), total);
            }
            pending_.erase(pending_.begin(),
                           pending_.begin() + static_cast<std::ptrdiff_t>(total));
        }
        if (completed > maxPacketsPerRead_) {
            maxPacketsPerRead_ = completed;
        }
        rxPackets_ += completed;
    }

    // ---- 统计访问（主线程）----
    uint64_t rxPackets() const {
        std::lock_guard<std::mutex> l(m_);
        return rxPackets_;
    }
    uint64_t badMagic() const {
        std::lock_guard<std::mutex> l(m_);
        return badMagic_;
    }
    uint64_t crcErrors() const {
        std::lock_guard<std::mutex> l(m_);
        return crcErrors_;
    }
    uint64_t protocolErrors() const {
        std::lock_guard<std::mutex> l(m_);
        return protocolErrors_;
    }
    uint64_t seqGaps() const {
        std::lock_guard<std::mutex> l(m_);
        return seqGaps_;
    }
    bool injected() const {
        std::lock_guard<std::mutex> l(m_);
        return injected_;
    }
    uint16_t targetFrameId() const {
        std::lock_guard<std::mutex> l(m_);
        return targetFrameId_;
    }
    uint64_t packetsSpanningReads() const {
        std::lock_guard<std::mutex> l(m_);
        return packetsSpanningReads_;
    }
    std::array<uint64_t, 0x60> typeCounts() const {
        std::lock_guard<std::mutex> l(m_);
        return typeCounts_;
    }
    uint64_t maxPacketsPerRead() const {
        std::lock_guard<std::mutex> l(m_);
        return maxPacketsPerRead_;
    }

private:
    mutable std::mutex m_;
    std::vector<uint8_t> pending_;
    Action action_ = Action::kPass;
    bool armed_ = false;
    bool inTargetFrame_ = false;
    bool injected_ = false;
    uint16_t targetFrameId_ = 0;
    uint16_t expectedSeq_ = 0;
    bool seqReady_ = false;
    bool debugDump_ = false;
    size_t debugDumped_ = 0;
    size_t debugPackets_ = 0;
    uint64_t rxPackets_ = 0;
    uint64_t badMagic_ = 0;
    uint64_t crcErrors_ = 0;
    uint64_t protocolErrors_ = 0;
    uint64_t seqGaps_ = 0;
    uint64_t packetsSpanningReads_ = 0;
    uint64_t maxPacketsPerRead_ = 0;
    std::array<uint64_t, 0x60> typeCounts_{};
};

// ---- 帧记录（含像素副本，测试专用）----

// ---- 确定性回归：ByteQueue 粘滞死循环（M1-3C 定位）----
// 旧实现 popAll 用 out.swap(buf_) 且不清空 out：drain 期间 RX 线程推入任意
// 字节后，同一块数据会在 out/buf_ 之间反复交换，popAll 永远返回 true。
bool selfTestByteQueue() {
    ByteQueue q;
    std::vector<uint8_t> chunk;
    const std::vector<uint8_t> a = {1, 2, 3, 4};
    q.push(a.data(), a.size());
    if (!q.popAll(chunk) || chunk != a) return false;  // chunk 现持有 a
    const std::vector<uint8_t> b = {5, 6};
    q.push(b.data(), b.size());
    std::vector<uint8_t> fresh;
    if (!q.popAll(fresh) || fresh != b) return false;  // fresh 为新向量，无问题
    const std::vector<uint8_t> c = {7, 8, 9, 10, 11};
    q.push(c.data(), c.size());
    // 复用 chunk（持有 a）：旧实现会把 a 换回队列，popAll 再次返回 true
    if (!q.popAll(chunk) || chunk != c) return false;
    if (q.popAll(chunk)) return false;  // 队列必须已空；旧实现会返回 true（粘滞）
    return true;
}

struct FrameRecord {
    CommittedFrame meta;
    std::vector<RectInfo> rects;
    std::vector<std::vector<uint8_t>> pixels;
    uint64_t beginMs = 0;
    uint64_t commitMs = 0;
};

// ---- 全局测试上下文 ----
struct Args {
    std::string port = "COM3";
    uint32_t baud = 115200;
    std::string mode = "full-small";
    bool reset = true;
    uint32_t timeoutMs = 90000;
    uint32_t reconnectWaitMs = 6500;
    uint32_t readTimeoutMs = 100;
};

struct Harness {
    Args args;
    HostUartTransport transport;
    ByteQueue queue;
    RxHook hook;
    std::unique_ptr<ProtocolEndpoint> ep;

    // 主线程收集
    bool connected = false;
    bool sawPeerHello = false;
    bool helloInitiated = false;  // 已主动发 HELLO（onTransportConnected）
    uint64_t openMs = 0;
    uint64_t connectedMs = 0;
    uint64_t lastCommitMs = 0;
    uint64_t lastDiscardMs = 0;
    uint64_t framesCommitted = 0;
    uint64_t framesDiscarded = 0;
    uint64_t txPackets = 0;
    uint64_t txBytes = 0;
    std::vector<SessionState> states;
    std::vector<std::string> protocolErrors;
    std::vector<std::string> errors;  // ERROR 消息文本（ESP32 heap 统计经此上报）
    std::vector<FrameRecord> commits;
    std::vector<FrameDiscardReason> discards;
    FrameRecord current;

    void resetSessionFlags() {
        connected = false;
        sawPeerHello = false;
        helloInitiated = false;
        current = FrameRecord{};
    }
};

void initEndpoint(Harness& h) {
    EndpointConfig cfg;
    cfg.protocol_version = espview::proto::kProtocolVersion;
    cfg.device_class = 0;
    cfg.width = 320;
    cfg.height = 240;
    cfg.pixel_format = PixelFormat::kRgb565;
    cfg.mode_mask = 0b1111;  // M7-C2：WINDOW|DEVICE|MIRROR|SPLIT
    cfg.device_name = "espview-pc";

    auto sink = [&h](const uint8_t* d, size_t n) -> SendStatus {
        ++h.txPackets;
        h.txBytes += n;
        return h.transport.send(d, n);  // M8-A3：send 直接返回 canonical SendStatus
    };

    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&h](SessionState s) {
        h.states.push_back(s);
        if (s == SessionState::kConnected) {
            h.connected = true;
            h.connectedMs = nowMs();
            h.hook.resetSeqBaseline();
            h.current = FrameRecord{};
        } else if (s == SessionState::kDisconnected) {
            h.connected = false;
            h.current = FrameRecord{};
        }
    };
    cb.onProtocolError = [&h](SessionError e, std::string_view d) {
        (void)e;
        h.protocolErrors.push_back(std::string(d));
    };
    cb.onHello = [&h](const HelloInfo&) { h.sawPeerHello = true; };
    cb.onFrameBegin = [&h](const FrameBeginInfo&) {
        h.current = FrameRecord{};
        h.current.beginMs = nowMs();
    };
    cb.onFrameRect = [&h](const RectInfo& r, const uint8_t* p, size_t n) {
        h.current.rects.push_back(r);
        h.current.pixels.emplace_back(p, p + n);
    };
    cb.onFrameCommit = [&h](const CommittedFrame& f) {
        h.current.meta = f;
        h.current.commitMs = nowMs();
        h.commits.push_back(std::move(h.current));
        h.current = FrameRecord{};
        ++h.framesCommitted;
        h.lastCommitMs = nowMs();
    };
    cb.onFrameDiscard = [&h](FrameDiscardReason r) {
        h.discards.push_back(r);
        h.current = FrameRecord{};
        ++h.framesDiscarded;
        h.lastDiscardMs = nowMs();
    };
    cb.onError = [&h](espview::proto::ErrorCode code, std::string_view text) {
        h.errors.push_back(std::string(text));
        std::printf("  [E] ERROR code=%u text=%.*s\n", static_cast<unsigned>(code),
                    static_cast<int>(text.size()), text.data());
    };
    h.ep = std::make_unique<ProtocolEndpoint>(cfg, sink, cb, nowMs);
}

void wireTransport(Harness& h) {
    h.transport.setDataCallback([&h](const uint8_t* d, size_t n) { h.hook.process(d, n, h.queue); });
    h.transport.setStateCallback([&h](HostUartTransport::State s) {
        if (s == HostUartTransport::State::kDisconnected || s == HostUartTransport::State::kError) {
            if (h.ep) {
                h.ep->onTransportDisconnected();
            }
        }
        // Connected 不触发 onTransportConnected：采用被动握手（等对端 HELLO），
        // 与 scripts/pc_com3_session_test.py 的先等 ESP32 HELLO 策略一致。
    });
}

bool openPort(Harness& h, bool resetPulse) {
    HostUartTransport::Config cfg;
    cfg.port = h.args.port;
    cfg.baud = h.args.baud;
    cfg.read_timeout_ms = h.args.readTimeoutMs;
    cfg.reset_on_open = resetPulse;
    h.transport.setConfig(cfg);  // M8-A3：canonical open() 无参；配置经 setConfig 注入
    h.resetSessionFlags();
    h.openMs = nowMs();
    return h.transport.open();
}

void closePort(Harness& h) {
    h.transport.close();
    h.resetSessionFlags();
}

// 主循环：drain + tick + 连接维持 + 场景阶段谓词。
bool pumpLoop(Harness& h, uint64_t timeoutMs, const std::function<bool()>& phase,
              const char* what) {
    const uint64_t start = nowMs();
    uint64_t lastBeat = 0;
    std::printf("  [wait] %s (%.1fs)\n", what, static_cast<double>(timeoutMs) / 1000.0);
    while (nowMs() - start < timeoutMs) {
        std::vector<uint8_t> chunk;
        while (h.queue.popAll(chunk)) {
            h.ep->onTransportData(chunk.data(), chunk.size());
        }
        h.ep->tick();
        if (nowMs() - start - lastBeat >= 10000) {
            lastBeat = nowMs() - start;
            std::printf(
                "  [pump] t=%llu ms state=%d connected=%d txPkt=%llu rxPkt=%llu commits=%zu "
                "discards=%zu decErr=%llu sessionErr=%llu queueBytes=%zu\n",
                static_cast<unsigned long long>(nowMs() - start), static_cast<int>(h.ep->state()),
                h.connected ? 1 : 0, static_cast<unsigned long long>(h.txPackets),
                static_cast<unsigned long long>(h.hook.rxPackets()), h.commits.size(),
                h.discards.size(), static_cast<unsigned long long>(h.ep->stats().decoderErrors),
                static_cast<unsigned long long>(h.ep->stats().errors), h.queue.sizeBytes());
    }
        if (!h.connected && !h.helloInitiated && nowMs() - h.openMs >= 7000) {
            // 被动等待超时（对端没有发来 HELLO，如无复位重连场景）：
            // 主动发起握手（onTransportConnected → 发本端 HELLO）。
            // 7s > ESP32 端 5s 握手超时：此时对端已进入 kDisconnected，
            // 收到本端 HELLO 会走被动恢复（回发 Hello #2 + completeHandshake），
            // 本端才能看到对端 HELLO 完成握手。首次上电场景 boot HELLO 约 1-2s
            // 内到达，不会触发该分支。
            h.helloInitiated = true;
            h.ep->onTransportConnected();
            std::printf("  [H] passive HELLO not seen in 7s -> initiating HELLO\n");
        }
        if (phase()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("  [wait] TIMEOUT: %s\n", what);
    return false;
}

bool waitConnected(Harness& h, uint64_t timeoutMs) {
    return pumpLoop(h, timeoutMs, [&h]() { return h.connected; }, "HELLO handshake -> CONNECTED");
}

// 确定性像素公式（与 ESP32 TestPattern 一致，test-only）：
//   RGB565: lo=(frameId+rectId+x)&0xFF, hi=(frameId+y+1)&0xFF, 小端字节对。
bool verifyFramePixels(const FrameRecord& fr) {
    if (fr.rects.size() != fr.meta.rectCount || fr.pixels.size() != fr.meta.rectCount) {
        return false;
    }
    size_t bytes = 0;
    const uint16_t frameId = fr.meta.frameId;
    for (uint16_t ri = 0; ri < fr.meta.rectCount; ++ri) {
        const auto& px = fr.pixels[ri];
        const uint16_t w = fr.rects[ri].w;
        const uint16_t ht = fr.rects[ri].h;
        if (px.size() != static_cast<size_t>(w) * ht * 2u) {
            return false;
        }
        bytes += px.size();
        for (uint16_t y = 0; y < ht; ++y) {
            for (uint16_t x = 0; x < w; ++x) {
                const uint8_t lo = static_cast<uint8_t>(frameId + ri + x);
                const uint8_t hi = static_cast<uint8_t>(frameId + y + 1u);
                const size_t off = (static_cast<size_t>(y) * w + x) * 2u;
                if (px[off] != lo || px[off + 1] != hi) {
                    return false;
                }
            }
        }
    }
    return bytes == fr.meta.byteCount;
}

bool hasCommitSince(const Harness& h, size_t since, bool full, uint16_t rectCount,
                    uint32_t byteCount) {
    for (size_t i = since; i < h.commits.size(); ++i) {
        const CommittedFrame& f = h.commits[i].meta;
        if ((!full || f.frameType == FrameType::kFull) && f.rectCount == rectCount &&
            f.byteCount == byteCount && f.width == 320 && f.height == 240) {
            return true;
        }
    }
    return false;
}

bool hasDiscardSince(const Harness& h, size_t since, FrameDiscardReason r) {
    for (size_t i = since; i < h.discards.size(); ++i) {
        if (h.discards[i] == r) {
            return true;
        }
    }
    return false;
}

const FrameRecord* lastFullCommit(const Harness& h, size_t since) {
    for (size_t i = h.commits.size(); i > since; --i) {
        if (h.commits[i - 1].meta.frameType == FrameType::kFull) {
            return &h.commits[i - 1];
        }
    }
    return nullptr;
}

// ================= 场景 =================

bool scenarioFullSmall(Harness& h) {
    const size_t c0 = h.commits.size();
    const bool ok = pumpLoop(h, h.args.timeoutMs, [&h, c0]() {
        for (size_t i = c0; i < h.commits.size(); ++i) {
            const FrameRecord& fr = h.commits[i];
            if (fr.meta.frameType == FrameType::kFull && fr.meta.rectCount == 4 &&
                fr.meta.byteCount == 2048 && fr.meta.width == 320 && fr.meta.height == 240) {
                std::printf("  [E] small FULL commit: frameId=%u rects=%u bytes=%u\n",
                            fr.meta.frameId, static_cast<unsigned>(fr.meta.rectCount),
                            fr.meta.byteCount);
                return verifyFramePixels(fr);
            }
        }
        return false;
    }, "small FULL (320x240, 4 rects, byte-verify)");
    if (!ok) {
        return false;
    }
    std::printf("  [E] PASS: small FULL 逐字节校验 OK\n");
    return true;
}

bool scenarioFullLarge(Harness& h) {
    const size_t c0 = h.commits.size();
    bool ok = pumpLoop(h, h.args.timeoutMs, [&h, c0]() {
        for (size_t i = c0; i < h.commits.size(); ++i) {
            const FrameRecord& fr = h.commits[i];
            // M1-3C：固件改为单 RECT 320x240 Streaming（Message 153608B 不驻留内存，
            // 拆 38 包）；向后兼容 M1-3B 的 8 条 320x30 条带固件，故接受 rectCount>=1。
            if (fr.meta.frameType == FrameType::kFull && fr.meta.rectCount >= 1 &&
                fr.meta.byteCount == 153600 && fr.meta.width == 320 && fr.meta.height == 240) {
                std::printf("  [E] large FULL commit: frameId=%u rects=%u bytes=%u elapsed=%llu ms\n",
                            fr.meta.frameId, static_cast<unsigned>(fr.meta.rectCount),
                            fr.meta.byteCount,
                            static_cast<unsigned long long>(fr.commitMs - fr.beginMs));
                return verifyFramePixels(fr);
            }
        }
        return false;
    }, "large FULL (320x240 153600B streaming CHUNKED)");
    if (!ok) {
        return false;
    }
    std::printf("  [E] PASS: 153600B Streaming FULL 逐字节校验 OK\n");

    // 等待并核对 ESP32 的 heap 统计上报（ERROR 消息，FRAME_END 之后）。
    bool sawHeap = false;
    ok = pumpLoop(h, 15000, [&h, &sawHeap]() {
        for (const std::string& e : h.errors) {
            if (e.rfind("heap_b=", 0) == 0) {
                std::printf("  [E] ESP32 heap report: %s\n", e.c_str());
                sawHeap = true;
                return true;
            }
        }
        return false;
    }, "ESP32 heap report (ERROR msg)");
    if (!ok) {
        std::printf("  [E] FAIL: no ESP32 heap report received\n");
        return false;
    }
    std::printf("  [E] PASS: ESP32 heap report received (before/during/after)\n");
    std::printf("  [E] frame rx: %llu packets (incl. control), tx: %llu packets, decErr=%llu\n",
                static_cast<unsigned long long>(h.hook.rxPackets()),
                static_cast<unsigned long long>(h.txPackets),
                static_cast<unsigned long long>(h.ep->stats().decoderErrors));
    return true;
}

bool scenarioPartial(Harness& h) {
    // 阶段 1：FULL 建立基准 → PARTIAL 提交。
    const size_t c0 = h.commits.size();
    bool ok = pumpLoop(h, h.args.timeoutMs, [&h, c0]() {
        return hasCommitSince(h, c0, true, 4, 2048);
    }, "FULL #100 (base)");
    if (!ok) {
        return false;
    }
    const uint16_t baseFrameId = h.commits.back().meta.frameId;
    ok = pumpLoop(h, 30000, [&h]() {
        return !h.commits.empty() && h.commits.back().meta.frameType == FrameType::kPartial;
    }, "PARTIAL #101 after FULL (must commit)");
    if (!ok) {
        return false;
    }
    std::printf("  [P] PASS: PARTIAL after FULL committed (base frameId=%u)\n", baseFrameId);

    // 阶段 2：断线 → 重连 → 无基准 → PARTIAL 拒绝 → FULL 恢复。
    std::printf("  [R] closing COM3, waiting %.1fs for peer timeout ...\n",
                static_cast<double>(h.args.reconnectWaitMs) / 1000.0);
    closePort(h);
    std::this_thread::sleep_for(std::chrono::milliseconds(h.args.reconnectWaitMs));
    if (!openPort(h, /*resetPulse=*/false)) {
        std::printf("  [R] FAIL: reopen failed\n");
        return false;
    }
    if (!waitConnected(h, 15000)) {
        std::printf("  [R] FAIL: reconnect handshake timeout\n");
        return false;
    }
    const size_t d0 = h.discards.size();
    const size_t c1 = h.commits.size();
    ok = pumpLoop(h, 20000, [&h, d0, c1]() {
        return hasDiscardSince(h, d0, FrameDiscardReason::kPartialWithoutBase) &&
               h.commits.size() == c1;  // 拒绝期间不得有提交
    }, "PARTIAL #102 without base (must be rejected)");
    if (!ok) {
        std::printf("  [P] FAIL: PARTIAL without base not rejected as expected\n");
        return false;
    }
    std::printf("  [P] PASS: PARTIAL without base rejected (no commit before discard)\n");
    ok = pumpLoop(h, 30000, [&h, baseFrameId]() {
        const FrameRecord* f = lastFullCommit(h, 0);
        return f != nullptr && f->meta.frameId != baseFrameId;
    }, "FULL #103 after reconnect (must commit)");
    if (!ok) {
        return false;
    }
    std::printf("  [P] PASS: FULL #103 committed after reconnect resync\n");
    return true;
}

bool scenarioCorruption(Harness& h) {
    const size_t c0 = h.commits.size();
    bool ok = pumpLoop(h, h.args.timeoutMs, [&h, c0]() {
        return hasCommitSince(h, c0, true, 4, 2048) || hasCommitSince(h, c0, true, 1, 153600);
    }, "first FULL commit (arm target)");
    if (!ok) {
        return false;
    }
    h.hook.setAction(RxHook::Action::kCorruptEnd);
    h.hook.arm();
    const uint64_t decoderErrors0 = h.ep->stats().decoderErrors;

    ok = pumpLoop(h, 120000, [&h, decoderErrors0]() {
        return h.hook.injected() && h.ep->stats().decoderErrors > decoderErrors0;
    }, "target END corrupted (CRC error expected)");
    if (!ok) {
        std::printf("  [C] FAIL: corruption not injected / CRC error not seen\n");
        return false;
    }
    const uint16_t target = h.hook.targetFrameId();
    std::printf("  [C] target frameId=%u corrupted\n", target);
    ok = pumpLoop(h, 30000, [&h, target]() {
        return !h.commits.empty() && h.commits.back().meta.frameId == target;
    }, "target frame must NOT commit");
    if (ok) {
        std::printf("  [C] FAIL: corrupted frame %u committed (should not)\n", target);
        return false;
    }
    // 会话不假装断开；下一 FULL 恢复。
    ok = pumpLoop(h, 60000, [&h, target]() {
        const FrameRecord* f = lastFullCommit(h, 0);
        return h.connected && f != nullptr && f->meta.frameId != target;
    }, "next FULL after corruption (must commit, session alive)");
    if (!ok) {
        std::printf("  [C] FAIL: no recovery FULL commit or session dropped\n");
        return false;
    }
    std::printf("  [C] PASS: CRC corruption -> frame discarded -> next FULL committed, "
                "session stayed CONNECTED\n");
    return true;
}

bool scenarioSeqGap(Harness& h) {
    const size_t c0 = h.commits.size();
    bool ok = pumpLoop(h, h.args.timeoutMs, [&h, c0]() {
        return hasCommitSince(h, c0, true, 4, 2048) || hasCommitSince(h, c0, true, 1, 153600);
    }, "first FULL commit (arm target)");
    if (!ok) {
        return false;
    }
    h.hook.setAction(RxHook::Action::kDropEnd);
    h.hook.arm();
    const uint64_t decoderErrors0 = h.ep->stats().decoderErrors;

    ok = pumpLoop(h, 120000, [&h, decoderErrors0]() {
        return h.hook.injected() && h.ep->stats().decoderErrors > decoderErrors0;
    }, "target END dropped (SEQ gap expected)");
    if (!ok) {
        std::printf("  [G] FAIL: drop not injected / seq gap not seen\n");
        return false;
    }
    const uint16_t target = h.hook.targetFrameId();
    std::printf("  [G] target frameId=%u END dropped\n", target);
    ok = pumpLoop(h, 30000, [&h, target]() {
        return !h.commits.empty() && h.commits.back().meta.frameId == target;
    }, "target frame must NOT commit");
    if (ok) {
        std::printf("  [G] FAIL: dropped-frame %u committed (should not)\n", target);
        return false;
    }
    ok = pumpLoop(h, 60000, [&h, target]() {
        const FrameRecord* f = lastFullCommit(h, 0);
        return h.connected && f != nullptr && f->meta.frameId != target;
    }, "next FULL after seq gap (must commit, session alive)");
    if (!ok) {
        std::printf("  [G] FAIL: no recovery FULL commit or session dropped\n");
        return false;
    }
    std::printf("  [G] PASS: SEQ gap -> frame discarded -> next FULL committed, "
                "session stayed CONNECTED\n");
    return true;
}

bool scenarioReconnect(Harness& h) {
    const size_t c0 = h.commits.size();
    bool ok = pumpLoop(h, h.args.timeoutMs, [&h, c0]() {
        return hasCommitSince(h, c0, true, 4, 2048) || hasCommitSince(h, c0, true, 1, 153600);
    }, "first FULL commit");
    if (!ok) {
        return false;
    }
    const uint16_t firstFrameId = h.commits.back().meta.frameId;
    std::printf("  [R] first FULL committed frameId=%u\n", firstFrameId);

    closePort(h);
    std::printf("  [R] closed COM3; waiting %.1fs for peer timeout + drain ...\n",
                static_cast<double>(h.args.reconnectWaitMs) / 1000.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(h.args.reconnectWaitMs));
    if (!openPort(h, /*resetPulse=*/false)) {
        std::printf("  [R] FAIL: reopen failed\n");
        return false;
    }
    if (!waitConnected(h, 15000)) {
        std::printf("  [R] FAIL: reconnect handshake timeout\n");
        return false;
    }
    std::printf("  [R] PASS: HELLO re-handshake OK\n");

    // 重连后无基准：先出现 PARTIAL 无基准拒绝，再出现新的 FULL 提交。
    const size_t d0 = h.discards.size();
    const size_t c1 = h.commits.size();
    ok = pumpLoop(h, 30000, [&h, d0, c1, firstFrameId]() {
        return hasDiscardSince(h, d0, FrameDiscardReason::kPartialWithoutBase) &&
               h.commits.size() == c1 &&  // 拒绝期间无提交
               !h.commits.empty() && h.commits.back().meta.frameId == firstFrameId;  // 最后提交仍是旧帧
    }, "PARTIAL-without-base rejected after reconnect");
    if (!ok) {
        std::printf("  [R] FAIL: PARTIAL-without-base rejection not observed\n");
        return false;
    }
    ok = pumpLoop(h, 60000, [&h, firstFrameId]() {
        const FrameRecord* f = lastFullCommit(h, 0);
        return f != nullptr && f->meta.frameId > firstFrameId;
    }, "FULL resync after reconnect (new frameId)");
    if (!ok) {
        std::printf("  [R] FAIL: no FULL commit after reconnect\n");
        return false;
    }
    std::printf("  [R] PASS: reconnect -> PARTIAL-without-base rejected -> FULL resync committed\n");
    return true;
}

// ---- 统计输出 ----
void printStats(Harness& h, uint64_t totalMs, bool pass) {
    const auto stats = h.ep->stats();
    const auto sizes = h.transport.lastReadSizes(20);
    std::printf("\n==== M1-3B 统计 ====\n");
    std::printf("  elapsed: %.3f s\n", static_cast<double>(totalMs) / 1000.0);
    std::printf("  packets tx: %llu\n", static_cast<unsigned long long>(h.txPackets));
    std::printf("  packets rx: %llu\n", static_cast<unsigned long long>(h.hook.rxPackets()));
    std::printf("  bytes tx:   %llu\n", static_cast<unsigned long long>(h.txBytes));
    std::printf("  bytes rx:   %llu\n", static_cast<unsigned long long>(h.transport.rxBytes()));
    std::printf("  CRC errors (hook):        %llu\n",
                static_cast<unsigned long long>(h.hook.crcErrors()));
    std::printf("  protocol errors (hook):   %llu\n",
                static_cast<unsigned long long>(h.hook.protocolErrors()));
    std::printf("  bad magic (hook):         %llu\n",
                static_cast<unsigned long long>(h.hook.badMagic()));
    std::printf("  sequence gaps (hook):     %llu\n",
                static_cast<unsigned long long>(h.hook.seqGaps()));
    std::printf("  decoder errors (endpoint):%llu\n",
                static_cast<unsigned long long>(stats.decoderErrors));
    std::printf("  session errors (endpoint):%llu\n",
                static_cast<unsigned long long>(stats.errors));
    std::printf("  rx messages: %llu  tx messages: %llu  last RTT: %u ms\n",
                static_cast<unsigned long long>(stats.rxMessages),
                static_cast<unsigned long long>(stats.txMessages), stats.rtt.lastMs.value_or(0));
    // M4 spec §23：大帧心跳可观察（PING/PONG/超时/RTT 聚合）。
    std::printf("  heartbeat: pingTx=%llu pingRx=%llu pongTx=%llu pongRx=%llu timeouts=%llu"
                " rttValid=%d rtt=%u min=%u avg=%u max=%u n=%llu\n",
                static_cast<unsigned long long>(stats.txPing),
                static_cast<unsigned long long>(stats.rxPing),
                static_cast<unsigned long long>(stats.txPong),
                static_cast<unsigned long long>(stats.rxPong),
                static_cast<unsigned long long>(stats.pingTimeouts),
                stats.rtt.lastMs.has_value() ? 1 : 0,
                static_cast<unsigned>(stats.rtt.lastMs.value_or(0)),
                static_cast<unsigned>(stats.rtt.minMs), static_cast<unsigned>(stats.rtt.avgMs),
                static_cast<unsigned>(stats.rtt.maxMs),
                static_cast<unsigned long long>(stats.rtt.samples));
    std::printf("  frames committed: %llu\n",
                static_cast<unsigned long long>(h.framesCommitted));
    std::printf("  frames discarded: %llu\n",
                static_cast<unsigned long long>(h.framesDiscarded));
    if (!h.commits.empty()) {
        const FrameRecord& fr = h.commits.back();
        std::printf("  last committed: frameId=%u type=%d %ux%u rects=%u bytes=%u\n",
                    fr.meta.frameId, static_cast<int>(fr.meta.frameType),
                    static_cast<unsigned>(fr.meta.width), static_cast<unsigned>(fr.meta.height),
                    static_cast<unsigned>(fr.meta.rectCount), fr.meta.byteCount);
        if (fr.commitMs >= fr.beginMs) {
            const uint64_t dur = fr.commitMs - fr.beginMs;
            std::printf("  last frame duration: %llu ms", static_cast<unsigned long long>(dur));
            if (dur > 0 && fr.meta.byteCount > 0) {
                std::printf("  (payload %.1f KB/s)",
                            static_cast<double>(fr.meta.byteCount) * 1000.0 /
                                static_cast<double>(dur) / 1024.0);
            }
            std::printf("\n");
        }
    }
    if (h.transport.readCount() > 0) {
        std::printf("  UART reads: %llu, last %zu sizes:",
                    static_cast<unsigned long long>(h.transport.readCount()), sizes.size());
        for (const size_t s : sizes) {
            std::printf(" %zu", s);
        }
        std::printf("\n  packets spanning reads (hook): %llu, max packets per read: %llu\n",
                    static_cast<unsigned long long>(h.hook.packetsSpanningReads()),
                    static_cast<unsigned long long>(h.hook.maxPacketsPerRead()));
    }
    const auto tc = h.hook.typeCounts();
    std::printf("  rx packet types (hook):");
    for (size_t i = 0; i < tc.size(); ++i) {
        if (tc[i] != 0) {
            std::printf(" 0x%02zX=%llu", i, static_cast<unsigned long long>(tc[i]));
        }
    }
    std::printf("\n");
    std::printf("  endpoint session state: %d (0=Disconnected 1=Connecting 2=Handshake 3=Connected)\n",
                static_cast<int>(h.ep->state()));
    std::printf("  session states observed:");
    for (const SessionState st : h.states) {
        std::printf(" %d", static_cast<int>(st));
    }
    std::printf("\n");
    if (!h.protocolErrors.empty()) {
        std::printf("  protocol errors:");
        for (const auto& e : h.protocolErrors) {
            std::printf(" [%s]", e.c_str());
        }
        std::printf("\n");
    }
    std::printf("  commits:");
    for (const auto& c : h.commits) {
        std::printf(" id=%u/t=%d/r=%u/b=%u", c.meta.frameId,
                    static_cast<int>(c.meta.frameType), static_cast<unsigned>(c.meta.rectCount),
                    c.meta.byteCount);
    }
    std::printf("\n  discards:");
    for (const auto& d : h.discards) {
        std::printf(" %d", static_cast<int>(d));
    }
    std::printf("\n  RESULT: %s\n", pass ? "PASS" : "FAIL");
}

// ---- CLI ----
void usage(const char* prog) {
    std::printf(
        "usage: %s --port COM3 --baud 115200 --mode <mode> [options]\n"
        "  --mode full-small|full-large|partial|corruption|seq-gap|reconnect\n"
        "  --no-reset            不触发 DTR/RTS 复位\n"
        "  --timeout-ms N        场景总超时（默认 90000）\n"
        "  --reconnect-wait-ms N 断线后等待对端超时的时长（默认 6500）\n"
        "  --selftest-queue      仅运行 ByteQueue 回归自检后退出（不需要串口）\n"
        ,
        prog);
}


}  // namespace

int main(int argc, char** argv) {
    // 测试工具：stdout 无缓冲，保证日志在超时/被杀时仍可读。
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    Args a;
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port") {
            a.port = next("--port");
        } else if (arg == "--baud") {
            a.baud = static_cast<uint32_t>(std::stoul(next("--baud")));
        } else if (arg == "--mode") {
            a.mode = next("--mode");
        } else if (arg == "--no-reset") {
            a.reset = false;
        } else if (arg == "--selftest-queue") {
            selfTest = true;
            a.reset = false;
        } else if (arg == "--timeout-ms") {
            a.timeoutMs = static_cast<uint32_t>(std::stoul(next("--timeout-ms")));
        } else if (arg == "--reconnect-wait-ms") {
            a.reconnectWaitMs = static_cast<uint32_t>(std::stoul(next("--reconnect-wait-ms")));
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }

    const bool validMode = a.mode == "full-small" || a.mode == "full-large" ||
                           a.mode == "partial" || a.mode == "corruption" ||
                           a.mode == "seq-gap" || a.mode == "reconnect";
    if (!validMode) {
        std::fprintf(stderr, "unknown mode: %s\n", a.mode.c_str());
        usage(argv[0]);
        return 2;
    }

    Harness h;
    h.args = a;
    initEndpoint(h);
    wireTransport(h);

    const uint64_t t0 = nowMs();
    std::printf("== M1-3B com3_frame_test: port=%s baud=%u mode=%s reset=%s ==\n", a.port.c_str(),
                a.baud, a.mode.c_str(), a.reset ? "yes" : "no");

    if (selfTest) {
        const bool ok = selfTestByteQueue();
        std::printf("selfTestByteQueue: %s\n", ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (!openPort(h, a.reset)) {
        std::printf("FAIL: cannot open %s\n", a.port.c_str());
        return 1;
    }
    h.hook.enableDebugDump();

    bool pass = false;
    if (!waitConnected(h, 15000)) {
        std::printf("FAIL: initial HELLO handshake timeout\n");
    } else {
        std::printf("  [H] PASS: HELLO handshake, peer=%ux%u fmt=%d name=%s\n",
                    h.ep->peerHello().width, h.ep->peerHello().height,
                    static_cast<int>(h.ep->peerHello().pixel_format),
                    h.ep->peerHello().device_name.c_str());
        if (a.mode == "full-small") {
            pass = scenarioFullSmall(h);
        } else if (a.mode == "full-large") {
            pass = scenarioFullLarge(h);
        } else if (a.mode == "partial") {
            pass = scenarioPartial(h);
        } else if (a.mode == "corruption") {
            pass = scenarioCorruption(h);
        } else if (a.mode == "seq-gap") {
            pass = scenarioSeqGap(h);
        } else if (a.mode == "reconnect") {
            pass = scenarioReconnect(h);
        }
    }

    // 收尾：最后再 drain 一次保证统计完整。
    std::vector<uint8_t> chunk;
    while (h.queue.popAll(chunk)) {
            h.ep->onTransportData(chunk.data(), chunk.size());
    }
    h.ep->tick();
    closePort(h);
    printStats(h, nowMs() - t0, pass);
    return pass ? 0 : 1;
}
