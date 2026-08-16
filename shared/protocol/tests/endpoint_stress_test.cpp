// ESPView M8-A2 — ProtocolEndpoint 多角色压测（endpoint_stress_test.cpp）。
//
// 真线程多角色（RX feeder / ticker / app sender / stats reader）压测，
// 结果必须可判定（精确计数、无 0 判定模糊）：
//   S1  bulk 控制面：A 发 10000 条 kInputKey（无 ACK_REQ）→ B 精确收 10001 条
//       （含握手 HELLO）；双向 decoderErrors==0、seqGaps==0；payload 逐条恢复
//       （Σi = N(N-1)/2）；txMessages 精确；终态 Connected。
//   S2  PING→PONG RTT：conductor 驱动 50 轮，rtt.samples == rxPong == 50。
//   S3  ACK 往返：B 在 onAckRequest 内自动 acknowledge；A ackReceived==N、
//       onAck>=1、ackRetries==0、ackFailures==0；最终 tick 无悬挂 pending。
//   S4  reconnect 周期（epoch 换代，双侧断开）：20 轮断开/重连握手，
//       txHello 精确累计、decoderErrors==0、无 stale 槽泄漏、SET_MODE 往返成功。
// 每阶段硬看门狗上限（30s）。纯 C++17；CHECK 宏线程安全。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "endpoint_harness.h"
#include "message.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_sync.h"
#include "test_util.h"

namespace {

using espview::proto::DisplayMode;
using espview::proto::Message;
using espview::proto::MessageType;
using espview::proto::SendResult;
using espview::proto::SessionState;

void openAllGates(espview::proto::test::CoordinatedHarness& h) {
    h.feedGateA.open();
    h.feedGateB.open();
    h.connectGateA.open();
    h.disconnectGateA.open();
    h.tickGateA.open();
    h.failGateA.open();
    h.sinkHoldA.open();
    h.sinkHoldB.open();
}

Message bulkMessage(uint32_t i) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(i & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 8) & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 16) & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 24) & 0xFFu));
    return espview::proto::makeMessage(static_cast<uint8_t>(MessageType::kInputKey), 0,
                                       std::move(payload));
}

// S1：bulk 控制面（真线程多角色；无 ACK_REQ → 计数精确）。
void stress_bulk_control_plane() {
    constexpr int kMessages = 10000;
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    // 压测阶段禁心跳/ACK 重试/对端超时：保持计数精确且不会中途 failSession。
    h.cfgA.ping_interval_ms = 1000000;
    h.cfgB.ping_interval_ms = 1000000;
    h.cfgA.peer_timeout_ms = 1000000;
    h.cfgB.peer_timeout_ms = 1000000;
    h.cfgA.ack_timeout_ms = 1000000;
    std::atomic<uint64_t> payloadSum{0};    // 收到的 index 之和（payload 恢复）
    std::atomic<uint64_t> payloadCount{0};
    h.cbB.onOtherMessage = [&payloadSum, &payloadCount](const Message& m) {
        if (m.payload.size() < 4) {
            return;
        }
        const uint32_t idx =
            static_cast<uint32_t>(m.payload[0]) |
            (static_cast<uint32_t>(m.payload[1]) << 8) |
            (static_cast<uint32_t>(m.payload[2]) << 16) |
            (static_cast<uint32_t>(m.payload[3]) << 24);
        payloadSum.fetch_add(idx, std::memory_order_relaxed);
        payloadCount.fetch_add(1, std::memory_order_relaxed);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    espview::proto::test::Gate go;
    h.startSender(true, [i = 0]() mutable { return bulkMessage(i++); }, kMessages, go);
    h.startFeederB();   // b2a_ 无流量，但保持"RX 角色"存在
    h.startFeederA();   // 消费 b2a_（B 侧无回复 → 空转，无影响）
    h.startTicker(1);   // 低频时钟推进（心跳已禁；驱动半包超时路径）
    h.startStatsReader();

    go.open();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (h.b().stats().rxMessages < static_cast<uint64_t>(kMessages + 1)) {
        if (std::chrono::steady_clock::now() > end) {
            break;  // 硬看门狗
        }
        std::this_thread::yield();
    }
    h.stopAll();
    CHECK_EQ(h.b().stats().rxMessages, static_cast<uint64_t>(kMessages + 1));
    CHECK_EQ(h.b().stats().packetsRx, static_cast<uint64_t>(kMessages + 1));
    CHECK_EQ(h.a().stats().txMessages, static_cast<uint64_t>(kMessages + 1));
    CHECK_EQ(h.b().stats().decoderErrors, 0u);
    CHECK_EQ(h.a().stats().decoderErrors, 0u);
    CHECK_EQ(h.b().stats().seqGaps, 0u);
    CHECK_EQ(h.a().stats().seqGaps, 0u);
    CHECK_EQ(payloadCount.load(std::memory_order_relaxed),
             static_cast<uint64_t>(kMessages));
    const uint64_t expectSum = static_cast<uint64_t>(kMessages) *
                               (static_cast<uint64_t>(kMessages) - 1) / 2;
    CHECK_EQ(payloadSum.load(std::memory_order_relaxed), expectSum);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.b().state(), SessionState::kConnected);
}

// S2：PING→PONG RTT（conductor 驱动 50 轮；rtt.samples == rxPong）。
void stress_ping_pong_rtt() {
    constexpr int kRounds = 50;
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    uint64_t t = 2000;
    for (int i = 0; i < kRounds; ++i) {
        h.clock.set(t);
        h.tickA();  // PING 到期（lastPingMs_ = t - 2000）
        CHECK_EQ(h.a().stats().txPing, static_cast<uint64_t>(i + 1));
        h.pumpB();  // B 处理 PING → 自动 PONG
        h.clock.set(t + 10);
        h.pumpA();  // A 处理 PONG → RTT=10
        CHECK_EQ(h.a().stats().rxPong, static_cast<uint64_t>(i + 1));
        CHECK_EQ(h.a().stats().rtt.samples, static_cast<uint64_t>(i + 1));
        t += 2000;
    }
    if (h.a().stats().rtt.lastMs.has_value()) {
        CHECK_EQ(*h.a().stats().rtt.lastMs, 10u);
    }
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.b().state(), SessionState::kConnected);
}

// S3：ACK 往返（B 在 onAckRequest 内自动 acknowledge；无重试、无悬挂 pending）。
void stress_ack_roundtrip() {
    constexpr int kMessages = 2000;
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.cfgA.ping_interval_ms = 1000000;
    h.cfgB.ping_interval_ms = 1000000;
    h.cfgA.peer_timeout_ms = 1000000;
    h.cfgB.peer_timeout_ms = 1000000;
    h.cfgA.ack_timeout_ms = 1000000;  // 拉长 deadline：本轮无重试
    std::atomic<int> ackReqs{0};
    h.cbB.onAckRequest = [&h, &ackReqs](uint8_t, const std::vector<uint8_t>&, uint16_t s) {
        ackReqs.fetch_add(1, std::memory_order_relaxed);
        // RX 回调内回 ACK：tryTransmit 系（非阻塞），符合回调契约。
        (void)h.b().acknowledge(s, 0, espview::proto::ErrorCode::kNone);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    espview::proto::test::Gate go;
    h.startSender(true, [] { return espview::proto::makeSetMode(DisplayMode::kWindow); },
                  kMessages, go);
    h.startFeederB();   // 消费 a2b_（SET_MODE）
    h.startFeederA();   // 消费 b2a_（ACK）
    h.startTicker(1);
    h.startStatsReader();

    go.open();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (h.a().stats().ackReceived < static_cast<uint64_t>(kMessages)) {
        if (std::chrono::steady_clock::now() > end) {
            break;  // 硬看门狗
        }
        std::this_thread::yield();
    }
    h.stopAll();
    CHECK_EQ(ackReqs.load(std::memory_order_relaxed), kMessages);
    CHECK_EQ(h.a().stats().ackReceived, static_cast<uint64_t>(kMessages));
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
    CHECK_EQ(h.ackTimeoutCount(), 0u);
    CHECK(h.onAckCount() >= 1);  // 单槽语义：至少末次匹配（精确值依赖交错）
    CHECK_EQ(h.b().stats().decoderErrors, 0u);
    CHECK_EQ(h.a().stats().decoderErrors, 0u);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.b().state(), SessionState::kConnected);

    // 无悬挂 pending：最终 tick（时钟大步进，不触发对端超时——已禁用）不重试。
    h.clock.set(2000000);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
}

// S4：reconnect 周期（epoch 换代，双侧断开）：计数精确累计、无 stale 槽泄漏。
void stress_reconnect_cycles() {
    constexpr int kCycles = 20;
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> ackReqs{0};
    std::atomic<uint16_t> lastAckSeq{0};
    h.cbB.onAckRequest = [&ackReqs, &lastAckSeq](uint8_t, const std::vector<uint8_t>&,
                                                 uint16_t s) {
        ackReqs.fetch_add(1, std::memory_order_relaxed);
        lastAckSeq.store(s, std::memory_order_relaxed);
    };
    h.init();
    CHECK(h.connectAndHandshake());
    const uint64_t helloBase = h.a().stats().txHello;  // 1

    for (int i = 0; i < kCycles; ++i) {
        // 链路断开 = 双方同时收到 disconnect（现实语义；双方 decoder 基线一致复位）。
        h.a().onTransportDisconnected();
        h.b().onTransportDisconnected();
        CHECK_EQ(h.a().state(), SessionState::kDisconnected);
        CHECK_EQ(h.b().state(), SessionState::kDisconnected);
        h.a().onTransportConnected();
        h.b().onTransportConnected();
        h.pumpBoth();  // 交换 HELLO → 双方 Connected
        CHECK_EQ(h.a().state(), SessionState::kConnected);
        CHECK_EQ(h.b().state(), SessionState::kConnected);
        CHECK_EQ(h.a().stats().decoderErrors, 0u);
        CHECK_EQ(h.b().stats().decoderErrors, 0u);
    }
    // 每轮 A 恰好发 1 个 HELLO（主动）；无 stale 槽泄漏（tick 不重发）。
    CHECK_EQ(h.a().stats().txHello, helloBase + static_cast<uint64_t>(kCycles));
    h.clock.set(100);
    h.tickA();
    CHECK_EQ(h.a().stats().txHello, helloBase + static_cast<uint64_t>(kCycles));
    CHECK_EQ(h.a().state(), SessionState::kConnected);

    // 最终一轮 SET_MODE 往返（会话健康；seq 归零可预测）。
    CHECK_EQ(h.a().sendMessage(espview::proto::makeSetMode(DisplayMode::kWindow)),
             SendResult::kOk);
    h.pumpB();
    CHECK_EQ(ackReqs.load(std::memory_order_relaxed), 1);
    CHECK_EQ(h.b().acknowledge(lastAckSeq.load(std::memory_order_relaxed), 0,
                               espview::proto::ErrorCode::kNone),
             SendResult::kOk);
    h.pumpA();
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 1u);
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
}

}  // namespace

void runEndpointStressTests() {
    std::printf("  stress_bulk_control_plane\n");
    stress_bulk_control_plane();
    std::printf("  stress_ping_pong_rtt\n");
    stress_ping_pong_rtt();
    std::printf("  stress_ack_roundtrip\n");
    stress_ack_roundtrip();
    std::printf("  stress_reconnect_cycles\n");
    stress_reconnect_cycles();
}