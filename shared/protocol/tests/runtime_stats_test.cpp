// ESPView M4 — 运行态统计/诊断层宿主测试（runtime_stats / Heartbeat / RTT /
// FrameStats / PacketStats / InputStats 域）。
//
// 规范来源：M4 spec §21（Host Tests：ConnectionState / Heartbeat / Frame Stats /
//          Packet Stats / Input Stats）。
// 原则：wire bytes 由真实 MessageEncoder 生成；双端 harness 与
// protocol_endpoint_test.cpp 同模式（同一 shared/protocol 实现）。
// 纯 C++17，零平台依赖。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "input_event.h"
#include "input_manager.h"
#include "input_policy.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "runtime_stats.h"
#include "test_util.h"

namespace {

using espview::proto::DecoderError;
using espview::proto::DiagnosticsRing;
using espview::proto::EndpointConfig;
using espview::proto::FrameAssembler;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameType;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeFrameRect;
using espview::proto::makeHello;
using espview::proto::makePing;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::RttAggregate;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionError;
using espview::proto::SessionState;
using espview::proto::Severity;

// ---- 假时钟 ----
struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

// ---- 双端 harness（与 protocol_endpoint_test.cpp 同模式）----
struct EndpointHarness {
    FakeClock clock;
    std::vector<uint8_t> rx;
    std::vector<std::vector<uint8_t>> txPackets;
    std::vector<SessionState> states;
    std::vector<SessionError> protoErrors;
    std::unique_ptr<ProtocolEndpoint> ep;

    void init(const EndpointConfig& cfg, EndpointHarness* peer) {
        ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](SessionState s) { states.push_back(s); };
        cb.onProtocolError = [this](SessionError e, std::string_view) { protoErrors.push_back(e); };
        auto sink = [this, peer](const uint8_t* d, size_t n) {
            if (peer != nullptr) {
                peer->rx.insert(peer->rx.end(), d, d + n);
            }
            txPackets.emplace_back(d, d + n);
            return SendStatus::kOk;
        };
        ep = std::make_unique<ProtocolEndpoint>(cfg, sink, cb, [this]() { return clock.now; });
    }
};

void pump(EndpointHarness& h) {
    std::vector<uint8_t> data = std::move(h.rx);
    h.rx.clear();
    if (!data.empty()) {
        h.ep->onTransportData(data.data(), data.size());
    }
}

void connectPair(EndpointHarness& a, EndpointHarness& b) {
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    pump(a);
    pump(b);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

bool hasProtoError(const EndpointHarness& h, SessionError e) {
    return std::find(h.protoErrors.begin(), h.protoErrors.end(), e) != h.protoErrors.end();
}

// 独立编码器（seq 从 0 开始；与握手后基线一致）。
struct Feeder {
    SequenceCounter seq;
    MessageEncoder enc;
    Feeder() : enc(seq) {}
    // 编码单条消息并返回全部 packet bytes（供损坏/丢包测试）。
    std::vector<std::vector<uint8_t>> packets(const Message& msg) {
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(msg, pkts), PacketError::kNone);
        return pkts;
    }
};

// ---- 1. DiagnosticsRing：容量 / 顺序 / last / severity / clear ----
void diagnostics_ring() {
    std::printf("  diagnostics ring\n");
    DiagnosticsRing ring(3);
    CHECK_EQ(ring.capacity(), 3u);
    CHECK_EQ(ring.size(), 0u);
    CHECK(ring.last() == nullptr);

    ring.push(100, Severity::kInfo, "transport", "connected");
    ring.push(200, Severity::kWarning, "frame", "seq gap");
    ring.push(300, Severity::kError, "session", "timeout");
    CHECK_EQ(ring.size(), 3u);
    CHECK(ring.last() != nullptr);
    CHECK_EQ(ring.last()->timestampMs, 300u);
    CHECK_EQ(static_cast<unsigned>(ring.last()->severity), static_cast<unsigned>(Severity::kError));
    CHECK(ring.last()->source == std::string("session"));

    // 超出容量：最旧条目被淘汰，保持最近 N 条
    ring.push(400, Severity::kCritical, "transport", "compat");
    CHECK_EQ(ring.size(), 3u);
    auto items = ring.items();
    CHECK_EQ(items.size(), 3u);
    CHECK_EQ(items[0].timestampMs, 200u);
    CHECK_EQ(items[2].timestampMs, 400u);

    // severity toString
    CHECK(std::string(espview::proto::toString(Severity::kInfo)) == std::string("INFO"));
    CHECK(std::string(espview::proto::toString(Severity::kWarning)) == std::string("WARNING"));
    CHECK(std::string(espview::proto::toString(Severity::kError)) == std::string("ERROR"));
    CHECK(std::string(espview::proto::toString(Severity::kCritical)) == std::string("CRITICAL"));

    ring.clear();
    CHECK_EQ(ring.size(), 0u);
    CHECK(ring.last() == nullptr);
}

// ---- 2. RttAggregate：record/reset 聚合语义 ----
void rtt_aggregate() {
    std::printf("  rtt aggregate\n");
    RttAggregate r;
    CHECK(!r.lastMs.has_value());  // 无测量 ≠ 0ms
    CHECK_EQ(r.samples, 0u);

    r.record(10);
    r.record(30);
    r.record(20);
    CHECK(r.lastMs.has_value());
    CHECK_EQ(*r.lastMs, 20u);
    CHECK_EQ(r.samples, 3u);
    CHECK_EQ(r.minMs, 10u);
    CHECK_EQ(r.maxMs, 30u);
    CHECK_EQ(r.avgMs, 20u);  // (10+30+20)/3

    r.record(1);
    CHECK_EQ(r.minMs, 1u);
    CHECK_EQ(r.avgMs, 15u);  // 61/4

    r.reset();
    CHECK(!r.lastMs.has_value());
    CHECK_EQ(r.samples, 0u);
    CHECK_EQ(r.minMs, 0u);
    CHECK_EQ(r.maxMs, 0u);
    CHECK_EQ(r.avgMs, 0u);
}

// ---- 3. ConnectionState：disconnected → connecting → handshake → connected
//         → timeout → reconnect（RTT 每会话重置）----
void session_lifecycle() {
    std::printf("  connection state\n");
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);

    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    // a 的 HELLO 已入 b.rx；b 的 HELLO 已入 a.rx
    CHECK_EQ(a.ep->state(), SessionState::kConnecting);
    CHECK_EQ(b.ep->state(), SessionState::kConnecting);
    pump(a);  // kHandshake 为瞬时态：HELLO 验证通过即 kConnected
    pump(b);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);

    // 断线 → Disconnected + RTT 回到无测量
    a.clock.now = 2000;
    a.ep->tick();  // 发 PING
    pump(b);
    a.clock.now = 2050;
    pump(a);
    CHECK(a.ep->stats().rtt.lastMs.has_value());
    a.ep->onTransportDisconnected();
    b.ep->onTransportDisconnected();  // 双方会话终止（真实重连场景）
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);
    CHECK(!a.ep->stats().rtt.lastMs.has_value());
    CHECK_EQ(a.ep->stats().rtt.samples, 0u);

    // 重连 → 新会话 RTT 无测量：a 主动发 HELLO → b 被动恢复并回 HELLO
    a.ep->onTransportConnected();
    CHECK_EQ(a.ep->state(), SessionState::kConnecting);
    pump(b);  // b（DISCONNECTED）收到 HELLO → 被动恢复：回发 HELLO，进入握手
    pump(a);  // a 收到 b 的 HELLO → 双方 CONNECTED
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK(!a.ep->stats().rtt.lastMs.has_value());
}

// ---- 4. Heartbeat：ping/pong 计数 + RTT valid + lastPing/PongTime ----
void heartbeat_stats() {
    std::printf("  heartbeat ping/pong\n");
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);
    CHECK(!a.ep->stats().rtt.lastMs.has_value());  // 握手后无测量

    a.clock.now = 2000;
    a.ep->tick();
    CHECK_EQ(a.ep->stats().txPing, 1u);
    CHECK_EQ(a.ep->stats().lastPingTimeMs, 2000u);
    pump(b);
    CHECK_EQ(b.ep->stats().rxPing, 1u);
    CHECK_EQ(b.ep->stats().txPong, 1u);

    a.clock.now = 2030;
    pump(a);
    CHECK_EQ(a.ep->stats().rxPong, 1u);
    CHECK_EQ(a.ep->stats().lastPongTimeMs, 2030u);
    CHECK(a.ep->stats().rtt.lastMs.has_value());
    CHECK_EQ(*a.ep->stats().rtt.lastMs, 30u);
    CHECK_EQ(a.ep->stats().rtt.samples, 1u);

    // 第二拍：RTT 聚合 min/avg/max 更新
    a.clock.now = 4000;
    a.ep->tick();
    CHECK_EQ(a.ep->stats().txPing, 2u);
    pump(b);
    a.clock.now = 4020;
    pump(a);
    CHECK_EQ(a.ep->stats().rtt.samples, 2u);
    CHECK_EQ(a.ep->stats().rtt.minMs, 20u);
    CHECK_EQ(a.ep->stats().rtt.maxMs, 30u);
    CHECK_EQ(a.ep->stats().rtt.avgMs, 25u);
}

// ---- 5. Heartbeat：RTT unavailable（无 PONG 前 / 断线后）----
void heartbeat_unavailable() {
    std::printf("  heartbeat unavailable\n");
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    CHECK(!a.ep->stats().rtt.lastMs.has_value());  // 未连接：无测量
    connectPair(a, b);
    CHECK(!a.ep->stats().rtt.lastMs.has_value());  // 连接后未收到 PONG：仍无测量
    CHECK_EQ(a.ep->stats().rxPong, 0u);
    CHECK_EQ(a.ep->stats().lastPongTimeMs, 0u);
}

// ---- 6. Heartbeat：delayed pong（延迟但未超时 → RTT 大但有效）----
void delayed_pong() {
    std::printf("  delayed pong\n");
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    a.clock.now = 2000;
    a.ep->tick();  // PING 入 b.rx
    pump(b);       // b 立刻回 PONG（入 a.rx）
    // PONG 延迟 2.9s 投递（4.9s < 5s 对端超时窗口）：RTT 大但有效，不误判断线
    a.clock.now = 4900;
    pump(a);
    CHECK_EQ(a.ep->stats().rxPong, 1u);
    CHECK(a.ep->stats().rtt.lastMs.has_value());
    CHECK_EQ(*a.ep->stats().rtt.lastMs, 2900u);
    CHECK_EQ(a.ep->stats().heartbeatTimeouts, 0u);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
}

// ---- 7. Heartbeat：timeout（对端 5s 无响应 → 断开 + heartbeatTimeouts）----
void heartbeat_timeout() {
    std::printf("  heartbeat timeout\n");
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);

    a.clock.now = 2000;
    a.ep->tick();  // PING 发出（不 pump，对端无响应）
    CHECK_EQ(a.ep->stats().txPing, 1u);
    a.clock.now = 7000;  // 距握手 7s > 5s 对端超时
    a.ep->tick();
    CHECK_EQ(a.ep->stats().pingTimeouts, 1u);
    CHECK_EQ(a.ep->stats().heartbeatTimeouts, 1u);
    CHECK(hasProtoError(a, SessionError::kPeerTimeout));
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);
    CHECK(!a.ep->stats().rtt.lastMs.has_value());  // 断线后 RTT 无测量
}

// ---- 8. FrameStats：full commit / partial commit / partial-without-base /
//          aborted / per-reason discard 计数 ----
void frame_stats() {
    std::printf("  frame stats\n");
    FrameAssembler::Callbacks cb;  // 全部可选
    FrameAssembler fa(cb);

    // PARTIAL 无基准 → 不提交
    const auto px = std::vector<uint8_t>(200, 0x11);
    const auto pb = makeFrameBegin(1, FrameType::kPartial, PixelFormat::kRgb565, 320, 240, 0);
    const auto pr = makeFrameRect(0, 0, 10, 10, px.data(), px.size());
    CHECK(pb.has_value());
    CHECK(pr.has_value());
    fa.onMessage(*pb);
    fa.onMessage(*pr);
    fa.onMessage(makeFrameEnd(1, 1, 200, false));
    CHECK_EQ(fa.stats().discards(FrameDiscardReason::kPartialWithoutBase), 1u);
    CHECK_EQ(fa.stats().discardsTotal, 1u);
    CHECK_EQ(fa.stats().commits(), 0u);

    // FULL commit
    const auto fb = makeFrameBegin(2, FrameType::kFull, PixelFormat::kRgb565, 320, 240, 0);
    fa.onMessage(*fb);
    fa.onMessage(*pr);
    fa.onMessage(makeFrameEnd(2, 1, 200, false));
    CHECK_EQ(fa.stats().commitsFull, 1u);
    CHECK_EQ(fa.stats().commits(), 1u);
    CHECK_EQ(fa.stats().discardsTotal, 1u);

    // PARTIAL（有 FULL 基准）→ commit
    const auto pb2 = makeFrameBegin(3, FrameType::kPartial, PixelFormat::kRgb565, 320, 240, 0);
    fa.onMessage(*pb2);
    fa.onMessage(*pr);
    fa.onMessage(makeFrameEnd(3, 1, 200, false));
    CHECK_EQ(fa.stats().commitsPartial, 1u);
    CHECK_EQ(fa.stats().commits(), 2u);

    // ABORTED
    const auto fb2 = makeFrameBegin(4, FrameType::kFull, PixelFormat::kRgb565, 320, 240, 0);
    fa.onMessage(*fb2);
    fa.onMessage(*pr);
    fa.onMessage(makeFrameEnd(4, 1, 200, true));
    CHECK_EQ(fa.stats().discards(FrameDiscardReason::kAborted), 1u);
    CHECK_EQ(fa.stats().discardsTotal, 2u);

    // reset 时正在收帧 → kReset 计数
    const auto fb3 = makeFrameBegin(5, FrameType::kFull, PixelFormat::kRgb565, 320, 240, 0);
    fa.onMessage(*fb3);
    fa.reset();
    CHECK_EQ(fa.stats().discards(FrameDiscardReason::kReset), 1u);
    CHECK_EQ(fa.stats().discardsTotal, 3u);
}

// ---- 9. PacketStats：packetsOk / crcErrors（真实编码 → 损坏一字节）----
void packet_stats_crc() {
    std::printf("  packet stats crc\n");
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);

    Feeder f;
    auto ping0 = f.packets(makePing(100));  // seq=0
    auto ping1 = f.packets(makePing(200));  // seq=1
    CHECK_EQ(ping0.size(), 1u);
    CHECK_EQ(ping1.size(), 1u);

    // 损坏第一个包的一个 payload 字节 → CRC 失败
    std::vector<uint8_t> bad = ping0[0];
    bad[bad.size() - 1] ^= 0xFF;
    a.rx.insert(a.rx.end(), bad.begin(), bad.end());
    pump(a);
    CHECK_EQ(a.ep->stats().decoderErrors, 1u);
    CHECK_EQ(a.ep->stats().crcErrors, 1u);
    CHECK_EQ(a.ep->stats().packetsRx, 0u);  // CRC 失败不算通过包

    // 下一个合法包（seq=1，CRC 失败后基线重定位为 1）→ 通过
    a.rx.insert(a.rx.end(), ping1[0].begin(), ping1[0].end());
    pump(a);
    CHECK_EQ(a.ep->stats().packetsRx, 1u);
    CHECK_EQ(a.ep->stats().decoderErrors, 1u);  // 未新增
}

// ---- 10. PacketStats：seqGaps（丢弃首包 → 续包触发 seq 跳变）----
void packet_stats_seqgap() {
    std::printf("  packet stats seq gap\n");
    EndpointHarness a, b;
    a.init(EndpointConfig{}, &b);
    b.init(EndpointConfig{}, &a);
    connectPair(a, b);  // 握手后 decoder 基线 = 0（与 Feeder 一致）

    Feeder f;
    auto p0 = f.packets(makePing(100));  // seq=0 —— 故意丢弃
    auto p1 = f.packets(makePing(200));  // seq=1
    auto p2 = f.packets(makePing(300));  // seq=2
    (void)p0;
    // 握手已消耗：a 收到 b 的 HELLO（1 包 / 1 消息）
    const uint64_t rx0 = a.ep->stats().packetsRx;   // == 1
    const uint64_t msg0 = a.ep->stats().rxMessages; // == 1
    CHECK_EQ(rx0, 1u);
    CHECK_EQ(msg0, 1u);

    a.rx.insert(a.rx.end(), p1[0].begin(), p1[0].end());
    pump(a);
    CHECK_EQ(a.ep->stats().seqGaps, 1u);
    CHECK_EQ(a.ep->stats().decoderErrors, 1u);
    CHECK_EQ(a.ep->stats().packetsRx, rx0 + 1);  // CRC 通过（onPacket 计数），但被 seq 规则丢弃不派发
    CHECK_EQ(a.ep->stats().rxMessages, msg0);    // 未被派发

    // 基线重定位为 2 → 下一个包通过并派发
    a.rx.insert(a.rx.end(), p2[0].begin(), p2[0].end());
    pump(a);
    CHECK_EQ(a.ep->stats().packetsRx, rx0 + 2);
    CHECK_EQ(a.ep->stats().seqGaps, 1u);
    CHECK_EQ(a.ep->stats().rxMessages, msg0 + 1);  // seq=2 的 PING 正常派发
}

// ---- 11. InputStats：key / mouse / wheel / coalesced move / reconnect recovery ----
void input_stats() {
    std::printf("  input stats\n");
    using espview::input::InputEvent;
    using espview::input::InputManager;
    using espview::input::InputType;
    using espview::input::MouseMoveThrottle;

    InputManager mgr(320, 240);
    // key + wheel + mouse 序列
    InputEvent keyDown;
    keyDown.type = InputType::kKeyDown;
    keyDown.keycode = 0x04;
    keyDown.modifiers = 0;
    InputEvent keyUp = keyDown;
    keyUp.type = InputType::kKeyUp;
    mgr.feed(keyDown);
    mgr.feed(keyUp);
    CHECK_EQ(mgr.stats().eventsReceived, 2u);
    CHECK_EQ(mgr.stats().pressedKeys, 0u);

    InputEvent move;
    move.type = InputType::kMouseMove;
    move.x = 160;
    move.y = 120;
    move.buttons = espview::input::kMouseLeft;
    mgr.feed(move);  // buttons 0→1 → MouseDown 推导
    CHECK_EQ(mgr.stats().eventsReceived, 3u);
    CHECK_EQ(mgr.stats().pressedButtons, static_cast<uint8_t>(espview::input::kMouseLeft));

    InputEvent wheel;
    wheel.type = InputType::kMouseWheel;
    wheel.x = 160;
    wheel.y = 120;
    wheel.wheelDelta = 1;
    mgr.feed(wheel);
    CHECK_EQ(mgr.stats().eventsReceived, 4u);

    move.buttons = 0;
    mgr.feed(move);  // buttons 1→0 → MouseUp 推导
    CHECK_EQ(mgr.stats().eventsReceived, 5u);
    CHECK_EQ(mgr.stats().pressedButtons, 0u);

    // coalesced move：16ms 窗口内第二次 move 被合并
    MouseMoveThrottle throttle;
    uint16_t ox = 0, oy = 0;
    uint8_t ob = 0;
    CHECK(throttle.acceptMove(100, 10, 10, 0, ox, oy, ob));     // 首次立即发送
    CHECK(!throttle.acceptMove(105, 20, 20, 0, ox, oy, ob));    // 16ms 窗口内 coalesce
    CHECK(throttle.acceptMove(116, 30, 30, 0, ox, oy, ob));     // 超间隔 → 发送
    CHECK_EQ(static_cast<unsigned>(ox), 30u);

    // reconnect recovery：按下状态 → resetState 本地释放
    InputManager mgr2(320, 240);
    mgr2.feed(keyDown);
    InputEvent down;
    down.type = InputType::kMouseDown;
    down.x = 10;
    down.y = 10;
    down.buttons = espview::input::kMouseLeft;
    mgr2.feed(down);
    CHECK_EQ(mgr2.stats().pressedKeys, 1u);
    CHECK_EQ(mgr2.stats().pressedButtons, static_cast<uint8_t>(espview::input::kMouseLeft));
    mgr2.resetState();
    CHECK_EQ(mgr2.stats().resetCount, 1u);
    CHECK_EQ(mgr2.stats().stuckKeysReleased, 1u);
    CHECK_EQ(mgr2.stats().stuckButtonsReleased, 1u);
    CHECK_EQ(mgr2.stats().pressedKeys, 0u);
    CHECK_EQ(mgr2.stats().pressedButtons, 0u);
}

}  // namespace

void runRuntimeStatsTests() {
    std::printf("[runtime_stats]\n");
    diagnostics_ring();
    rtt_aggregate();
    session_lifecycle();
    heartbeat_stats();
    heartbeat_unavailable();
    delayed_pong();
    heartbeat_timeout();
    frame_stats();
    packet_stats_crc();
    packet_stats_seqgap();
    input_stats();
}
