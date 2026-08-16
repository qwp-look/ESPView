// ESPView M8-A2 — ProtocolEndpoint 生命周期/销毁纪律测试（endpoint_lifecycle_test.cpp）。
//
// 覆盖（DESIGN.md AN 章「回调生命周期契约」+ M8-A2 生命周期纪律）：
//   L1  quiesce（先 join 角色线程）后销毁 endpoint：无崩溃、无悬挂线程、计数精确；
//   L2  disconnect 后 pending 控制槽已清 → 随后的 tick 不重试、不发送；
//   L3  reconnect 后 peerHello_/回调状态复位：旧会话信息不泄漏到新会话；
//   L4  shared_ptr + weak_ptr mock transport（weak token 仅测试侧）：transport
//       销毁后 sink 返回 kError → 发送面安全失败、会话状态不变、不崩溃；
//   L5  stop transport（RX 回调在途）→ join → 销毁：先 join 后销毁安全。
// 纯 C++17；CHECK 宏线程安全。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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

// 批量消息（合法类型 kInputKey → onOtherMessage；单包；无 ACK_REQ）。
Message bulkMessage(uint32_t i) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(i & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 8) & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 16) & 0xFFu));
    payload.push_back(static_cast<uint8_t>((i >> 24) & 0xFFu));
    return espview::proto::makeMessage(static_cast<uint8_t>(MessageType::kInputKey), 0,
                                       std::move(payload));
}

// L1：quiesce（先 join 角色线程）后销毁 → 无崩溃；终态可判定。
void lifecycle_quiesce_then_destroy() {
    constexpr int kMessages = 500;
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    // 关闭心跳/ACK 重试，保持计数精确（只测生命周期）。
    h.cfgA.ping_interval_ms = 1000000;
    h.cfgB.ping_interval_ms = 1000000;
    h.cfgA.ack_timeout_ms = 1000000;
    h.init();
    CHECK(h.connectAndHandshake());

    espview::proto::test::Gate go;
    h.startSender(true, [i = 0]() mutable { return bulkMessage(i++); }, kMessages, go);
    h.startFeederB();
    h.startStatsReader();

    go.open();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (h.b().stats().rxMessages < static_cast<uint64_t>(kMessages + 1)) {
        if (std::chrono::steady_clock::now() > end) {
            break;  // 硬看门狗
        }
        std::this_thread::yield();
    }
    h.stopAll();  // quiesce：join sender/feeder/stats reader 后才离开作用域
    CHECK_EQ(h.b().stats().rxMessages, static_cast<uint64_t>(kMessages + 1));
    CHECK_EQ(h.a().stats().txMessages, static_cast<uint64_t>(kMessages + 1));
    CHECK_EQ(h.b().stats().decoderErrors, 0u);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.b().state(), SessionState::kConnected);
    // 作用域退出：harness 析构销毁两个 endpoint（已 quiesce）→ 安全。
}

// L2：disconnect 后 pending 控制槽已清 → 随后的 tick 不重试、不发送。
void lifecycle_disconnect_pending_control_pump() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    CHECK_EQ(h.a().stats().txMessages, 2u);  // HELLO + SET_MODE
    h.a().onTransportDisconnected();         // pendingAck/槽 随会话清空
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);

    h.clock.set(600);  // 原 pendingAck deadline 已过
    h.tickA();         // 无 pending → 不重试、不失败、不回调
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
    CHECK_EQ(h.ackTimeoutCount(), 0u);
    CHECK_EQ(h.a().stats().txMessages, 2u);  // 断开后无任何发送
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);
}

// L3：reconnect 后 peerHello_/回调状态复位：旧会话信息不泄漏到新会话。
void lifecycle_reconnect_peer_hello_reset() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> hellos{0};
    std::atomic<int> connectHookCalls{0};
    // M8-A2：接入未使用的 testHooks.onConnectEnter（onTransportConnected 取锁前）。
    h.cfgA.testHooks.onConnectEnter = [&connectHookCalls] {
        connectHookCalls.fetch_add(1, std::memory_order_release);
    };
    h.cfgB.device_name = "peer-B";
    h.cbA.onHello = [&hellos](const espview::proto::HelloInfo&) {
        hellos.fetch_add(1, std::memory_order_relaxed);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    // 会话 1：peerHello_ 已填 B 的信息；onHello 触发一次。
    CHECK(h.a().peerHello().device_name == "peer-B");
    CHECK_EQ(hellos.load(std::memory_order_relaxed), 1);

    // 断开：peerHello_ 清空（Faraday）、RTT 无测量。
    h.a().onTransportDisconnected();
    CHECK(h.a().peerHello().device_name.empty());
    CHECK(!h.a().stats().rtt.lastMs.has_value());

    // 重连握手：peerHello_ 重新填充；onHello 对新会话再触发一次。
    h.a().onTransportConnected();
    CHECK_EQ(h.b().sendHello(), SendResult::kOk);
    h.pumpA();
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK(h.a().peerHello().device_name == "peer-B");
    CHECK_EQ(hellos.load(std::memory_order_relaxed), 2);
    CHECK_EQ(connectHookCalls.load(std::memory_order_acquire), 2);  // 初次连接 + 重连
}

// L4：shared_ptr + weak_ptr mock transport（weak token 仅测试侧）——transport
// 销毁后 sink 返回 kError → 发送面安全失败、会话状态不变、不崩溃。
void lifecycle_shared_weak_mock_transport() {
    auto transport = std::make_shared<int>(0x1234);
    std::weak_ptr<int> weak = transport;
    std::vector<std::vector<uint8_t>> out;
    espview::proto::EndpointConfig cfg;
    espview::proto::ProtocolEndpoint::Callbacks cb;
    const auto sink = [weak, &out](const uint8_t* d, size_t n) -> espview::proto::SendStatus {
        if (weak.expired()) {
            return espview::proto::SendStatus::kError;
        }
        out.emplace_back(d, d + n);
        return espview::proto::SendStatus::kOk;
    };
    espview::proto::ProtocolEndpoint ep(cfg, sink, cb);
    ep.onTransportConnected();  // HELLO 发出（transport 存活）
    CHECK_EQ(out.size(), 1u);

    // 手动完成握手：喂一个对端 HELLO（独立编码器；seq 从 0 起）。
    const auto peerHello = espview::proto::makeHello(
        espview::proto::kProtocolVersion, 1, 640, 480,
        espview::proto::PixelFormat::kRgb565, 0b1111, "peer");
    CHECK(peerHello.has_value());
    {
        espview::proto::SequenceCounter seq;
        espview::proto::MessageEncoder enc(seq);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(*peerHello, pkts), espview::proto::PacketError::kNone);
        for (const auto& p : pkts) {
            ep.onTransportData(p.data(), p.size());
        }
    }
    CHECK_EQ(ep.state(), SessionState::kConnected);
    CHECK_EQ(ep.sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    CHECK_EQ(out.size(), 2u);  // HELLO + SET_MODE

    transport.reset();  // transport 销毁 → sink 的 weak token 失效
    CHECK(weak.expired());
    CHECK_EQ(ep.sendMessage(makeSetMode(DisplayMode::kMirror)), SendResult::kTransportError);
    CHECK_EQ(ep.state(), SessionState::kConnected);  // 发送失败不改变会话状态
    CHECK_EQ(out.size(), 2u);                        // 失败的消息未写入
}

// L5：在途 RX 回调 + stop transport → join → 销毁：先 join 后销毁安全。
void lifecycle_inflight_rx_then_stop_destroy() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> entered{0};
    // onFeedEnter：先记录"已进入"，再阻塞在 feedGateA（模拟 RX 回调在途）。
    h.cfgA.testHooks.onFeedEnter = [&h, &entered] {
        entered.fetch_add(1, std::memory_order_release);
        h.feedGateA.wait();
    };
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    h.pumpB();
    h.b().acknowledge(0, 0, espview::proto::ErrorCode::kNone);
    CHECK(!h.b2a_.empty());  // ACK 已在 b2a_
    // M8-A2 HIGH-2：清掉握手 pump（connectAndHandshake 的 pumpA）已触发的钩子计数，
    // 使下面的等待只反映 feeder 对 ACK 的处理（旧写法下 entered 在握手时已为 1，
    // 等待立即通过——feeder 尚未 pop 就被 stopAll 的 stop 信号跳过，ACK 永远不消费）。
    entered.store(0, std::memory_order_release);

    h.feedGateA.close();
    h.startFeederA();  // 立即 pop 到 ACK → onTransportData 内被钉住
    while (entered.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();
    }
    // 此刻 feeder 正阻塞在 onTransportData 内（RX 回调在途）。
    h.stopAll();  // 放行闸门 + join feeder —— 回调完成后才离开作用域销毁。
    CHECK_EQ(h.a().stats().ackReceived, 1u);  // 在途回调已正常完成
    CHECK_EQ(h.onAckCount(), 1u);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
}

}  // namespace

void runEndpointLifecycleTests() {
    std::printf("  lifecycle_quiesce_then_destroy\n");
    lifecycle_quiesce_then_destroy();
    std::printf("  lifecycle_disconnect_pending_control_pump\n");
    lifecycle_disconnect_pending_control_pump();
    std::printf("  lifecycle_reconnect_peer_hello_reset\n");
    lifecycle_reconnect_peer_hello_reset();
    std::printf("  lifecycle_shared_weak_mock_transport\n");
    lifecycle_shared_weak_mock_transport();
    std::printf("  lifecycle_inflight_rx_then_stop_destroy\n");
    lifecycle_inflight_rx_then_stop_destroy();
}
