// ESPView M6-A/M6-D — HostTcpTransport / TcpListener loopback host tests（§二十八 1-19）。
//
// 规范来源：M6-A 任务书 §二十八（host tests）+ §七（TCP 是 byte stream）+
//   §二十三（sendAll / short write）+ §九（单客户端 / BUSY）
//   + M6-D 任务书 §十七/§十八（HostTcpTransport remote-close 回归：
//   rxLoop 必须在锁外 setState(Disconnected)，否则 Worker pumpLoop 永不返回；
//   re-accept / stale state cleared）。
// 原则：
//   - 全部走 127.0.0.1 loopback（ctest 不依赖真实 Wi-Fi / 硬件）；
//   - 协议字节全部由 shared/protocol Encoder 产生，不手工拼接 wire bytes；
//   - 覆盖 Transport 语义（connect/disconnect/reconnect/partial/sticky/short
//     write/remote close/timeout/invalid address/BUSY）与 Protocol integration
//     （HELLO/PING-PONG/FULL/CHUNKED FULL/PARTIAL/CRC corruption/seq gap/
//     FULL resync/reconnect resync）。
// 平台：Windows + WinSock2（MinGW64 链接 ws2_32），纯 C++17，无 Qt / ESP-IDF。

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

#include "encoder.h"
#include "host_tcp_transport.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::pc::HostTcpTransport;
using espview::pc::IPcTransport;
using espview::pc::TcpListener;

using espview::proto::CommittedFrame;
using espview::proto::DecoderError;
using espview::proto::EndpointConfig;
using espview::proto::FrameType;
using espview::proto::HelloInfo;
using espview::proto::IMessagePayloadSource;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::Message;
using espview::proto::MessageHeader;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::RectInfo;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SessionState;

uint64_t steadyMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void sleepMs(uint64_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

bool waitUntil(uint64_t timeoutMs, const std::function<bool()>& cond) {
    const uint64_t start = steadyMs();
    while (steadyMs() - start < timeoutMs) {
        if (cond()) {
            return true;
        }
        sleepMs(5);
    }
    return cond();
}

// ---- Packet 级故障注入（测试 seam：Transport 字节流 → ProtocolEndpoint）----
// 按 Packet 边界解析字节流（MAGIC + decodeHeader），对第 targetIndex 个完整
// Packet（1-based）执行动作：丢弃整包 / 翻转 payload 内一个字节。用于
// CRC corruption / seq gap / FULL resync 测试；无故障时输出与原字节流一致。
class PacketFaultInjector {
public:
    enum class Action : uint8_t { kKeep = 0, kDrop = 1, kFlipPayloadByte = 2 };

    void setFault(size_t targetIndex, Action action, size_t flipOffset = 0,
                  uint8_t xorMask = 0xFF) {
        std::lock_guard<std::mutex> lk(m);
        targetIndex_ = targetIndex;
        action_ = action;
        flipOffset_ = flipOffset;
        xorMask_ = xorMask;
    }

    void feed(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(m);
        pending_.insert(pending_.end(), data, data + len);
        parse();
    }

    std::vector<uint8_t> take() {
        std::lock_guard<std::mutex> lk(m);
        std::vector<uint8_t> out;
        out.swap(out_);
        return out;
    }

    // 清空计数与缓冲（在 sendFull 前调用，确保 fault 目标 = 下一帧内第 N 个包）。
    void reset() {
        std::lock_guard<std::mutex> lk(m);
        pending_.clear();
        out_.clear();
        packetsSeen_ = 0;
    }

    size_t packetsSeen() const {
        std::lock_guard<std::mutex> lk(m);
        return packetsSeen_;
    }

private:
    mutable std::mutex m;
    void parse() {
        using espview::proto::decodeHeader;
        using espview::proto::kPacketHeaderSize;
        while (pending_.size() >= kPacketHeaderSize) {
            const bool magicOk = pending_[0] == 0x45 && pending_[1] == 0x53 &&
                                 pending_[2] == 0x50 && pending_[3] == 0x56;
            if (!magicOk) {
                // 防御：头被破坏（本测试不会发生，除非 fault 作用在 header）。
                out_.push_back(pending_[0]);
                pending_.erase(pending_.begin());
                continue;
            }
            PacketHeader h;
            const PacketError err = decodeHeader(pending_.data(), pending_.size(), &h);
            if (err == PacketError::kBufferTooSmall) {
                break;  // 包不完整：等待更多字节
            }
            if (err != PacketError::kNone) {
                out_.push_back(pending_[0]);
                pending_.erase(pending_.begin());
                continue;
            }
            const size_t total = kPacketHeaderSize + h.length;
            if (pending_.size() < total) {
                break;
            }
            ++packetsSeen_;
            bool keep = true;
            if (packetsSeen_ == targetIndex_) {
                if (action_ == Action::kDrop) {
                    keep = false;
                } else if (action_ == Action::kFlipPayloadByte && flipOffset_ < h.length) {
                    pending_[kPacketHeaderSize + flipOffset_] ^= xorMask_;
                }
            }
            if (keep) {
                out_.insert(out_.end(), pending_.begin(), pending_.begin() + total);
            }
            pending_.erase(pending_.begin(), pending_.begin() + total);
        }
    }

    std::vector<uint8_t> pending_;
    std::vector<uint8_t> out_;
    size_t packetsSeen_ = 0;
    size_t targetIndex_ = SIZE_MAX;
    Action action_ = Action::kKeep;
    size_t flipOffset_ = 0;
    uint8_t xorMask_ = 0xFF;
};

// ---- ServerSide：TcpListener + 已 accept 的 HostTcpTransport（§九）----
struct ServerSide {
    TcpListener listener;
    HostTcpTransport accepted;
    std::thread acceptThread;
    std::atomic<bool> acceptDone{false};
    bool acceptedOk = false;
    uint16_t port = 0;

    bool start() {
        TcpListener::Config cfg;
        cfg.bind = "127.0.0.1";
        cfg.port = 0;  // ephemeral（测试不固定端口）
        if (!listener.bindListen(cfg)) {
            return false;
        }
        port = listener.boundPort();
        acceptThread = std::thread([this]() {
            acceptedOk = listener.acceptOne(accepted);
            acceptDone.store(true);
        });
        return true;
    }

    bool waitAccept(uint64_t timeoutMs) {
        const bool done = waitUntil(timeoutMs, [this] { return acceptDone.load(); });
        return done && acceptedOk;
    }

    void acceptAgain() {
        if (acceptThread.joinable()) {
            acceptThread.join();
        }
        accepted.close();  // 清掉上一轮已断开/残留的 accepted socket
        acceptDone.store(false);
        acceptedOk = false;
        acceptThread = std::thread([this]() {
            acceptedOk = listener.acceptOne(accepted);
            acceptDone.store(true);
        });
    }

    void shutdown() {
        listener.cancel();  // 先唤醒 acceptOne（只置标志，不碰 socket）
        accepted.close();
        listener.close();
        if (acceptThread.joinable()) {
            acceptThread.join();
        }
    }
};

HostTcpTransport::Config clientCfg(uint16_t port) {
    HostTcpTransport::Config cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.connect_timeout_ms = 3000;
    cfg.rx_timeout_ms = 50;
    cfg.send_timeout_ms = 3000;
    return cfg;
}

// ---- RX 收集（Transport 级）----
struct RxCollector {
    std::mutex m;
    std::vector<uint8_t> buf;

    void wire(HostTcpTransport& t) {
        t.setDataCallback([this](const uint8_t* d, size_t n) {
            std::lock_guard<std::mutex> lk(m);
            buf.insert(buf.end(), d, d + n);
        });
    }

    std::vector<uint8_t> snapshot() {
        std::lock_guard<std::mutex> lk(m);
        return buf;
    }

    bool waitBytes(size_t min, uint64_t timeoutMs) {
        return waitUntil(timeoutMs, [this, min] {
            std::lock_guard<std::mutex> lk(m);
            return buf.size() >= min;
        });
    }
};

// ---- 帧像素公式（ESP32 侧产生 / PC 侧校验，两侧共用同一函数）----
uint8_t pixelByte(size_t off, uint16_t frameId, size_t rectIndex) {
    return static_cast<uint8_t>((off * 7u + frameId * 13u + rectIndex * 29u + 1u) & 0xFFu);
}

// ---- Protocol 侧 Peer（一侧一个 ProtocolEndpoint）----
struct Peer {
    HostTcpTransport& transport;  // 引用（属于 ServerSide.accepted 或本地 client）
    std::unique_ptr<ProtocolEndpoint> ep;
    PacketFaultInjector* injector = nullptr;  // 可空：PC 侧 RX 故障 seam

    std::mutex rxMutex;
    std::vector<uint8_t> rxBuf;

    bool connected = false;
    bool sawHello = false;
    uint64_t commits = 0;
    uint64_t discards = 0;
    CommittedFrame lastCommit{};
    std::vector<RectInfo> lastRects;
    std::vector<std::vector<uint8_t>> lastPixels;

    explicit Peer(HostTcpTransport& t) : transport(t) {}

    void wire() {
        transport.setDataCallback([this](const uint8_t* d, size_t n) {
            std::vector<uint8_t> chunk(d, d + n);
            if (injector != nullptr) {
                injector->feed(chunk.data(), chunk.size());
                chunk = injector->take();
                if (chunk.empty()) {
                    return;
                }
            }
            std::lock_guard<std::mutex> lk(rxMutex);
            rxBuf.insert(rxBuf.end(), chunk.begin(), chunk.end());
        });
        transport.setStateCallback([this](IPcTransport::State s) {
            if (s == IPcTransport::State::Disconnected || s == IPcTransport::State::Error) {
                if (ep) {
                    ep->onTransportDisconnected();
                }
            }
        });
    }

    void drain() {
        std::vector<uint8_t> chunk;
        {
            std::lock_guard<std::mutex> lk(rxMutex);
            if (!rxBuf.empty()) {
                chunk.swap(rxBuf);
            }
        }
        if (!chunk.empty() && ep) {
            ep->onTransportData(chunk.data(), chunk.size());
        }
        if (ep) {
            ep->tick();
        }
    }

    bool pumpUntil(uint64_t timeoutMs, const std::function<bool()>& cond) {
        const uint64_t start = steadyMs();
        while (steadyMs() - start < timeoutMs) {
            drain();
            if (cond()) {
                return true;
            }
            sleepMs(5);
        }
        drain();
        return cond();
    }
};

// 双端同时 drain（握手 / PING-PONG / 帧收发都需要两侧都 pump）。
bool pumpBoth(Peer& a, Peer& b, uint64_t timeoutMs, const std::function<bool()>& cond) {
    const uint64_t start = steadyMs();
    while (steadyMs() - start < timeoutMs) {
        a.drain();
        b.drain();
        if (cond()) {
            return true;
        }
        sleepMs(5);
    }
    a.drain();
    b.drain();
    return cond();
}

void initEndpoint(Peer& p, const EndpointConfig& cfg) {
    auto sink = [&p](const uint8_t* d, size_t n) -> SendStatus {
        return p.transport.send(d, n) ? SendStatus::kOk : SendStatus::kError;
    };
    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&p](SessionState s) {
        p.connected = (s == SessionState::kConnected);
    };
    cb.onHello = [&p](const HelloInfo&) { p.sawHello = true; };
    cb.onFrameBegin = [&p](const espview::proto::FrameBeginInfo&) {
        p.lastRects.clear();
        p.lastPixels.clear();
    };
    cb.onFrameRect = [&p](const RectInfo& r, const uint8_t* px, size_t n) {
        p.lastRects.push_back(r);
        p.lastPixels.emplace_back(px, px + n);
    };
    cb.onFrameCommit = [&p](const CommittedFrame& f) {
        p.lastCommit = f;
        ++p.commits;
    };
    cb.onFrameDiscard = [&p](espview::proto::FrameDiscardReason) { ++p.discards; };
    p.ep = std::make_unique<ProtocolEndpoint>(cfg, sink, cb, steadyMs);
}

EndpointConfig pcCfg() {
    EndpointConfig cfg;
    cfg.protocol_version = espview::proto::kProtocolVersion;
    cfg.width = 320;
    cfg.height = 240;
    cfg.pixel_format = PixelFormat::kRgb565;
    cfg.mode_mask = 0b1111;  // M7-C2：WINDOW|DEVICE|MIRROR|SPLIT
    cfg.device_name = "pc-test";
    cfg.ping_interval_ms = 200;  // 测试加速心跳（生产 2000ms）
    cfg.peer_timeout_ms = 2000;
    cfg.handshake_timeout_ms = 2000;
    return cfg;
}

EndpointConfig espCfg() {
    EndpointConfig cfg = pcCfg();
    cfg.device_name = "esp32-test";
    return cfg;
}

// ---- FRAME_RECT 流式载荷源（8B 矩形头 + 像素；不整段驻留）----
class RectSource : public IMessagePayloadSource {
public:
    RectSource(uint16_t x, uint16_t y, uint16_t w, uint16_t h, size_t pixelBytes,
               std::function<uint8_t(size_t)> byteAt)
        : x_(x), y_(y), w_(w), h_(h), pixelBytes_(pixelBytes), byteAt_(std::move(byteAt)) {}

    size_t read(uint8_t* dst, size_t maxBytes) override {
        const size_t total = 8 + pixelBytes_;
        size_t produced = 0;
        while (produced < maxBytes && offset_ < total) {
            if (offset_ < 8) {
                uint16_t v = 0;
                switch (offset_ / 2) {
                    case 0: v = x_; break;
                    case 1: v = y_; break;
                    case 2: v = w_; break;
                    default: v = h_; break;
                }
                dst[produced++] = static_cast<uint8_t>((offset_ & 1) ? (v >> 8) : (v & 0xFF));
            } else {
                dst[produced++] = byteAt_(offset_ - 8);
            }
            ++offset_;
        }
        return produced;
    }

private:
    uint16_t x_ = 0;
    uint16_t y_ = 0;
    uint16_t w_ = 0;
    uint16_t h_ = 0;
    size_t pixelBytes_ = 0;
    std::function<uint8_t(size_t)> byteAt_;
    size_t offset_ = 0;
};

// 发送 320x240 FULL：1 个 BEGIN + rectCount 个流式 RECT + 1 个 END。
// 每个 RECT payload = 8 + pixelBytes（>4096 时由 Encoder 自动 CHUNKED 拆包）。
bool sendFull(Peer& esp, uint16_t frameId, size_t rectCount) {
    constexpr uint16_t kW = 320;
    constexpr uint16_t kH = 240;
    const size_t rectPixelBytes = (static_cast<size_t>(kW) * kH * 2u) / rectCount;
    if (esp.ep->sendMessage(*makeFrameBegin(frameId, FrameType::kFull, PixelFormat::kRgb565,
                                            kW, kH, static_cast<uint32_t>(kW * kH * 2u))) !=
        SendResult::kOk) {
        return false;
    }
    const uint16_t rectH = static_cast<uint16_t>(kH / rectCount);
    for (size_t r = 0; r < rectCount; ++r) {
        RectSource src(0, static_cast<uint16_t>(r * rectH), kW, rectH, rectPixelBytes,
                       [frameId, r](size_t off) { return pixelByte(off, frameId, r); });
        MessageHeader hdr;
        hdr.type = static_cast<uint8_t>(MessageType::kFrameRect);
        hdr.flags = 0;
        if (esp.ep->sendMessageStreaming(hdr, src) != SendResult::kOk) {
            return false;
        }
    }
    return esp.ep->sendMessage(makeFrameEnd(frameId, static_cast<uint16_t>(rectCount),
                                            static_cast<uint32_t>(kW * kH * 2u), false)) ==
           SendResult::kOk;
}

bool sendPartial(Peer& esp, uint16_t frameId, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    const size_t pixelBytes = static_cast<size_t>(w) * h * 2u;
    if (esp.ep->sendMessage(*makeFrameBegin(frameId, FrameType::kPartial, PixelFormat::kRgb565,
                                            320, 240, static_cast<uint32_t>(pixelBytes))) !=
        SendResult::kOk) {
        return false;
    }
    RectSource src(x, y, w, h, pixelBytes,
                   [frameId](size_t off) { return pixelByte(off, frameId, 0); });
    MessageHeader hdr;
    hdr.type = static_cast<uint8_t>(MessageType::kFrameRect);
    hdr.flags = 0;
    if (esp.ep->sendMessageStreaming(hdr, src) != SendResult::kOk) {
        return false;
    }
    return esp.ep->sendMessage(makeFrameEnd(frameId, 1, static_cast<uint32_t>(pixelBytes),
                                            false)) == SendResult::kOk;
}

// 校验 PC 侧最近一次提交帧的 meta + 像素。
bool verifyFrame(Peer& pc, uint16_t frameId, FrameType ft, size_t rectCount,
                 size_t pixelTotal) {
    if (pc.lastCommit.frameId != frameId) {
        std::printf("  verifyFrame: frameId %u != %u\n", pc.lastCommit.frameId, frameId);
        return false;
    }
    if (static_cast<size_t>(pc.lastCommit.frameType) != static_cast<size_t>(ft)) {
        std::printf("  verifyFrame: frameType %u != %u\n",
                    static_cast<unsigned>(pc.lastCommit.frameType),
                    static_cast<unsigned>(ft));
        return false;
    }
    if (pc.lastRects.size() != rectCount) {
        std::printf("  verifyFrame: rects %zu != %zu\n", pc.lastRects.size(), rectCount);
        return false;
    }
    size_t total = 0;
    for (size_t r = 0; r < rectCount; ++r) {
        const size_t n = pc.lastPixels[r].size();
        total += n;
        for (size_t i = 0; i < n; ++i) {
            if (pc.lastPixels[r][i] != pixelByte(i, frameId, r)) {
                std::printf("  verifyFrame: pixel mismatch rect=%zu off=%zu\n", r, i);
                return false;
            }
        }
    }
    if (total != pixelTotal) {
        std::printf("  verifyFrame: pixelTotal %zu != %zu\n", total, pixelTotal);
        return false;
    }
    if (pc.lastCommit.byteCount != pixelTotal) {
        std::printf("  verifyFrame: byteCount %u != %zu\n",
                    static_cast<unsigned>(pc.lastCommit.byteCount), pixelTotal);
        return false;
    }
    return true;
}

// ===================== Transport 语义（§二十八 1-10）=====================
void runTransportTests() {
    std::printf("[tcp_transport] transport semantics\n");

    // ---- 1. connect + 2. disconnect ----
    {
        ServerSide srv;
        CHECK_MSG(srv.start(), "server bindListen");
        HostTcpTransport client;
        CHECK_MSG(client.open(clientCfg(srv.port)), "client open");
        CHECK_MSG(srv.waitAccept(3000), "server acceptOne");
        CHECK(client.isConnected());
        CHECK(srv.accepted.isConnected());

        // 2. disconnect：client close → server 侧 recv 0 → disconnected
        client.close();
        CHECK_MSG(waitUntil(3000, [&] { return !srv.accepted.isConnected(); }),
                  "server observes client disconnect");
        CHECK(!client.isConnected());

        // ---- 3. reconnect（§十一：断开 → 重新 accept）----
        srv.acceptAgain();
        CHECK_MSG(client.open(clientCfg(srv.port)), "client reconnect open");
        CHECK_MSG(srv.waitAccept(3000), "server re-accept");

        // ---- 10. BUSY：已有活跃客户端时 acceptOne 立即返回 false（§九）----
        CHECK(!srv.listener.acceptOne(srv.accepted));

        client.close();
        srv.shutdown();
    }

    // ---- 4. partial recv（一次 recv 只拿到包的一部分，最终全部到达）----
    {
        ServerSide srv;
        CHECK(srv.start());
        HostTcpTransport client;
        CHECK(client.open(clientCfg(srv.port)));
        CHECK(srv.waitAccept(3000));

        RxCollector clientRx;
        clientRx.wire(client);

        std::vector<uint8_t> msg(100);
        for (size_t i = 0; i < msg.size(); ++i) {
            msg[i] = static_cast<uint8_t>(i & 0xFF);
        }
        CHECK(srv.accepted.send(msg.data(), 60));  // 半个包
        sleepMs(50);
        CHECK(srv.accepted.send(msg.data() + 60, 40));  // 后半
        CHECK_MSG(clientRx.waitBytes(msg.size(), 3000), "client receives full message");
        const std::vector<uint8_t> got = clientRx.snapshot();
        CHECK_EQ(got.size(), msg.size());
        CHECK(std::memcmp(got.data(), msg.data(), msg.size()) == 0);

        client.close();
        srv.shutdown();
    }

    // ---- 5. sticky recv（一次 send 包含多个完整消息）----
    {
        ServerSide srv;
        CHECK(srv.start());
        HostTcpTransport client;
        CHECK(client.open(clientCfg(srv.port)));
        CHECK(srv.waitAccept(3000));

        RxCollector clientRx;
        clientRx.wire(client);

        std::vector<uint8_t> blob;
        for (int m = 0; m < 3; ++m) {
            for (int i = 0; i < 37; ++i) {
                blob.push_back(static_cast<uint8_t>(m * 37 + i));
            }
        }
        CHECK(srv.accepted.send(blob.data(), blob.size()));  // 一次 send 全部
        CHECK_MSG(clientRx.waitBytes(blob.size(), 3000), "client receives sticky blob");
        const std::vector<uint8_t> got = clientRx.snapshot();
        CHECK_EQ(got.size(), blob.size());
        CHECK(std::memcmp(got.data(), blob.data(), blob.size()) == 0);

        client.close();
        srv.shutdown();
    }

    // ---- 6. send short write（sendAll：4116B 完整包一次 send 必须全部送出）----
    {
        ServerSide srv;
        CHECK(srv.start());
        HostTcpTransport client;
        CHECK(client.open(clientCfg(srv.port)));
        CHECK(srv.waitAccept(3000));

        RxCollector serverRx;
        serverRx.wire(srv.accepted);

        std::vector<uint8_t> big(20 + 4096);  // mtu() == 4116
        for (size_t i = 0; i < big.size(); ++i) {
            big[i] = static_cast<uint8_t>((i * 3 + 7) & 0xFF);
        }
        CHECK_MSG(client.send(big.data(), big.size()), "sendAll 4116B");
        CHECK_MSG(serverRx.waitBytes(big.size(), 3000), "server receives 4116B");
        const std::vector<uint8_t> got = serverRx.snapshot();
        CHECK_EQ(got.size(), big.size());
        CHECK(std::memcmp(got.data(), big.data(), big.size()) == 0);

        client.close();
        srv.shutdown();
    }

    // ---- 7. remote close（server 关闭 → client 侧断开）----
    {
        ServerSide srv;
        CHECK(srv.start());
        HostTcpTransport client;
        CHECK(client.open(clientCfg(srv.port)));
        CHECK(srv.waitAccept(3000));
        srv.accepted.close();
        CHECK_MSG(waitUntil(3000, [&] { return !client.isConnected(); }),
                  "client observes remote close");
        client.close();
        srv.shutdown();
    }

    // ---- 8. timeout / refused（connect 到无人监听的端口）----
    {
        ServerSide tmp;
        CHECK(tmp.start());
        const uint16_t deadPort = tmp.port;
        tmp.shutdown();  // listener 关闭 → 端口不再监听
        HostTcpTransport c;
        CHECK_MSG(!c.open(clientCfg(deadPort)), "connect to closed port fails");
        CHECK(!c.isConnected());
    }

    // ---- 9. invalid address（getaddrinfo 失败）----
    // 用非法 IPv4 数值（256.x）：getaddrinfo 无法解析为地址且 DNS 不会命中 → 明确失败。
    {
        HostTcpTransport c;
        HostTcpTransport::Config cfg;
        cfg.host = "256.256.256.256";
        cfg.port = 8765;
        cfg.connect_timeout_ms = 1000;
        CHECK_MSG(!c.open(cfg), "invalid address fails");
        CHECK(!c.isConnected());
    }

    // ---- 11. M6-D §十七 回归：remote close → setState(Disconnected) 状态回调 ----
    // M6-C 缺陷：rxLoop 的 recv==0/select 错误只置 connected_=false，未调用
    // setState(Disconnected) → Worker pumpLoop 永不返回 → 不再 re-accept。
    // 本测试要求状态回调（不只是 isConnected()==false）在锁外触发。
    {
        ServerSide srv;
        CHECK(srv.start());
        std::atomic<int> serverStateCb{-1};  // -1=未收到, 0=Disconnected, 2=Connected
        // 回调必须在 accept 之前挂上（attach 的 Connected 回调在 accept 时触发）。
        srv.accepted.setStateCallback([&serverStateCb](IPcTransport::State s) {
            serverStateCb.store(static_cast<int>(s));
        });
        HostTcpTransport client;
        CHECK(client.open(clientCfg(srv.port)));
        CHECK(srv.waitAccept(3000));
        CHECK_MSG(waitUntil(3000, [&] { return serverStateCb.load() == 2; }),
                  "attach → Connected state callback");

        client.close();  // 对端（client）关闭 socket
        CHECK_MSG(waitUntil(3000, [&] { return serverStateCb.load() == 0; }),
                  "remote close → Disconnected state callback (M6-C regression)");
        CHECK(!srv.accepted.isConnected());
        client.close();
        srv.shutdown();
    }

    // ---- 12. M6-D §十八.12-14：remote close → re-accept → stale state cleared ----
    {
        ServerSide srv;
        CHECK(srv.start());
        std::atomic<int> stateCb{-1};
        srv.accepted.setStateCallback([&stateCb](IPcTransport::State s) {
            stateCb.store(static_cast<int>(s));
        });
        HostTcpTransport c1;
        CHECK(c1.open(clientCfg(srv.port)));
        CHECK(srv.waitAccept(3000));
        CHECK(waitUntil(3000, [&] { return stateCb.load() == 2; }));

        c1.close();  // 对端断开
        CHECK_MSG(waitUntil(3000, [&] { return stateCb.load() == 0; }),
                  "disconnect state callback before re-accept");

        // 重新 accept 新客户端：必须回到 Connected 状态回调 + 新会话数据可达
        //（不残留旧会话的 Disconnected 状态，即 stale state cleared）。
        srv.acceptAgain();
        HostTcpTransport c2;
        CHECK(c2.open(clientCfg(srv.port)));
        CHECK_MSG(srv.waitAccept(3000), "server re-accept new client");
        CHECK_MSG(waitUntil(3000, [&] { return stateCb.load() == 2; }),
                  "re-accept → Connected state callback (stale state cleared)");

        RxCollector serverRx;
        serverRx.wire(srv.accepted);
        const uint8_t blob[4] = {0xAA, 0xBB, 0xCC, 0xDD};
        CHECK_MSG(c2.send(blob, sizeof(blob)), "new session send");
        CHECK_MSG(serverRx.waitBytes(sizeof(blob), 3000), "new session data reaches server");
        c2.close();
        srv.shutdown();
    }

    // ---- 13. CS-1 回归：send 致命错误 → Disconnected 状态回调（pumpLoop 不悬挂）----
    // 服务端以 RST 关闭（SO_LINGER {on=1, linger=0}）→ 客户端下一次 send() 命中
    // WSAECONNRESET/EPIPE 致命路径 → 必须锁外 setState(Disconnected)（旧实现只置
    // connected_=false，rxLoop 空转、pumpLoop 永不返回）。主断言：致命 send 失败后
    // 3s 内状态回调必须到达 Disconnected（Windows 上 RST 与 rxLoop recv 的先后
    // 不完全确定，故允许 send 失败或后续 recv 两条路径触发，但回调必须到达）。
    {
        SOCKET ls = socket(AF_INET, SOCK_STREAM, 0);
        CHECK_MSG(ls != INVALID_SOCKET, "server listen socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK_MSG(bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
                  "server bind");
        CHECK_MSG(listen(ls, 1) == 0, "server listen");
        sockaddr_in local{};
        int localLen = sizeof(local);
        getsockname(ls, reinterpret_cast<sockaddr*>(&local), &localLen);
        const uint16_t port = ntohs(local.sin_port);

        std::thread serverThread([&ls]() {
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c != INVALID_SOCKET) {
                linger lg{};
                lg.l_onoff = 1;  // RST 关闭：丢弃未读数据，不产生 FIN
                lg.l_linger = 0;
                setsockopt(c, SOL_SOCKET, SO_LINGER,
                           reinterpret_cast<const char*>(&lg), sizeof(lg));
                closesocket(c);
            }
        });

        HostTcpTransport client;
        std::atomic<int> stateCb{-1};
        client.setStateCallback(
            [&stateCb](IPcTransport::State s) { stateCb.store(static_cast<int>(s)); });
        const bool opened = client.open(clientCfg(port));
        if (!opened) {
            closesocket(ls);  // 解除服务端 accept 阻塞，避免 join 悬挂
        }
        CHECK_MSG(opened, "client open");
        CHECK_MSG(waitUntil(3000, [&] { return stateCb.load() == 2; }),
                  "Connected state callback");
        if (stateCb.load() != 2) {
            closesocket(ls);
        }
        serverThread.join();
        closesocket(ls);

        // RST 已发出：send() 必须失败；随后状态回调必须到达 Disconnected。
        const uint8_t blob[4] = {0x11, 0x22, 0x33, 0x44};
        bool sawSendFail = false;
        for (int i = 0; i < 8 && !sawSendFail; ++i) {
            if (!client.send(blob, sizeof(blob))) {
                sawSendFail = true;
                break;
            }
            sleepMs(10);
        }
        CHECK(sawSendFail);  // RST 后 send 必须失败（致命路径被触发）
        CHECK_MSG(waitUntil(3000, [&] { return stateCb.load() == 0; }),
                  "send fatal error -> Disconnected state callback (CS-1 regression)");
        CHECK(!client.isConnected());
        client.close();
    }

    // ---- 14. 本地 close 后 send：返回 false 且不崩溃（幂等）----
    {
        ServerSide srv;
        CHECK(srv.start());
        HostTcpTransport client;
        CHECK(client.open(clientCfg(srv.port)));
        CHECK(srv.waitAccept(3000));
        client.close();
        const uint8_t b[4] = {1, 2, 3, 4};
        CHECK(!client.send(b, sizeof(b)));
        CHECK(!client.send(b, sizeof(b)));  // 重复调用安全
        srv.shutdown();
    }

    // ---- 15. 多次重连周期（同一 ServerSide 连续 3 轮 connect/accept/close）----
    {
        ServerSide srv;
        CHECK(srv.start());
        for (int cycle = 0; cycle < 3; ++cycle) {
            HostTcpTransport client;
            CHECK_MSG(client.open(clientCfg(srv.port)), "reconnect open cycle");
            CHECK_MSG(srv.waitAccept(3000), "server accept cycle");
            CHECK(client.isConnected());
            CHECK(srv.accepted.isConnected());
            const uint8_t b[4] = {static_cast<uint8_t>(cycle), 0xAA, 0xBB, 0xCC};
            CHECK_MSG(client.send(b, sizeof(b)), "cycle send");
            client.close();
            CHECK_MSG(waitUntil(3000, [&] { return !srv.accepted.isConnected(); }),
                      "server observes client close");
            if (cycle + 1 < 3) {
                srv.acceptAgain();  // 下一轮 accept 新客户端
            }
        }
        srv.shutdown();
    }
    std::printf("[tcp_transport] transport semantics OK\n");
}

// ===================== Protocol integration（§二十八 11-19）=====================
void runProtocolIntegration() {
    std::printf("[tcp_transport] protocol integration over loopback TCP\n");

    ServerSide srv;
    CHECK_MSG(srv.start(), "server bindListen");

    HostTcpTransport espTransport;
    Peer pc(srv.accepted);
    Peer esp(espTransport);
    initEndpoint(pc, pcCfg());
    initEndpoint(esp, espCfg());
    pc.wire();
    esp.wire();

    // ---- 11. connect + HELLO 握手（ESP32 角色主动；PC 角色被动）----
    CHECK(espTransport.open(clientCfg(srv.port)));
    CHECK_MSG(srv.waitAccept(3000), "server accepts ESP32");
    esp.ep->onTransportConnected();  // ESP32 角色：发起 HELLO
    CHECK_MSG(pumpBoth(esp, pc, 3000,
                       [&] { return esp.ep->isConnected() && pc.ep->isConnected(); }),
              "both CONNECTED");
    CHECK(pc.sawHello);
    CHECK(esp.sawHello);

    // ---- 12. PING/PONG（心跳加速 200ms；协议层自动回 PONG）----
    CHECK_MSG(pumpBoth(esp, pc, 3000,
                       [&] { return esp.ep->stats().rxPong > 0 && pc.ep->stats().rxPing > 0; }),
              "PING/PONG exchange");

    // ---- 13. FULL（153600B，10 个 RECT，每 RECT 15360B → CHUNKED 4 包）----
    CHECK_MSG(sendFull(esp, 1, 10), "send FULL frame 1");
    CHECK_MSG(pumpBoth(esp, pc, 5000, [&] { return pc.commits >= 1; }), "pc commits FULL");
    CHECK_MSG(verifyFrame(pc, 1, FrameType::kFull, 10, 153600), "FULL frame content");

    // ---- 14. CHUNKED FULL 明确验证：某 RECT 必须拆成多包 ----
    {
        const uint64_t packetsBefore = pc.ep->stats().packetsRx;
        CHECK_MSG(sendFull(esp, 7, 10), "send CHUNKED FULL frame 7");
        CHECK_MSG(pumpBoth(esp, pc, 5000, [&] { return pc.commits >= 2; }),
                  "pc commits CHUNKED FULL");
        CHECK_MSG(verifyFrame(pc, 7, FrameType::kFull, 10, 153600), "CHUNKED FULL content");
        // 10 个 RECT × 4 包 + BEGIN + END = 42 包（vs 单包消息 1 包）
        CHECK(pc.ep->stats().packetsRx - packetsBefore >= 42);
    }

    // ---- 15. PARTIAL（FULL 之后应用，1 个 100x50 RECT = 10000B → CHUNKED 3 包）----
    CHECK_MSG(sendPartial(esp, 2, 10, 10, 100, 50), "send PARTIAL frame 2");
    CHECK_MSG(pumpBoth(esp, pc, 5000, [&] { return pc.commits >= 3; }), "pc commits PARTIAL");
    CHECK_MSG(verifyFrame(pc, 2, FrameType::kPartial, 1, 10000), "PARTIAL content");

    // ---- 16. CRC corruption（翻转某 RECT payload 字节 → CRC 错误 → 帧作废）----
    // 故障窗口只 pump PC（不 drain ESP32，避免 PONG 干扰包序号计数）。
    {
        PacketFaultInjector inj;
        inj.setFault(2, PacketFaultInjector::Action::kFlipPayloadByte, 3, 0xFF);
        inj.reset();  // 计数从下一帧第一个包开始
        pc.injector = &inj;
        CHECK(sendFull(esp, 3, 10));  // 该帧第 2 个包 payload 被翻转
        CHECK_MSG(pc.pumpUntil(5000, [&] { return pc.ep->stats().crcErrors > 0; }),
                  "CRC error detected");
        pc.injector = nullptr;
        CHECK(pc.ep->stats().decoderErrors >= 1);
        // ---- 17. FULL resync：下一个未损坏 FULL 帧正常提交 ----
        const uint64_t commitsBefore = pc.commits;
        CHECK_MSG(sendFull(esp, 4, 10), "send FULL frame 4 after CRC fault");
        CHECK_MSG(pc.pumpUntil(5000, [&] { return pc.commits == commitsBefore + 1; }),
                  "FULL resync after CRC fault");
        CHECK_MSG(verifyFrame(pc, 4, FrameType::kFull, 10, 153600), "post-CRC FULL content");
        CHECK(pc.discards >= 1);  // 坏帧被作废
    }

    // ---- 18. seq gap（丢弃第 2 个包 → seq 跳变 → 帧作废 → 下帧恢复）----
    {
        PacketFaultInjector inj;
        inj.setFault(2, PacketFaultInjector::Action::kDrop);
        inj.reset();  // 计数从下一帧第一个包开始
        pc.injector = &inj;
        CHECK(sendFull(esp, 5, 10));
        CHECK_MSG(pc.pumpUntil(5000, [&] { return pc.ep->stats().seqGaps > 0; }),
                  "seq gap detected");
        pc.injector = nullptr;
        const uint64_t commitsBefore = pc.commits;
        CHECK_MSG(sendFull(esp, 6, 10), "send FULL frame 6 after seq gap");
        CHECK_MSG(pc.pumpUntil(5000, [&] { return pc.commits == commitsBefore + 1; }),
                  "FULL resync after seq gap");
        CHECK_MSG(verifyFrame(pc, 6, FrameType::kFull, 10, 153600), "post-gap FULL content");
    }

    // ---- 19. reconnect resync（client 断开 → 新会话 HELLO → FULL 重同步）----
    {
        espTransport.close();  // ESP32 侧断开
        CHECK_MSG(pc.pumpUntil(3000, [&] { return !pc.ep->isConnected(); }),
                  "pc session disconnected");
        // server 重新 accept；ESP32 重新连接并走完整握手
        srv.acceptAgain();
        CHECK_MSG(espTransport.open(clientCfg(srv.port)), "esp reconnect open");
        CHECK_MSG(srv.waitAccept(3000), "server re-accept after reconnect");
        esp.ep->onTransportConnected();
        CHECK_MSG(pumpBoth(esp, pc, 3000,
                           [&] { return esp.ep->isConnected() && pc.ep->isConnected(); }),
                  "both re-CONNECTED");
        const uint64_t commitsBefore = pc.commits;
        CHECK_MSG(sendFull(esp, 8, 10), "send FULL frame 8 after reconnect");
        CHECK_MSG(pumpBoth(esp, pc, 5000, [&] { return pc.commits == commitsBefore + 1; }),
                  "FULL commit after reconnect");
        CHECK_MSG(verifyFrame(pc, 8, FrameType::kFull, 10, 153600),
                  "post-reconnect FULL content");
        // 帧级统计随会话重置（DESIGN.md：握手完成 seq/帧状态清零）
        CHECK(pc.ep->frameStats().commits() >= 1);
    }

    espTransport.close();
    pc.transport.close();
    srv.shutdown();
    std::printf("[tcp_transport] protocol integration OK\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView host TCP loopback tests (M6-A) ==\n");
    runTransportTests();
    runProtocolIntegration();
    std::printf("----\nchecks: %d, failures: %d\n", espview::proto::test::gChecks,
                espview::proto::test::gFailures);
    return espview::proto::test::gFailures == 0 ? 0 : 1;
}
