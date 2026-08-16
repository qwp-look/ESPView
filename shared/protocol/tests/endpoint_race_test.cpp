// ESPView M8-A2 — ProtocolEndpoint 确定性竞态测试（endpoint_race_test.cpp）。
//
// CASE 1-10（DESIGN.md AN 章）：
//   C1  tick + RX ACK（两种顺序）
//   C2  disconnect + ACK 在途（真实 feeder 线程）
//   C3  HELLO + tick handshake timeout（两种顺序）
//   C4  PING + PONG + disconnect（两种顺序；stale PONG 不污染 RTT）
//   C5  CAPABILITIES pending + disconnect（槽随会话清空）
//   C6  decoder reset + onTransportData（延后 reset 与 feed 的两种顺序）
//   C7  decoder 回调内 failSession → 延后 reset（tick 先执行）
//   C8  stats 快照 + 计数器单调性（真实多线程）
//   C9  reconnect + stale deferred control（主动 HELLO 延迟槽随边界清空）
//   C10 两路控制事件并发（SET_MODE ACK_REQ + PING 心跳）
// 全部经 CoordinatedHarness + testHooks 确定性编排；CHECK 宏线程安全。
// 纯 C++17。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "endpoint_harness.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_sync.h"
#include "test_util.h"

namespace {

using espview::proto::CapabilitiesController;
using espview::proto::CapabilitiesInfo;
using espview::proto::DisplayMode;
using espview::proto::ErrorCode;
using espview::proto::makePing;
using espview::proto::makeSetMode;
using espview::proto::MessageType;
using espview::proto::PixelFormat;
using espview::proto::SendResult;
using espview::proto::SessionError;
using espview::proto::SessionState;

// HIGH-1 测试辅助：4 字节 payload（index）的 bulk 消息（与 endpoint_stress_test 同构）。
espview::proto::Message bulkMessage4(uint32_t i) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(i & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 8) & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 16) & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 24) & 0xFFu));
    return espview::proto::makeMessage(static_cast<uint8_t>(MessageType::kInputKey), 0,
                                       std::move(payload));
}

// HIGH-1 测试辅助：合法 CAPABILITIES（B 侧 onCapabilities 计数用）。
CapabilitiesInfo makeCapsInfo() {
    CapabilitiesInfo caps;
    caps.virtualPresent = true;
    caps.width = 320;
    caps.height = 240;
    caps.pixelFormat = PixelFormat::kRgb565;
    caps.colorDepth = 16;
    caps.modeMask = 0b1111;
    caps.physWidth = 128;
    caps.physHeight = 64;
    caps.physPixelFormat = espview::proto::PhysicalPixelFormat::kMono1;
    caps.physColorDepth = 1;
    caps.physMono = true;
    caps.physController = CapabilitiesController::kSsd1306;
    caps.physI2cAddress = 0x3C;
    caps.sceneSupport = 0b11;
    return caps;
}

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

// C1a：tick 重试先于 ACK 落地 → 重试计数、pending 保留，随后 ACK 正常清空。
void case1_tick_retry_then_ack() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    // A 发 SET_MODE（seq=0）→ 进 a2b_；B 消费 → rxMessages = HELLO(握手) + SET_MODE。
    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(h.b().stats().rxMessages, 2u);

    // tick 重试在途（sink 保持），ACK 尚未喂给 A。
    h.sinkHoldA.close();
    h.clock.set(500);
    const int sinkBase = h.sinkCallsA.load(std::memory_order_acquire);
    std::thread ticker([&h] { h.tickA(); });
    while (h.sinkCallsA.load(std::memory_order_acquire) == sinkBase) {
        std::this_thread::yield();
    }
    // 此刻 B 回 ACK（进入 b2a_）。注意 ackSeq 必须匹配重试后的 seq（1）：
    // 重试重新编码（seq 0→1），pendingAck 已更新到 seq 1；回 seq 0 会被
    // handleAck 判定为错配（只计数不清槽），这正是本 CASE 想避免的时序。
    h.b().acknowledge(1, 0, ErrorCode::kNone);
    h.sinkHoldA.open();
    ticker.join();
    CHECK_EQ(h.a().stats().ackRetries, 1u);  // 重试已结算（ACK 尚未处理）
    CHECK_EQ(h.a().stats().ackFailures, 0u);
    h.pumpA();  // ACK 落地 → 清空 pending、onAck
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 1u);
    h.clock.set(1100);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 1u);  // 无 pending → 不再重试
    CHECK_EQ(h.ackTimeoutCount(), 0u);
}

// C1b：ACK 先落地（tick 被 tickGateA 钉住）→ tick 恢复后看到 pending 已清。
void case1_ack_then_tick() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> entered{0};
    h.cfgA.testHooks.onTickStateSnapshot = [&h, &entered] {
        entered.fetch_add(1, std::memory_order_release);
        h.tickGateA.wait();
    };
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(h.b().stats().rxMessages, 2u);

    h.tickGateA.close();
    h.clock.set(500);
    std::thread ticker([&h] { h.tickA(); });
    // 等 tick 进入快照钩子（tickGateA 关闭 → 阻塞）。钩子在状态快照后、ACK
    // 重试判定前；entered 原子确认线程已进入（确定性，无固定 sleep）。
    while (entered.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    // ACK 先落地（b 回 ACK 并喂给 A）。
    h.b().acknowledge(0, 0, ErrorCode::kNone);
    h.pumpA();
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 1u);

    h.tickGateA.open();
    ticker.join();
    CHECK_EQ(h.a().stats().ackRetries, 0u);  // tick 恢复后 pending 已清 → 不重试
    CHECK_EQ(h.ackTimeoutCount(), 0u);
}

// C2：disconnect + ACK 在途（真实 feeder 线程被 feedGateA 钉住）。
void case2_disconnect_ack_inflight() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    // 真实 feeder 线程在取 decoderMutex_ 前被 feedGateA 钉住（onFeedEnter 钩子）。
    h.cfgA.testHooks.onFeedEnter = [&h] { h.feedGateA.wait(); };
    std::atomic<int> discHookCalls{0};
    // M8-A2：接入未使用的 testHooks.onDisconnectCleared（断开清理完成后触发）。
    h.cfgA.testHooks.onDisconnectCleared = [&discHookCalls] {
        discHookCalls.fetch_add(1, std::memory_order_release);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kMirror)), SendResult::kOk);
    h.pumpB();
    h.b().acknowledge(0, 0, ErrorCode::kNone);
    CHECK(!h.b2a_.empty());  // ACK 在途

    h.feedGateA.close();
    h.startFeederA();  // 进入 onFeedEnter 即被钉住
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    h.a().onTransportDisconnected();  // 断开（epoch++、pendingAck 清空）
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);

    h.feedGateA.open();  // 放行 ACK → 会话层丢弃
    h.stopAll();
    CHECK_EQ(h.a().stats().ackReceived, 0u);
    CHECK_EQ(h.onAckCount(), 0u);
    CHECK_EQ(discHookCalls.load(std::memory_order_acquire), 1);  // 钩子恰好触发一次
}

// C3a：handshake timeout tick 先于 HELLO → Disconnected，随后 HELLO 被动恢复。
void case3_timeout_then_hello() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> failHookCalls{0};
    // M8-A2：接入未使用的 testHooks.onTickBeforeFail（tick 将 failSession 前触发）。
    h.cfgA.testHooks.onTickBeforeFail = [&failHookCalls] {
        failHookCalls.fetch_add(1, std::memory_order_release);
    };
    h.init();
    h.connectBoth();
    CHECK_EQ(h.a().state(), SessionState::kConnecting);

    h.clock.set(5000);
    h.tickA();
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);
    CHECK_EQ(h.a().stats().handshakeTimeouts, 1u);
    CHECK_EQ(failHookCalls.load(std::memory_order_acquire), 1);  // 钩子恰好触发一次

    CHECK_EQ(h.b().sendHello(), SendResult::kOk);
    h.pumpA();  // 被动恢复 → 回发 HELLO → 握手完成
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.a().stats().txHello, 2u);  // 主动 1 + 被动回复 1
    CHECK_EQ(h.a().stats().errors, 1u);
}

// C3b：HELLO 先于 timeout tick（tick 在快照钩子被钉住）→ 握手完成；tick 恢复后
// 按 M8-A1 快照语义判定（快照是 Connecting + deadline 已过）→ failSession；
// 随后被动恢复重新握手成功（failSession 幂等 + epoch 换代保证干净）。
void case3_hello_then_timeout_tick() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> entered{0};
    h.cfgA.testHooks.onTickStateSnapshot = [&h, &entered] {
        entered.fetch_add(1, std::memory_order_release);
        h.tickGateA.wait();
    };
    h.init();
    h.connectBoth();
    h.tickGateA.close();
    h.clock.set(5000);
    std::thread ticker([&h] { h.tickA(); });
    while (entered.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }

    CHECK_EQ(h.b().sendHello(), SendResult::kOk);
    h.pumpA();  // HELLO 先处理 → Connected
    CHECK_EQ(h.a().state(), SessionState::kConnected);

    h.tickGateA.open();
    ticker.join();
    // 快照（Connecting）在 HELLO 之前取得：恢复后 deadline 已过 → 判超时断开。
    CHECK_EQ(h.a().stats().handshakeTimeouts, 1u);
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);
    CHECK_EQ(h.a().stats().errors, 1u);

    // 被动恢复：B 需以新会话基线（seq 从 0 起）重发 HELLO——B 从未断开，
    // 其 encoder 已推进到 seq 2；若直接用 sendHello()，A failSession 后的
    // decoder 基线 0 会把 seq>0 的 HELLO 当 seq gap 丢弃，恢复无法完成。
    // 让 B 断开+重连（decoder/encoder 复位）再发 HELLO，与真实重启语义一致。
    h.b().onTransportDisconnected();
    h.b().onTransportConnected();
    h.pumpA();
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.a().stats().txHello, 2u);
}

// C4a：PONG 先落地 → RTT 记录；随后断开重置 RTT。
void case4_pong_then_disconnect() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    h.clock.set(2000);
    h.tickA();  // A 发 PING
    CHECK_EQ(h.a().stats().txPing, 1u);
    h.pumpB();  // B 处理 PING → 回 PONG
    h.clock.set(2010);
    h.pumpA();  // A 处理 PONG → RTT=10
    CHECK_EQ(h.a().stats().rxPong, 1u);
    CHECK_EQ(h.a().stats().rtt.samples, 1u);

    h.a().onTransportDisconnected();  // RTT 随会话重置
    CHECK_EQ(h.a().stats().rtt.samples, 0u);
    CHECK(!h.a().stats().rtt.lastMs.has_value());
}

// C4b：断开+重连后，旧会话 stale PONG 不污染新会话 RTT。
void case4_disconnect_then_stale_pong() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    h.clock.set(2000);
    h.tickA();  // 会话 1：PING（txPing=1，lastPingSentAtMs_=2000，epoch=1）
    h.pumpB();  // B 回 PONG → b2a_（A 未消费，在途）
    h.clock.set(2010);
    // A 重连（decoder 基线复位为 0；A HELLO(0) → a2b_）。
    h.a().onTransportDisconnected();
    h.a().onTransportConnected();
    // B 仍在旧会话：再发 HELLO 走旧 encoder → seq=1——恰好匹配 A 消费 stale
    // PONG(0) 后的基线 1（若 B 也复位，其 HELLO 会是 seq 0，必被 A 判 seq gap）。
    CHECK_EQ(h.b().sendHello(), SendResult::kOk);
    // B 对称重连：decoder/encoder 复位（新会话基线 0）；重连 HELLO 背压进
    // helloSlot，不污染 A 的 RX 队列（否则 HELLO(0) 会把 A 基线推到 1，
    // 新 PONG(0) 将无法解码）。
    h.blockSinkB.store(true, std::memory_order_release);
    h.b().onTransportDisconnected();
    h.b().onTransportConnected();
    h.blockSinkB.store(false, std::memory_order_release);
    // A 先消费 [stale PONG(0), B 的旧会话 HELLO(1)] → 握手完成。
    h.pumpA();
    CHECK_EQ(h.a().stats().rxPong, 1u);
    CHECK_EQ(h.a().stats().rtt.samples, 0u);  // stale PONG 不污染 RTT
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    // B 收 A 的 HELLO(0) → 完成握手（decoder 基线 0）。
    h.pumpB();
    CHECK_EQ(h.b().state(), SessionState::kConnected);

    // 新会话 PING 到期（握手完成于 2010 → 4010 差 2000ms）→ 发送。
    h.clock.set(4010);
    h.tickA();
    CHECK_EQ(h.a().stats().txPing, 2u);  // 会话 1 的 1 次 + 会话 2 的 1 次（跨会话累计）
    h.pumpB();  // B 处理 PING → 回 PONG(0)
    h.clock.set(4020);
    h.pumpA();  // A 基线 0 → PONG(0) 解码 → RTT=10
    CHECK_EQ(h.a().stats().rxPong, 2u);
    CHECK_EQ(h.a().stats().rtt.samples, 1u);
    if (h.a().stats().rtt.lastMs.has_value()) {
        CHECK_EQ(*h.a().stats().rtt.lastMs, 10u);
    }
}

// C5：CAPABILITIES 背压暂存 + disconnect → 槽随会话清空；新会话重发正常。
void case5_capabilities_pending_then_disconnect() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    CapabilitiesInfo caps;
    caps.virtualPresent = true;
    caps.width = 320;
    caps.height = 240;
    caps.pixelFormat = PixelFormat::kRgb565;
    caps.colorDepth = 16;
    caps.modeMask = 0b1111;
    caps.physWidth = 128;
    caps.physHeight = 64;
    caps.physPixelFormat = espview::proto::PhysicalPixelFormat::kMono1;
    caps.physColorDepth = 1;
    caps.physMono = true;
    caps.physController = CapabilitiesController::kSsd1306;
    caps.physI2cAddress = 0x3C;
    caps.sceneSupport = 0b11;

    h.blockSinkA.store(true, std::memory_order_release);
    const SendResult r = h.a().sendCapabilities(caps);
    CHECK_EQ(r, SendResult::kBackpressure);
    CHECK_EQ(h.a().stats().txCapabilities, 0u);  // 暂存未计数

    h.blockSinkA.store(false, std::memory_order_release);
    h.a().onTransportDisconnected();  // 槽清空（epoch 边界）
    h.a().onTransportConnected();
    CHECK_EQ(h.b().sendHello(), SendResult::kOk);
    h.pumpA();
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    h.tickA();
    CHECK_EQ(h.a().stats().txCapabilities, 0u);  // 旧槽已清 → 不发送

    CHECK_EQ(h.a().sendCapabilities(caps), SendResult::kOk);
    CHECK_EQ(h.a().stats().txCapabilities, 1u);
}

// C6a：延后 decoder reset 由 tick 先执行，随后 HELLO feed 干净恢复。
void case6_tick_deferred_reset_then_feed() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    h.connectBoth();

    // 非法 HELLO → failSession（decoder 回调内）→ decoderResetPending_=true。
    espview::proto::Message bad;
    bad.type = static_cast<uint8_t>(MessageType::kHello);
    bad.flags = 0;
    bad.payload = {1, 0, 0x40, 0x01, 0xf0, 0x00, 0x00, 0x07, 5, 'e', 's', 'p', 't', 'e', 's', 't'};
    {
        // 用独立编码器产生真实字节（seq 从 0 起，与握手基线一致）。
        espview::proto::SequenceCounter seq;
        espview::proto::MessageEncoder enc(seq);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(bad, pkts), espview::proto::PacketError::kNone);
        for (const auto& p : pkts) {
            h.a().onTransportData(p.data(), p.size());
        }
    }
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);
    CHECK_EQ(h.a().stats().errors, 1u);

    h.clock.set(50);
    h.tickA();  // tick 执行延后 reset（decoderResetPending_ 路径）

    // connectBoth 时 B 的 HELLO(0) 仍在 b2a_（从未消费）——即被动恢复的
    // "HELLO feed"。不能再 B.sendHello()：B 的 encoder 已推进到 seq 1，
    // A 复位后的基线 0 会把 seq>0 的 HELLO 当 seq gap 丢弃，恢复无法完成。
    h.pumpA();
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.a().stats().decoderErrors, 0u);
}

// C6b：延后 reset 由下一次 feed 开头执行（onTransportData 路径）。
void case6_feed_deferred_reset_first() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    h.connectBoth();

    espview::proto::Message bad;
    bad.type = static_cast<uint8_t>(MessageType::kHello);
    bad.flags = 0;
    bad.payload = {1, 0, 0x40, 0x01, 0xf0, 0x00, 0x00, 0x07, 5, 'e', 's', 'p', 't', 'e', 's', 't'};
    {
        espview::proto::SequenceCounter seq;
        espview::proto::MessageEncoder enc(seq);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(bad, pkts), espview::proto::PacketError::kNone);
        for (const auto& p : pkts) {
            h.a().onTransportData(p.data(), p.size());
        }
    }
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);

    // feed 开头执行延后 reset → 干净被动恢复（B 的 HELLO(0) 仍排队在 b2a_；
    // 不能 B.sendHello()——旧 encoder 的 seq>0 会被 A 复位后的基线判 gap）。
    h.pumpA();
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.a().stats().decoderErrors, 0u);
    CHECK_EQ(h.a().stats().rxHello, 1u);
}

// C7：同一 feed 内 非法 HELLO → 被动恢复（decoderResetPending_ 被被动分支清除，
// 不把延迟 reset 打进恢复后的会话）。
void case7_callback_failsession_then_passive_recovery_same_feed() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    h.connectBoth();

    espview::proto::Message bad;
    bad.type = static_cast<uint8_t>(MessageType::kHello);
    bad.flags = 0;
    bad.payload = {1, 0, 0x40, 0x01, 0xf0, 0x00, 0x00, 0x07, 5, 'e', 's', 'p', 't', 'e', 's', 't'};
    const auto ok = espview::proto::makeHello(espview::proto::kProtocolVersion, 0, 320, 240,
                                              PixelFormat::kRgb565, 0b111, "peer");
    CHECK(ok.has_value());
    {
        // 注意：MessageEncoder::encode() 每次清空 out——必须用两个独立 vector，
        // 否则第二个 encode 会覆盖第一个（bad 包丢失，场景不成立）。
        // 同一 SequenceCounter：bad 包 seq=0、好 HELLO 包 seq=1。
        espview::proto::SequenceCounter seq;
        espview::proto::MessageEncoder enc(seq);
        std::vector<std::vector<uint8_t>> badPkts, okPkts;
        CHECK_EQ(enc.encode(bad, badPkts), espview::proto::PacketError::kNone);
        CHECK_EQ(enc.encode(*ok, okPkts), espview::proto::PacketError::kNone);
        // 单次 onTransportData 内先坏 HELLO 后好 HELLO（同一 feed）。
        std::vector<uint8_t> all;
        for (const auto& p : badPkts) {
            all.insert(all.end(), p.begin(), p.end());
        }
        for (const auto& p : okPkts) {
            all.insert(all.end(), p.begin(), p.end());
        }
        h.a().onTransportData(all.data(), all.size());
    }
    // 坏 HELLO（seq 0）→ failSession（decoder 回调内）；好 HELLO（seq 1）紧随
    // 同一 feed 消费（bad 已把基线推到 1）→ 被动恢复分支清除 decoderResetPending_
    // （不把延迟 reset 打进恢复后的会话）→ 完成握手。
    CHECK_EQ(h.a().state(), SessionState::kConnected);  // 被动恢复完成
    CHECK_EQ(h.a().stats().errors, 1u);
    CHECK_EQ(h.a().stats().decoderErrors, 0u);
    // 恢复后控制面可用（无延迟 reset 污染）：先让 B 消费 A 的旧 HELLO 并对称重连
    // 对齐基线，再做 PING 往返（否则残留 HELLO/基线错位会把 PING 当 seq gap 丢弃）。
    h.pumpB();  // B 收 A 的 HELLO(0)（connectBoth 时发出）→ B Connected
    h.b().onTransportDisconnected();  // B decoder/encoder 复位
    h.b().onTransportConnected();     // B HELLO(0) → b2a_（新会话）
    h.pumpA();                        // A 已 Connected：重复 HELLO → 重新确认 no-op
    CHECK_EQ(h.a().sendMessage(makePing(1)), SendResult::kOk);
    h.pumpB();  // B 基线 0 → PING(0) 解码 → rxPing=1
    CHECK_EQ(h.b().stats().rxPing, 1u);
}

// C8：stats 快照单调性（真实多线程：B 发 N 个 PING + stats reader 并发快照）。
void case8_stats_snapshot_monotonic() {
    constexpr int kPings = 400;
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    espview::proto::test::Gate go;
    h.startSender(false, [] { return makePing(7); }, kPings, go);  // B 发 N 个 PING → b2a_
    h.startFeederA();  // b2a_ → A（A 的 RX；B 从未收包，rxPing 不增长）
    h.startStatsReader();

    std::atomic<uint64_t> lastPackets{0};
    std::atomic<uint64_t> lastRxPing{0};
    // 用 conductor 循环读 A 快照，校验单调不减（看门狗 15s）。
    go.open();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool monotonic = true;
    while (h.a().stats().packetsRx < static_cast<uint64_t>(kPings + 1)) {
        const auto s = h.a().stats();
        if (s.packetsRx < lastPackets.load(std::memory_order_relaxed) ||
            s.rxPing < lastRxPing.load(std::memory_order_relaxed)) {
            monotonic = false;
        }
        lastPackets.store(s.packetsRx, std::memory_order_relaxed);
        lastRxPing.store(s.rxPing, std::memory_order_relaxed);
        if (std::chrono::steady_clock::now() > end) {
            break;  // 看门狗：不无限等
        }
        std::this_thread::yield();
    }
    h.stopAll();
    CHECK(monotonic);
    // A 收到：握手 HELLO 1 包 + PING×N（每 PING 1 包）；PING 消息计数同包数。
    CHECK_EQ(h.a().stats().packetsRx, static_cast<uint64_t>(kPings + 1));
    CHECK_EQ(h.a().stats().rxMessages, static_cast<uint64_t>(kPings + 1));
    CHECK_EQ(h.a().stats().rxPing, static_cast<uint64_t>(kPings));
    // B 的 PING 经 sendMessage() 发送 → 计入 txMessages（txPing 只在心跳 tick
    // 路径递增；B 没有 tick）。B 的握手 HELLO 走 sendHello → tryTransmit →
    // afterSend，也计入 txMessages，故为 kPings + 1。
    CHECK_EQ(h.b().stats().txMessages, static_cast<uint64_t>(kPings + 1));
}

// C9：主动 HELLO 延迟槽随 disconnect 清空；新会话 HELLO 正常发送。
void case9_reconnect_stale_deferred_control() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();

    h.blockSinkA.store(true, std::memory_order_release);
    h.a().onTransportConnected();  // 主动 HELLO 背压失败 → helloSlot
    CHECK_EQ(h.a().state(), SessionState::kConnecting);
    CHECK_EQ(h.a().stats().txHello, 0u);  // 延迟未计数

    h.blockSinkA.store(false, std::memory_order_release);
    h.a().onTransportDisconnected();  // 槽清空（epoch 边界）
    h.a().onTransportConnected();     // 新会话：HELLO 立即发出
    CHECK_EQ(h.a().stats().txHello, 1u);
    h.b().onTransportConnected();  // B 也必须连接，否则 sendHello 返回 kNotConnected
    CHECK_EQ(h.b().sendHello(), SendResult::kOk);
    h.pumpA();
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    h.tickA();
    CHECK_EQ(h.a().stats().txHello, 1u);  // 旧槽已清 → 不重复发送
}

// C10：两路控制事件并发（SET_MODE ACK_REQ + PING 心跳）。
void case10_two_control_events_concurrent() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> ackReqs{0};
    h.cbB.onAckRequest = [&ackReqs](uint8_t, const std::vector<uint8_t>&, uint16_t) {
        ackReqs.fetch_add(1, std::memory_order_relaxed);
    };
    // ACK deadline 拉长：SET_MODE 的 pendingAck 不会在本轮 tick 触发重试
    // （重试会多发一份 SET_MODE → B 的计数不再精确）。
    h.cfgA.ack_timeout_ms = 1000000;
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    h.startFeederB();          // B 并发消费 A 的 SET_MODE（onAckRequest）
    h.clock.set(2000);
    h.tickA();                 // A：心跳 PING 到期（lastPingMs_=0）→ 恰好一次
    h.tickB();                 // B：同理发 PING（仅一次；不用 ticker 避免多轮）

    // 并发等待两路事件落地（feeder 线程消费；硬看门狗）。
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (ackReqs.load(std::memory_order_relaxed) < 1 ||
           h.b().stats().rxPing < 1u) {
        if (std::chrono::steady_clock::now() > end) {
            break;  // 看门狗
        }
        std::this_thread::yield();
    }
    h.stopAll();
    CHECK_EQ(ackReqs.load(std::memory_order_relaxed), 1);  // SET_MODE 恰好一次
    CHECK_EQ(h.b().stats().rxPing, 1u);                    // PING 恰好一次
    CHECK_EQ(h.b().stats().txPong, 1u);                    // 自动 PONG 一次
    CHECK_EQ(h.b().state(), SessionState::kConnected);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    // 控制消息不得破坏握手/会话。
    CHECK_EQ(h.a().stats().decoderErrors, 0u);
    CHECK_EQ(h.b().stats().decoderErrors, 0u);
}

// M8-A2 HIGH-1：drain 失败（仅一次 sink 失败）+ 并发 app sender 成功发送的交错。
//   回退在发送临界区内按 epoch 复查、只回退未上送 seq——并发成功发送消耗的 seq
//   绝不被回卷（旧实现：外部 seq_.reset(seqBefore) 在 T 释放后执行，会把并发线程
//   已上送的 seq 回卷 → 对端判 kSequenceGap 丢弃 → 握手/控制面卡死）。
void high1_drain_fail_concurrent_send_no_seq_gap() {
    constexpr int kMsgs = 800;
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<uint64_t> payloadSum{0};
    std::atomic<uint64_t> payloadCount{0};
    std::atomic<int> capsSeen{0};
    h.cbB.onOtherMessage = [&payloadSum, &payloadCount](const espview::proto::Message& m) {
        if (m.payload.size() < 4) {
            return;
        }
        const uint32_t idx = static_cast<uint32_t>(m.payload[0]) |
                             (static_cast<uint32_t>(m.payload[1]) << 8) |
                             (static_cast<uint32_t>(m.payload[2]) << 16) |
                             (static_cast<uint32_t>(m.payload[3]) << 24);
        payloadSum.fetch_add(idx, std::memory_order_relaxed);
        payloadCount.fetch_add(1, std::memory_order_relaxed);
    };
    h.cbB.onCapabilities = [&capsSeen](const espview::proto::CapabilitiesInfo&) {
        capsSeen.fetch_add(1, std::memory_order_relaxed);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    // 阶段 1：capabilities 背压入槽（失败尝试不消耗 seq）。
    h.blockSinkA.store(true, std::memory_order_release);
    CHECK_EQ(h.a().sendCapabilities(makeCapsInfo()), SendResult::kBackpressure);
    CHECK_EQ(h.a().stats().txCapabilities, 0u);
    h.blockSinkA.store(false, std::memory_order_release);

    // 阶段 2：drain 首次尝试恰好失败一次 + 并发 app sender 成功发送。
    //   failNext 可能被 drain 或 app 首个消息命中——两种交错下回退都只在发送
    //   临界区内执行，对端 seq 基线保持连续。
    h.failNextSinkA.store(true, std::memory_order_release);
    espview::proto::test::Gate go;
    h.startSender(true, [i = 0]() mutable { return bulkMessage4(i++); }, kMsgs, go);
    go.open();
    h.tickA();  // drain：首次 sink 调用失败 → kBackpressure（槽保留）
    h.stopAll();
    h.pumpB();

    // 阶段 3：drain 重试成功 → capabilities 恰好一次到达 B；seq 连续无 gap。
    h.tickA();
    CHECK_EQ(h.a().stats().txCapabilities, 1u);
    h.pumpB();
    CHECK_EQ(capsSeen.load(std::memory_order_relaxed), 1);

    // 终态：对端零 decoder 错误 / 零 seq gap / payload 校验和完整（无丢失无乱序）。
    CHECK_EQ(h.b().stats().decoderErrors, 0u);
    CHECK_EQ(h.b().stats().seqGaps, 0u);
    CHECK_EQ(h.a().stats().decoderErrors, 0u);
    CHECK_EQ(h.a().stats().seqGaps, 0u);
    const uint64_t n = payloadCount.load(std::memory_order_relaxed);
    const bool allSent = (n == static_cast<uint64_t>(kMsgs));
    CHECK(allSent || n == static_cast<uint64_t>(kMsgs) - 1);  // 首个 app 消息可能吃 failNext
    const uint64_t expectSum = allSent ? n * (n - 1) / 2 : n * (n + 1) / 2;
    CHECK_EQ(payloadSum.load(std::memory_order_relaxed), expectSum);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.b().state(), SessionState::kConnected);
}

}  // namespace
void runEndpointRaceTests() {
    std::printf("  case1_tick_retry_then_ack\n");
    case1_tick_retry_then_ack();
    std::printf("  case1_ack_then_tick\n");
    case1_ack_then_tick();
    std::printf("  case2_disconnect_ack_inflight\n");
    case2_disconnect_ack_inflight();
    std::printf("  case3_timeout_then_hello\n");
    case3_timeout_then_hello();
    std::printf("  case3_hello_then_timeout_tick\n");
    case3_hello_then_timeout_tick();
    std::printf("  case4_pong_then_disconnect\n");
    case4_pong_then_disconnect();
    std::printf("  case4_disconnect_then_stale_pong\n");
    case4_disconnect_then_stale_pong();
    std::printf("  case5_capabilities_pending_then_disconnect\n");
    case5_capabilities_pending_then_disconnect();
    std::printf("  case6_tick_deferred_reset_then_feed\n");
    case6_tick_deferred_reset_then_feed();
    std::printf("  case6_feed_deferred_reset_first\n");
    case6_feed_deferred_reset_first();
    std::printf("  case7_callback_failsession_then_passive_recovery_same_feed\n");
    case7_callback_failsession_then_passive_recovery_same_feed();
    std::printf("  case8_stats_snapshot_monotonic\n");
    case8_stats_snapshot_monotonic();
    std::printf("  case9_reconnect_stale_deferred_control\n");
    case9_reconnect_stale_deferred_control();
    std::printf("  case10_two_control_events_concurrent\n");
    case10_two_control_events_concurrent();
    std::printf("  high1_drain_fail_concurrent_send_no_seq_gap\n");
    high1_drain_fail_concurrent_send_no_seq_gap();
}
