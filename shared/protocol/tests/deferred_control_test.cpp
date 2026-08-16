// ESPView M8-A2 — Deferred Control 单槽语义测试（deferred_control_test.cpp）。
//
// 覆盖（DESIGN.md AN 章「Deferred Control」）：
//   D1  helloSlot：replace-if-present（主动 HELLO 背压失败进入槽；重复入槽覆盖）；
//       排空失败不清槽；排空成功恰好一次；随后对称重连 + SET_MODE 往返成功。
//   D2  helloSlot：disconnect 清空（epoch 边界）；新会话 HELLO 正常发出并往返。
//   D3  capabilitiesSlot：latest-wins（两次背压入槽 → 只发最新）；恰好一次排空。
//   D4  capabilitiesSlot：disconnect 清空；对称重连后重发正常。
//   D5  组合场景：hello+capabilities 均 deferred → 各自排空 → 对称重连后
//       SET_MODE 往返成功（结尾无永久 pending）。
// 纯 C++17；conductor 驱动（确定性）；CHECK 宏线程安全。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "endpoint_harness.h"
#include "message.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_sync.h"
#include "test_util.h"

namespace {

using espview::proto::CapabilitiesController;
using espview::proto::CapabilitiesInfo;
using espview::proto::DisplayMode;
using espview::proto::MessageType;
using espview::proto::makeSetMode;
using espview::proto::PixelFormat;
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

CapabilitiesInfo makeCaps(uint16_t width, uint16_t height) {
    CapabilitiesInfo caps;
    caps.virtualPresent = true;
    caps.width = width;
    caps.height = height;
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

// 双侧对称重连（epoch 边界全量清槽；双方 decoder/encoder 基线一致复位），
// 保证后续 SET_MODE 往返无 seq 歧义。
void symmetricReconnect(espview::proto::test::CoordinatedHarness& h) {
    // 先消费残留队列：旧会话未消费的包（seq 基线已推进）会污染新会话的
    // decoder 基线，使新会话的首包被判 seq gap（确定性前置，防基线错位）。
    h.pumpBoth();
    h.a().onTransportDisconnected();
    h.b().onTransportDisconnected();
    h.a().onTransportConnected();
    h.b().onTransportConnected();
    h.pumpBoth();
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.b().state(), SessionState::kConnected);
}

// 一轮成功 SET_MODE 往返（结尾可判定：无悬挂 pending）。
void setModeRoundtrip(espview::proto::test::CoordinatedHarness& h) {
    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(h.b().acknowledge(0, 0, espview::proto::ErrorCode::kNone), SendResult::kOk);
    h.pumpA();
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 1u);
    h.clock.set(600);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
}

// D1：helloSlot replace-if-present + 排空恰好一次 + 结尾往返。
void deferred_hello_replace_and_drain_once() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();

    // A 单侧连接：HELLO 背压失败 → helloSlot（延迟未计数）。
    h.blockSinkA.store(true, std::memory_order_release);
    h.a().onTransportConnected();
    CHECK_EQ(h.a().state(), SessionState::kConnecting);
    CHECK_EQ(h.a().stats().txHello, 0u);

    // 再入槽（replace-if-present）：同会话至多 1 份。
    CHECK_EQ(h.a().sendHello(), SendResult::kBackpressure);
    CHECK_EQ(h.a().stats().txHello, 0u);

    // 排空失败（仍背压）：不清槽。
    h.tickA();
    CHECK_EQ(h.a().stats().txHello, 0u);

    // 放行 → 恰好一次排空（drain → completeHandshake，Connecting 分支）。
    h.blockSinkA.store(false, std::memory_order_release);
    h.tickA();
    CHECK_EQ(h.a().stats().txHello, 1u);
    CHECK_EQ(h.a().state(), SessionState::kConnected);

    // 再 tick 不重发（槽已清）。
    h.tickA();
    CHECK_EQ(h.a().stats().txHello, 1u);

    // B 连接 → 交换 HELLO → 双方 Connected（A 的 HELLO 已由 drain 发出、排队在
    // a2b_；B 收它完成握手）。
    h.b().onTransportConnected();
    h.pumpBoth();
    CHECK_EQ(h.b().state(), SessionState::kConnected);

    // 对称重连 → 结尾 SET_MODE 往返成功（无永久 pending）。
    symmetricReconnect(h);
    CHECK_EQ(h.a().stats().txHello, 2u);  // 1 次 drain + 1 次新会话主动 HELLO
    setModeRoundtrip(h);
}

// D2：helloSlot 随 disconnect 清空（epoch 边界）；新会话 HELLO 正常发出。
void deferred_hello_disconnect_clears() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();

    h.blockSinkA.store(true, std::memory_order_release);
    h.a().onTransportConnected();  // HELLO → helloSlot
    CHECK_EQ(h.a().stats().txHello, 0u);
    h.a().onTransportDisconnected();  // 槽清空（epoch 边界）

    h.blockSinkA.store(false, std::memory_order_release);
    h.a().onTransportConnected();  // 新会话：HELLO 立即发出
    CHECK_EQ(h.a().stats().txHello, 1u);
    h.b().onTransportConnected();  // B 侧正常
    h.pumpA();  // A 收 B 的 HELLO → 握手完成
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    h.pumpB();  // B 收 A 的 HELLO → B Connected
    CHECK_EQ(h.b().state(), SessionState::kConnected);

    h.tickA();  // 旧槽已清 → 不重复发送
    CHECK_EQ(h.a().stats().txHello, 1u);

    setModeRoundtrip(h);
}

// D3：capabilitiesSlot latest-wins + 恰好一次排空。
void deferred_capabilities_latest_wins() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> capsSeen{0};
    std::atomic<uint16_t> capsWidth{0};
    h.cbB.onCapabilities = [&capsSeen, &capsWidth](const espview::proto::CapabilitiesInfo& c) {
        capsSeen.fetch_add(1, std::memory_order_relaxed);
        capsWidth.store(c.width, std::memory_order_relaxed);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    h.blockSinkA.store(true, std::memory_order_release);
    CHECK_EQ(h.a().sendCapabilities(makeCaps(320, 240)), SendResult::kBackpressure);
    CHECK_EQ(h.a().sendCapabilities(makeCaps(640, 480)), SendResult::kBackpressure);
    CHECK_EQ(h.a().stats().txCapabilities, 0u);

    h.blockSinkA.store(false, std::memory_order_release);
    h.tickA();  // Connected 排空 → 只发最新
    CHECK_EQ(h.a().stats().txCapabilities, 1u);
    h.pumpB();
    CHECK_EQ(capsSeen.load(std::memory_order_relaxed), 1);
    CHECK_EQ(capsWidth.load(std::memory_order_relaxed), 640);

    h.tickA();  // 槽已清 → 不重发
    CHECK_EQ(h.a().stats().txCapabilities, 1u);
    h.pumpB();
    CHECK_EQ(capsSeen.load(std::memory_order_relaxed), 1);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
}

// D4：capabilitiesSlot 随 disconnect 清空；对称重连后重发正常。
void deferred_capabilities_disconnect_clears() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> capsSeen{0};
    h.cbB.onCapabilities = [&capsSeen](const espview::proto::CapabilitiesInfo&) {
        capsSeen.fetch_add(1, std::memory_order_relaxed);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    h.blockSinkA.store(true, std::memory_order_release);
    CHECK_EQ(h.a().sendCapabilities(makeCaps(320, 240)), SendResult::kBackpressure);
    CHECK_EQ(h.a().stats().txCapabilities, 0u);
    h.a().onTransportDisconnected();  // 槽清空（epoch 边界）
    h.blockSinkA.store(false, std::memory_order_release);

    // 双侧对称重连（基线一致）。
    symmetricReconnect(h);

    h.tickA();  // 旧槽已清 → 不发送
    CHECK_EQ(h.a().stats().txCapabilities, 0u);

    // 新会话重发正常：恰好 1 份到达 B。
    CHECK_EQ(h.a().sendCapabilities(makeCaps(640, 480)), SendResult::kOk);
    CHECK_EQ(h.a().stats().txCapabilities, 1u);
    h.pumpB();
    CHECK_EQ(capsSeen.load(std::memory_order_relaxed), 1);
    CHECK_EQ(h.a().state(), SessionState::kConnected);
}

// D5：hello + capabilities 均 deferred → 各自排空 → 对称重连后往返成功。
void deferred_combined_then_roundtrip() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::atomic<int> capsSeen{0};
    h.cbB.onCapabilities = [&capsSeen](const espview::proto::CapabilitiesInfo&) {
        capsSeen.fetch_add(1, std::memory_order_relaxed);
    };
    h.init();

    h.blockSinkA.store(true, std::memory_order_release);
    h.a().onTransportConnected();  // HELLO → helloSlot
    h.b().onTransportConnected();  // B 侧正常
    CHECK_EQ(h.a().stats().txHello, 0u);

    h.blockSinkA.store(false, std::memory_order_release);
    h.tickA();  // 排空 helloSlot → A Connected（drain 路径 completeHandshake）
    CHECK_EQ(h.a().stats().txHello, 1u);
    h.pumpA();  // A 收 B 的 HELLO → 重新确认（已 Connected）
    h.pumpB();  // B 收 A 的 HELLO → B Connected
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    CHECK_EQ(h.b().state(), SessionState::kConnected);
    CHECK_EQ(h.b().stats().rxHello, 1u);

    // capabilities 背压入槽 → Connected 排空 → B 收到恰好 1 份。
    h.blockSinkA.store(true, std::memory_order_release);
    CHECK_EQ(h.a().sendCapabilities(makeCaps(320, 240)), SendResult::kBackpressure);
    h.blockSinkA.store(false, std::memory_order_release);
    h.tickA();
    CHECK_EQ(h.a().stats().txCapabilities, 1u);
    h.pumpB();
    CHECK_EQ(capsSeen.load(std::memory_order_relaxed), 1);

    // 对称重连 → SET_MODE 往返（结尾无永久 pending）。
    symmetricReconnect(h);
    setModeRoundtrip(h);
}

}  // namespace

void runDeferredControlTests() {
    std::printf("  deferred_hello_replace_and_drain_once\n");
    deferred_hello_replace_and_drain_once();
    std::printf("  deferred_hello_disconnect_clears\n");
    deferred_hello_disconnect_clears();
    std::printf("  deferred_capabilities_latest_wins\n");
    deferred_capabilities_latest_wins();
    std::printf("  deferred_capabilities_disconnect_clears\n");
    deferred_capabilities_disconnect_clears();
    std::printf("  deferred_combined_then_roundtrip\n");
    deferred_combined_then_roundtrip();
}
