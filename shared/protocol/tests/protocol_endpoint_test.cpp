// ESPView — ProtocolEndpoint Host Tests（M1-2）
//
// 规范来源：docs/DESIGN.md E 节（连接状态机 / ACK 语义 / 消息表）。
// 原则：
//   - 所有 wire bytes 均由真实 MessageEncoder 生成、由真实 StreamDecoder 消费；
//   - 双端 harness（A=PC, B=ESP32）通过 in-memory sink 互连，会话层代码与
//     ESP32/PC 完全一致（同一 shared/protocol 实现）；
//   - 故意损坏（翻转字节 / 跳包 / 穿插控制消息）仅在已编码的真实包上操作；
//   - 不手工拼接二进制协议。
// 纯 C++17，零平台依赖。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::CommittedFrame;
using espview::proto::DecoderError;
using espview::proto::DisplayMode;
using espview::proto::EndpointConfig;
using espview::proto::ErrorCode;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameType;
using espview::proto::HelloInfo;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeHello;
using espview::proto::makeFrameRect;
using espview::proto::makeMessage;
using espview::proto::makePing;
using espview::proto::makeSetMode;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::RectInfo;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionError;
using espview::proto::SessionState;

// ---- 假时钟 ----
struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct AckRecord {
    uint16_t seq = 0;
    uint8_t status = 0;
    ErrorCode code = ErrorCode::kNone;
};

// ---- 单端 harness ----
struct EndpointHarness {
    FakeClock clock;
    std::vector<uint8_t> rx;  // 对端发来、本端待消费
    std::vector<std::vector<uint8_t>> txPackets;  // 本端发出的包（逐包捕获，主 sink）
    std::vector<std::vector<uint8_t>> tryTxPackets;  // trySink 收到的包（tryTransmit 专用路径）
    bool blockSink = false;    // 模拟背压：sink 返回 kBackpressure 且不投递

    std::vector<SessionState> states;
    std::vector<HelloInfo> hellos;
    std::vector<SessionError> protoErrors;
    std::vector<FrameBeginInfo> begins;
    std::vector<CommittedFrame> commits;
    std::vector<FrameDiscardReason> discards;
    std::vector<std::pair<uint8_t, uint16_t>> ackRequests;  // (type, ackSeq)
    std::vector<AckRecord> acks;
    std::vector<uint16_t> ackTimeouts;
    std::vector<std::pair<ErrorCode, std::string>> errors;
    std::vector<Message> others;
    std::vector<DecoderError> decoderErrors;

    std::unique_ptr<ProtocolEndpoint> ep;

    // peer = 对端 harness（其 rx 接收本端发出的包）；nullptr 表示单端测试。
    // useTrySink：构造时提供独立 trySink（tryTransmit 专用；主 sink 恒背压时
    //   控制回复仍可达）。trySink 不受 blockSink 影响（语义：单次尝试、快速路径）。
    void init(const EndpointConfig& cfg, EndpointHarness* peer, bool useTrySink = false) {
        ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](SessionState s) { states.push_back(s); };
        cb.onProtocolError = [this](SessionError e, std::string_view d) {
            protoErrors.push_back(e);
            (void)d;
        };
        cb.onFrameBegin = [this](const FrameBeginInfo& b) { begins.push_back(b); };
        cb.onFrameCommit = [this](const CommittedFrame& f) { commits.push_back(f); };
        cb.onFrameDiscard = [this](FrameDiscardReason r) { discards.push_back(r); };
        cb.onHello = [this](const HelloInfo& h) { hellos.push_back(h); };
        cb.onAckRequest = [this](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
            ackRequests.emplace_back(t, s);
        };
        cb.onAck = [this](uint16_t s, uint8_t st, ErrorCode c) { acks.push_back({s, st, c}); };
        cb.onAckTimeout = [this](uint16_t s) { ackTimeouts.push_back(s); };
        cb.onError = [this](ErrorCode c, std::string_view t) {
            errors.emplace_back(c, std::string(t));
        };
        cb.onOtherMessage = [this](const Message& m) { others.push_back(m); };
        cb.onFrameRect = [](const RectInfo&, const uint8_t*, size_t) {};

        auto sink = [this, peer](const uint8_t* d, size_t n) {
            if (blockSink) {
                return SendStatus::kBackpressure;
            }
            if (peer != nullptr) {
                peer->rx.insert(peer->rx.end(), d, d + n);
            }
            txPackets.emplace_back(d, d + n);
            return SendStatus::kOk;
        };
        auto trySink = [this, peer](const uint8_t* d, size_t n) {
            if (peer != nullptr) {
                peer->rx.insert(peer->rx.end(), d, d + n);
            }
            tryTxPackets.emplace_back(d, d + n);
            return SendStatus::kOk;
        };
        if (useTrySink) {
            ep = std::make_unique<ProtocolEndpoint>(cfg, sink, trySink, cb,
                                                    [this]() { return clock.now; });
        } else {
            ep = std::make_unique<ProtocolEndpoint>(cfg, sink, cb, [this]() { return clock.now; });
        }
    }
};

// ---- 泵送：把本端 rx 喂给 decoder（可指定 chunk 模拟拆包）----
void pump(EndpointHarness& h, int chunk = 0) {
    std::vector<uint8_t> data = std::move(h.rx);
    h.rx.clear();
    if (data.empty()) {
        return;
    }
    if (chunk <= 0) {
        h.ep->onTransportData(data.data(), data.size());
        return;
    }
    for (size_t i = 0; i < data.size(); i += static_cast<size_t>(chunk)) {
        const size_t n = std::min(static_cast<size_t>(chunk), data.size() - i);
        h.ep->onTransportData(data.data() + i, n);
    }
}

bool hasProtoError(const EndpointHarness& h, SessionError e) {
    return std::find(h.protoErrors.begin(), h.protoErrors.end(), e) != h.protoErrors.end();
}

bool hasDiscard(const EndpointHarness& h, FrameDiscardReason r) {
    return std::find(h.discards.begin(), h.discards.end(), r) != h.discards.end();
}

// 双端连接（正常握手路径，断言双方进入 CONNECTED）。
void connectPair(EndpointHarness& a, EndpointHarness& b) {
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    pump(a);
    pump(b);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(a.ep->stats().txHello, 1u);
    CHECK_EQ(b.ep->stats().txHello, 1u);
    CHECK_EQ(a.ep->stats().rxHello, 1u);
    CHECK_EQ(b.ep->stats().rxHello, 1u);
}

// 独立编码器（字节级拼接测试用；seq 从 0 开始，与握手后基线一致）。
struct Feeder {
    SequenceCounter seq;
    MessageEncoder enc;
    Feeder() : enc(seq) {}
    void pushTo(EndpointHarness& to, const Message& msg) {
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(msg, pkts), PacketError::kNone);
        for (const auto& p : pkts) {
            to.rx.insert(to.rx.end(), p.begin(), p.end());
        }
    }
};

uint16_t seqOf(const std::vector<uint8_t>& p) {
    PacketHeader h;
    CHECK_EQ(decodeHeader(p.data(), p.size(), &h), PacketError::kNone);
    return h.seq;
}

uint8_t typeOf(const std::vector<uint8_t>& p) {
    PacketHeader h;
    CHECK_EQ(decodeHeader(p.data(), p.size(), &h), PacketError::kNone);
    return h.type;
}

uint8_t flagsOf(const std::vector<uint8_t>& p) {
    PacketHeader h;
    CHECK_EQ(decodeHeader(p.data(), p.size(), &h), PacketError::kNone);
    return h.flags;
}

size_t countTypeIn(const std::vector<std::vector<uint8_t>>& pkts, uint8_t type) {
    size_t n = 0;
    for (const auto& p : pkts) {
        if (typeOf(p) == type) {
            ++n;
        }
    }
    return n;
}

size_t countType(const EndpointHarness& h, uint8_t type) {
    return countTypeIn(h.txPackets, type);
}

// 10x10 RGB565 rect 像素（200 字节，值 0xAB）
std::vector<uint8_t> smallPixels() { return std::vector<uint8_t>(10u * 10u * 2u, 0xAB); }

std::vector<uint8_t> bigPixels(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint8_t>(i * 7);
    }
    return v;
}

}  // namespace

// 1. disconnected → hello → connected；握手后 packet.seq 清零
void session_connect_hello() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);

    a.ep->onTransportConnected();
    CHECK_EQ(a.ep->state(), SessionState::kConnecting);
    CHECK_EQ(a.ep->stats().txHello, 1u);

    b.ep->onTransportConnected();
    pump(a);  // b 的 HELLO → a
    pump(b);  // a 的 HELLO → b
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.hellos.size(), 1u);
    CHECK_EQ(b.hellos[0].protocol_version, espview::proto::kProtocolVersion);
    CHECK_EQ(b.hellos[0].width, 320u);
    CHECK_EQ(b.hellos[0].height, 240u);
    CHECK_EQ(b.hellos[0].mode_mask, 0b111u);

    // 握手后第一包 seq == 0（seq 已清零）
    CHECK_EQ(a.ep->sendMessage(makePing(1)), SendResult::kOk);
    CHECK(!a.txPackets.empty());
    CHECK_EQ(seqOf(a.txPackets.back()), 0u);
}

// 2. hello version mismatch → 双方断开，不建立会话
void hello_version_mismatch() {
    EndpointHarness a, b;
    EndpointConfig cfgA;
    cfgA.protocol_version = 1;
    EndpointConfig cfgB;
    cfgB.protocol_version = 2;
    a.init(cfgA, &b);
    b.init(cfgB, &a);
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    pump(a);
    pump(b);
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);
    CHECK_EQ(b.ep->state(), SessionState::kDisconnected);
    CHECK(hasProtoError(a, SessionError::kHelloVersionMismatch));
    CHECK(hasProtoError(b, SessionError::kHelloVersionMismatch));
    CHECK_EQ(a.ep->stats().errors, 1u);
    CHECK_EQ(b.ep->stats().errors, 1u);
}

// 2b. 非法 HELLO（nameLen 与 payload 长度不符）→ failSession 发生在 decoder 消息
//     回调内：不得重入 decoder_.reset()（ESP32 上会冻结/崩溃），reset 延后执行；
//     随后一个有效 HELLO 必须能被动恢复会话。
void invalid_hello_then_recovery() {
    EndpointHarness h;
    h.init(EndpointConfig{}, nullptr);
    h.ep->onTransportConnected();
    CHECK_EQ(h.ep->state(), SessionState::kConnecting);

    // 构造 nameLen=5 但 payload 为 9+7 字节的非法 HELLO（真实编码、合法包 CRC）。
    Feeder f;
    Message bad;
    bad.type = static_cast<uint8_t>(MessageType::kHello);
    bad.flags = 0;
    bad.payload = {1, 0, 0x40, 0x01, 0xf0, 0x00, 0x00, 0x07, 5};
    const char name7[] = "esptest";
    bad.payload.insert(bad.payload.end(), name7, name7 + 7);
    f.pushTo(h, bad);
    pump(h);
    CHECK_EQ(h.ep->state(), SessionState::kDisconnected);
    CHECK(hasProtoError(h, SessionError::kHelloInvalidLayout));
    CHECK_EQ(h.ep->stats().errors, 1u);
    CHECK_EQ(h.ep->stats().rxHello, 0u);

    // 有效 HELLO → 被动恢复（延后的 decoder reset 在本次 onTransportData 开头执行）。
    Feeder f2;
    const auto ok = makeHello(espview::proto::kProtocolVersion, 0, 320, 240,
                              PixelFormat::kRgb565, 0b111, "esptest");
    CHECK(ok.has_value());
    f2.pushTo(h, *ok);
    pump(h);
    CHECK_EQ(h.ep->state(), SessionState::kConnected);
    CHECK_EQ(h.ep->stats().rxHello, 1u);
    CHECK_EQ(h.ep->stats().txHello, 2u);  // 被动恢复重发本端 HELLO
    CHECK_EQ(h.hellos.size(), 1u);
}

// 3. duplicate hello：CONNECTED 后再次收到 HELLO → 重新确认，不重置会话
void duplicate_hello() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);
    CHECK_EQ(b.hellos.size(), 1u);

    CHECK_EQ(a.ep->sendHello(), SendResult::kOk);
    pump(b);
    CHECK_EQ(b.hellos.size(), 2u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->stats().rxHello, 2u);
    CHECK_EQ(b.ep->stats().errors, 0u);
}

// 4. ping/pong：PING → 自动 PONG；RTT 统计；PING 不进入 FrameAssembler
void ping_pong() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    a.clock.now = 2000;
    a.ep->tick();  // 心跳：发 PING
    CHECK_EQ(a.ep->stats().txPing, 1u);

    pump(b);  // b 收到 PING → 自动回 PONG
    CHECK_EQ(b.ep->stats().rxPing, 1u);
    CHECK_EQ(b.ep->stats().txPong, 1u);
    CHECK_EQ(b.commits.size(), 0u);  // PING 不进 FrameAssembler

    a.clock.now = 2030;
    pump(a);  // a 收到 PONG → RTT = 30ms
    CHECK_EQ(a.ep->stats().rxPong, 1u);
    CHECK(a.ep->stats().rtt.lastMs.has_value());
    CHECK_EQ(*a.ep->stats().rtt.lastMs, 30u);
    CHECK_EQ(a.ep->stats().rtt.samples, 1u);
    CHECK_EQ(a.ep->stats().rtt.minMs, 30u);
    CHECK_EQ(a.ep->stats().rtt.maxMs, 30u);
    CHECK_EQ(a.ep->stats().rtt.avgMs, 30u);
    CHECK_EQ(a.ep->stats().lastPongTimeMs, 2030u);
    CHECK_EQ(a.commits.size(), 0u);
}

// 5. ping timeout：对端 5s 无响应 → 超时断开
void ping_timeout() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    a.clock.now = 2000;
    a.ep->tick();  // 发出 PING（不 pump，对端无响应）
    CHECK_EQ(a.ep->stats().txPing, 1u);

    a.clock.now = 5000;
    a.ep->tick();  // 距最后收到对端消息 5s → 超时
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);
    CHECK_EQ(a.ep->stats().pingTimeouts, 1u);
    CHECK(hasProtoError(a, SessionError::kPeerTimeout));
}

// 附加：HELLO 丢失 → handshake 超时
void handshake_timeout() {
    EndpointHarness a;
    a.init(EndpointConfig{}, nullptr);
    a.ep->onTransportConnected();
    CHECK_EQ(a.ep->state(), SessionState::kConnecting);
    a.clock.now = 5000;
    a.ep->tick();
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);
    CHECK_EQ(a.ep->stats().handshakeTimeouts, 1u);
    CHECK(hasProtoError(a, SessionError::kHandshakeTimeout));
}

// 6. set_mode → ACK
void set_mode_ack() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    CHECK_EQ(a.ep->sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    CHECK_EQ(countType(a, static_cast<uint8_t>(MessageType::kSetMode)), 1u);
    CHECK_EQ(flagsOf(a.txPackets.back()) & espview::proto::kFlagAckReq,
             espview::proto::kFlagAckReq);

    pump(b);
    CHECK_EQ(b.ackRequests.size(), 1u);
    const uint16_t ackSeq = b.ackRequests[0].second;
    CHECK_EQ(b.ackRequests[0].first, static_cast<uint8_t>(MessageType::kSetMode));

    CHECK_EQ(b.ep->acknowledge(ackSeq, 0, ErrorCode::kNone), SendResult::kOk);
    CHECK_EQ(b.ep->stats().ackSent, 1u);
    pump(a);
    CHECK_EQ(a.acks.size(), 1u);
    CHECK_EQ(a.acks[0].seq, ackSeq);
    CHECK_EQ(a.acks[0].status, 0u);
    CHECK_EQ(a.acks[0].code, ErrorCode::kNone);
    CHECK_EQ(a.ep->stats().ackReceived, 1u);
}

// 7. set_mode ACK 重试：500ms 超时，最多重试 2 次（共 3 次发送），耗尽后 onAckTimeout
void set_mode_retry() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    CHECK_EQ(a.ep->sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    CHECK_EQ(countType(a, static_cast<uint8_t>(MessageType::kSetMode)), 1u);
    const uint16_t s1 = seqOf(a.txPackets.back());

    a.clock.now = 500;
    a.ep->tick();
    CHECK_EQ(a.ep->stats().ackRetries, 1u);
    const uint16_t s2 = seqOf(a.txPackets.back());

    a.clock.now = 1000;
    a.ep->tick();
    CHECK_EQ(a.ep->stats().ackRetries, 2u);
    const uint16_t s3 = seqOf(a.txPackets.back());

    a.clock.now = 1500;
    a.ep->tick();
    CHECK_EQ(a.ep->stats().ackFailures, 1u);
    CHECK_EQ(a.ackTimeouts.size(), 1u);
    CHECK_EQ(countType(a, static_cast<uint8_t>(MessageType::kSetMode)), 3u);
    CHECK(s1 != s2 && s2 != s3 && s1 != s3);  // 重试按 SequenceCounter 生成新 seq

    a.clock.now = 2500;
    a.ep->tick();  // 耗尽后不再重试
    CHECK_EQ(countType(a, static_cast<uint8_t>(MessageType::kSetMode)), 3u);
}

// 8. set_mode 非法 mode → ACK ERR + kInvalidParam
void set_mode_error() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    // 非法 mode=0xFF（ACCEPT_REQ 置位）
    CHECK_EQ(a.ep->sendMessage(makeMessage(static_cast<uint8_t>(MessageType::kSetMode),
                                           espview::proto::kFlagAckReq, {0xFF})),
             SendResult::kOk);
    pump(b);
    CHECK_EQ(b.ackRequests.size(), 1u);
    CHECK_EQ(b.ackRequests[0].second, seqOf(a.txPackets.back()));

    CHECK_EQ(b.ep->acknowledge(b.ackRequests[0].second, 1, ErrorCode::kInvalidParam),
             SendResult::kOk);
    pump(a);
    CHECK_EQ(a.acks.size(), 1u);
    CHECK_EQ(a.acks[0].status, 1u);
    CHECK_EQ(a.acks[0].code, ErrorCode::kInvalidParam);
    CHECK_EQ(a.ep->stats().ackReceived, 1u);
}

// 9. disconnect 清空 pending ACK：不重试、不误报超时
void disconnect_clears_pending_ack() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    CHECK_EQ(a.ep->sendMessage(makeSetMode(DisplayMode::kMirror)), SendResult::kOk);
    const size_t n = a.txPackets.size();
    a.ep->onTransportDisconnected();
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);

    a.clock.now = 600;
    a.ep->tick();
    CHECK_EQ(a.txPackets.size(), n);          // 无重试
    CHECK_EQ(a.ep->stats().ackFailures, 0u);  // 不误报超时
    CHECK_EQ(a.ackTimeouts.size(), 0u);
}

// 10. 重连必须重新 HELLO；11. 重连后等待 FULL 帧
void reconnect_requires_hello_waits_full() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    a.ep->onTransportDisconnected();
    b.ep->onTransportDisconnected();
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);
    CHECK_EQ(b.ep->state(), SessionState::kDisconnected);

    // 重连：双方重新 HELLO
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    pump(a);
    pump(b);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(a.ep->stats().txHello, 2u);
    CHECK_EQ(b.ep->stats().txHello, 2u);

    // 重连后无基准：PARTIAL 不得提交；随后 FULL 建立基准（单一 Feeder 保证 seq 连续）
    {
        Feeder f;
        f.pushTo(b, *makeFrameBegin(1, FrameType::kPartial, PixelFormat::kRgb565, 10, 10, 200));
        f.pushTo(b, *makeFrameRect(0, 0, 10, 10, smallPixels().data(), smallPixels().size()));
        f.pushTo(b, makeFrameEnd(1, 1, 200, false));
        pump(b);
        CHECK_EQ(b.commits.size(), 0u);
        CHECK(hasDiscard(b, FrameDiscardReason::kPartialWithoutBase));

        f.pushTo(b, *makeFrameBegin(2, FrameType::kFull, PixelFormat::kRgb565, 10, 10, 200));
        f.pushTo(b, *makeFrameRect(0, 0, 10, 10, smallPixels().data(), smallPixels().size()));
        f.pushTo(b, makeFrameEnd(2, 1, 200, false));
        pump(b);
        CHECK_EQ(b.commits.size(), 1u);
        CHECK_EQ(b.commits[0].frameId, 2u);
        CHECK_EQ(b.commits[0].frameType, FrameType::kFull);
    }
}
void crc_error_keeps_session() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    CHECK_EQ(a.ep->sendMessage(makePing(7)), SendResult::kOk);
    CHECK(!b.rx.empty());
    b.rx[20] ^= 0xFF;  // 翻转第一个 payload 字节（LENGTH 不变 → CRC 必失败）
    pump(b);
    CHECK_EQ(b.ep->stats().decoderErrors, 1u);
    CHECK_EQ(b.ep->stats().rxPing, 0u);  // 坏包不派发
    CHECK_EQ(b.ep->state(), SessionState::kConnected);

    // 下一个合法包正常恢复
    CHECK_EQ(a.ep->sendMessage(makePing(8)), SendResult::kOk);
    pump(b);
    CHECK_EQ(b.ep->stats().rxPing, 1u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

// 13. seq gap：当前消息/帧作废，但不伪造断开；下一个 FULL 帧恢复
void seq_gap_resets_frame() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    // 帧 1：BEGIN + (丢 RECT) + END —— 单编码器保证 seq 连续（0,1,2）
    Feeder f;
    {
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(f.enc.encode(*makeFrameBegin(1, FrameType::kFull, PixelFormat::kRgb565, 10, 10,
                                              200),
                              pkts),
                 PacketError::kNone);
        b.rx.insert(b.rx.end(), pkts[0].begin(), pkts[0].end());  // BEGIN seq0
        pkts.clear();
        CHECK_EQ(f.enc.encode(*makeFrameRect(0, 0, 10, 10, smallPixels().data(),
                                             smallPixels().size()),
                              pkts),
                 PacketError::kNone);
        // RECT seq1 故意不写
        pkts.clear();
        CHECK_EQ(f.enc.encode(makeFrameEnd(1, 1, 200, false), pkts), PacketError::kNone);
        b.rx.insert(b.rx.end(), pkts[0].begin(), pkts[0].end());  // END seq2
    }
    pump(b);
    CHECK_EQ(b.commits.size(), 0u);
    CHECK(hasDiscard(b, FrameDiscardReason::kStreamError));
    CHECK_EQ(b.ep->state(), SessionState::kConnected);  // 不伪造断开

    // 帧 2：完整 FULL（seq 3,4,5 连续）→ 恢复提交
    f.pushTo(b, *makeFrameBegin(2, FrameType::kFull, PixelFormat::kRgb565, 10, 10, 200));
    f.pushTo(b, *makeFrameRect(0, 0, 10, 10, smallPixels().data(), smallPixels().size()));
    f.pushTo(b, makeFrameEnd(2, 1, 200, false));
    pump(b);
    CHECK_EQ(b.commits.size(), 1u);
    CHECK_EQ(b.commits[0].frameId, 2u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

// 14. 控制消息不得插入 CHUNKED Message 内部（Decoder 报 kChunkViolation，消息作废）
void control_inside_chunked_message() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    // 构造：chunk1(seq0, CHUNKED=1) + PING(seq1) + chunk2(seq2, CHUNKED=0)
    // seq 全部连续（真实发送方编码顺序），因此 PING 能通过 seq 检查、
    // 在组装中被判定为 kChunkViolation：CHUNKED 消息作废，PING 作为独立消息派发。
    {
        // 大 RECT：5000 字节像素 → 2 个 CHUNKED 包（4096 + 904）
        const auto pixels = bigPixels(5000);
        const auto rect = makeFrameRect(0, 0, 50, 50, pixels.data(), pixels.size());
        CHECK(rect.has_value());

        // encA：chunk1 = seq0
        SequenceCounter seqA(0);
        MessageEncoder encA(seqA);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(encA.encode(*rect, pkts), PacketError::kNone);
        CHECK_EQ(pkts.size(), 2u);
        b.rx.insert(b.rx.end(), pkts[0].begin(), pkts[0].end());  // chunk1 seq0 CHUNKED=1

        // encB：PING = seq1（与 chunk1 连续）
        SequenceCounter seqB(1);
        MessageEncoder encB(seqB);
        pkts.clear();
        CHECK_EQ(encB.encode(makePing(1234), pkts), PacketError::kNone);
        b.rx.insert(b.rx.end(), pkts[0].begin(), pkts[0].end());  // PING seq1

        // encC：chunk2 = seq2，用单包 RECT（200B）作为末片（CHUNKED=0）
        SequenceCounter seqC(2);
        MessageEncoder encC(seqC);
        pkts.clear();
        CHECK_EQ(encC.encode(*makeFrameRect(0, 0, 10, 10, smallPixels().data(),
                                            smallPixels().size()),
                             pkts),
                 PacketError::kNone);
        CHECK_EQ(pkts.size(), 1u);
        b.rx.insert(b.rx.end(), pkts[0].begin(), pkts[0].end());  // chunk2 seq2 CHUNKED=0
    }
    pump(b);
    // CHUNKED 消息被作废；PING 作为独立消息派发（未进入 CHUNKED 消息）；
    // 会话保持 CONNECTED（协议违规不伪造断开）。
    CHECK_EQ(b.ep->stats().rxPing, 1u);
    CHECK_EQ(b.ep->stats().decoderErrors, 1u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.commits.size(), 0u);
}

// 15. PARTIAL 无基准不提交；16. FULL 建立基准后 PARTIAL 可提交
void partial_requires_full_base() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    // 帧 1：PARTIAL（无基准）→ 不提交；帧 2：FULL → 提交；帧 3：PARTIAL → 提交
    // （单一 Feeder 保证 seq 连续）
    {
        Feeder f;
        f.pushTo(b, *makeFrameBegin(1, FrameType::kPartial, PixelFormat::kRgb565, 10, 10, 200));
        f.pushTo(b, *makeFrameRect(0, 0, 10, 10, smallPixels().data(), smallPixels().size()));
        f.pushTo(b, makeFrameEnd(1, 1, 200, false));
        pump(b);
        CHECK_EQ(b.commits.size(), 0u);
        CHECK(hasDiscard(b, FrameDiscardReason::kPartialWithoutBase));

        f.pushTo(b, *makeFrameBegin(2, FrameType::kFull, PixelFormat::kRgb565, 10, 10, 200));
        f.pushTo(b, *makeFrameRect(0, 0, 10, 10, smallPixels().data(), smallPixels().size()));
        f.pushTo(b, makeFrameEnd(2, 1, 200, false));
        pump(b);
        CHECK_EQ(b.commits.size(), 1u);
        CHECK_EQ(b.commits[0].frameId, 2u);
        CHECK_EQ(b.commits[0].frameType, FrameType::kFull);

        f.pushTo(b, *makeFrameBegin(3, FrameType::kPartial, PixelFormat::kRgb565, 10, 10, 200));
        f.pushTo(b, *makeFrameRect(0, 0, 10, 10, smallPixels().data(), smallPixels().size()));
        f.pushTo(b, makeFrameEnd(3, 1, 200, false));
        pump(b);
        CHECK_EQ(b.commits.size(), 2u);
        CHECK_EQ(b.commits[1].frameId, 3u);
        CHECK_EQ(b.commits[1].frameType, FrameType::kPartial);
    }

}
// 附加：DISCONNECTED 后收到 HELLO → 被动恢复会话（DESIGN.md：ESP32 等待对端 HELLO）
void passive_hello_recovery() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    // b 侧 5s 无对端消息 → 超时断开
    b.clock.now = 5000;
    b.ep->tick();
    CHECK_EQ(b.ep->state(), SessionState::kDisconnected);
    CHECK_EQ(b.ep->stats().pingTimeouts, 1u);

    // 对端重新发 HELLO（无 Transport 事件）→ b 被动恢复
    CHECK_EQ(a.ep->sendHello(), SendResult::kOk);
    pump(b);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->stats().rxHello, 2u);
    CHECK_EQ(b.ep->stats().txHello, 2u);  // 恢复时重新发送了本端 HELLO

    // 恢复后控制面正常。握手时序竞态说明：a 侧已 CONNECTED，把 b 重发的 HELLO
    // 视为"重复确认"而不复位 seq；因此 b（已复位 seq=0）的首个包会被 a 以
    // seq-gap 自愈丢弃（协议设计行为），第二包（seq 连续）必然到达。
    CHECK_EQ(b.ep->sendMessage(makePing(9)), SendResult::kOk);
    pump(a);
    CHECK_EQ(a.ep->stats().rxPing, 0u);  // 首包自愈丢弃
    CHECK_EQ(b.ep->sendMessage(makePing(10)), SendResult::kOk);
    pump(a);
    CHECK_EQ(a.ep->stats().rxPing, 1u);  // 自愈后必达
    CHECK_EQ(a.ep->stats().txPong, 1u);  // a 自动回 PONG
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

// 回归（M3 reconnect 暴露）：断线前已发出数据（PING/输入）消耗了本端 seq，
// 重连时主动 HELLO 必须从 seq=0 开始——对端 failSession/断线后 decoder reset
// （expectedSeq=0），首个包 seq != 0 会被当作 seq 跳变丢弃，被动恢复永远无法完成。
void reconnect_after_traffic_restarts_seq() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    // a 侧发出多条消息，消耗 seq（模拟 PING/输入事件）。
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(a.ep->sendMessage(makePing(static_cast<uint64_t>(i))), SendResult::kOk);
    }
    pump(b);
    CHECK_EQ(b.ep->stats().rxPing, 5u);

    // 双方断开：a 走 Transport 断线；b 走对端超时（与 ESP32 真实路径一致）。
    a.ep->onTransportDisconnected();
    a.rx.clear();  // 断线丢弃未送达字节（真实 Transport 关闭会清空缓冲）
    b.clock.now = 5000;
    b.ep->tick();
    CHECK_EQ(b.ep->state(), SessionState::kDisconnected);
    CHECK_EQ(b.ep->stats().pingTimeouts, 1u);

    // b 保持 DISCONNECTED 被动等待（ESP32 模式）；a 重新发起 HELLO。
    a.ep->onTransportConnected();  // a 的 HELLO -> b.rx
    pump(b);                      // b 被动恢复并回复 HELLO -> a.rx
    pump(a);                      // a 收到回复 HELLO -> 握手完成
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(a.ep->stats().txHello, 2u);
    CHECK_EQ(b.ep->stats().rxHello, 2u);
    CHECK_EQ(b.ep->stats().txHello, 2u);

    // 重连后控制面立即可用（无自愈丢弃：双方基线均为 0）。
    CHECK_EQ(b.ep->sendMessage(makePing(42)), SendResult::kOk);
    pump(a);
    CHECK_EQ(a.ep->stats().rxPing, 1u);
}

// 附加：ERROR 消息收发
void error_message() {
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    CHECK_EQ(a.ep->sendError(ErrorCode::kBusy, "busy now"), SendResult::kOk);
    pump(b);
    CHECK_EQ(b.errors.size(), 1u);
    CHECK_EQ(b.errors[0].first, ErrorCode::kBusy);
    CHECK(b.errors[0].second == "busy now");
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}


// M4：tryTransmit（PONG 回复、心跳 PING）必须走非阻塞 trySink，绝不触碰主 sink。
//   主 sink 恒背压模拟大帧流式发送占用 TX 缓冲；trySink 单次尝试成功。
//   回归目标：ESP32 大帧期间 RX 任务/会话 tick 的控制回复不得进入 paced sink
//   的重试循环（否则阻塞 RX 读取 → 输入丢包 + 帧流停滞 → 对端误判 5s 超时）。
void try_sink_used_for_control_replies() {
    EndpointHarness h;
    h.init(EndpointConfig{}, nullptr, /*useTrySink=*/true);
    h.blockSink = true;  // 主 sink 恒背压（模拟 TX 缓冲满）
    h.ep->onTransportConnected();  // HELLO 经主 sink 背压失败，状态仍 kConnecting
    CHECK_EQ(h.ep->state(), SessionState::kConnecting);

    // 喂入对端 HELLO（真实编码）→ 握手完成（不依赖本端发送成功）。
    Feeder f;
    const auto helloMsg = makeHello(espview::proto::kProtocolVersion, 0, 320, 240,
                                           PixelFormat::kRgb565, 0b111, "peer");
    CHECK(helloMsg.has_value());
    f.pushTo(h, *helloMsg);
    pump(h);
    CHECK_EQ(h.ep->state(), SessionState::kConnected);
    CHECK_EQ(h.ep->stats().rxHello, 1u);

    // 收到 PING → PONG 必须经 trySink 发出（主 sink 背压不影响控制回复）。
    // 注意：握手后 decoder 基线归零，PING 必须用新的 Feeder（seq 从 0 开始）。
    Feeder f2;
    f2.pushTo(h, makePing(12345));
    pump(h);
    CHECK_EQ(h.ep->stats().rxPing, 1u);
    CHECK_EQ(h.ep->stats().txPong, 1u);
    CHECK_EQ(countTypeIn(h.tryTxPackets, static_cast<uint8_t>(MessageType::kPong)), 1u);
    CHECK_EQ(countTypeIn(h.txPackets, static_cast<uint8_t>(MessageType::kPong)), 0u);

    // 心跳 PING（tick）同样走 trySink。
    h.clock.now = 2000;
    h.ep->tick();
    CHECK_EQ(h.ep->stats().txPing, 1u);
    CHECK_EQ(countTypeIn(h.tryTxPackets, static_cast<uint8_t>(MessageType::kPing)), 1u);
    CHECK_EQ(countTypeIn(h.txPackets, static_cast<uint8_t>(MessageType::kPing)), 0u);
}


void runProtocolEndpointTests() {
    std::printf("  session_connect_hello\n");
    session_connect_hello();
    std::printf("  hello_version_mismatch\n");
    hello_version_mismatch();
    std::printf("  invalid_hello_then_recovery\n");
    invalid_hello_then_recovery();
    std::printf("  duplicate_hello\n");
    duplicate_hello();
    std::printf("  ping_pong\n");
    ping_pong();
    std::printf("  ping_timeout\n");
    ping_timeout();
    std::printf("  handshake_timeout\n");
    handshake_timeout();
    std::printf("  set_mode_ack\n");
    set_mode_ack();
    std::printf("  set_mode_retry\n");
    set_mode_retry();
    std::printf("  set_mode_error\n");
    set_mode_error();
    std::printf("  disconnect_clears_pending_ack\n");
    disconnect_clears_pending_ack();
    std::printf("  reconnect_requires_hello_waits_full\n");
    reconnect_requires_hello_waits_full();
    std::printf("  crc_error_keeps_session\n");
    crc_error_keeps_session();
    std::printf("  seq_gap_resets_frame\n");
    seq_gap_resets_frame();
    std::printf("  control_inside_chunked_message\n");
    control_inside_chunked_message();
    std::printf("  partial_requires_full_base\n");
    partial_requires_full_base();
    std::printf("  passive_hello_recovery\n");
    passive_hello_recovery();
    std::printf("  reconnect_after_traffic_restarts_seq\n");
    reconnect_after_traffic_restarts_seq();
    std::printf("  error_message\n");
    error_message();
    std::printf("  try_sink_used_for_control_replies\n");
    try_sink_used_for_control_replies();
}



