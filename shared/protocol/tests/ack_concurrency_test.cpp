// ESPView M8-A2 — ACK 并发竞态确定性测试（ack_concurrency_test.cpp）。
//
// 覆盖 6 类 ACK 竞态（DESIGN.md AN 章 R1-R6）：
//   A1 timeout 边界（499/500/501ms 精确 deadline 判定）
//   A2 ACK + disconnect 在途（两种顺序：先 ACK 后断开 / 先断开后 ACK）
//   A3 ACK 重试 + reconnect（Faraday：重试不得覆盖新会话的 pendingAck 槽）
//   A4 duplicate ACK（同一 pending 至多触发一次 onAck）
//   A5 wrong ackSeq（错配只计数、不清槽、不回调）
//   A6 stale ACK 跨会话 seq 复用（无 live pending 时不触发 onAck）
// 断言精确的 ackSent/ackReceived/ackRetries/ackFailures/onAck 次数。
// 纯 C++17；并发经 CoordinatedHarness + testHooks 确定性编排。

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

using espview::proto::DisplayMode;
using espview::proto::makeSetMode;
using espview::proto::MessageType;
using espview::proto::ProtocolEndpoint;
using espview::proto::SendResult;
using espview::proto::SessionState;

// 打开全部测试闸门（握手等前置流程不被钉住）。
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

// A1：ACK timeout 边界 —— 499ms 不重试、500ms 首次重试、501ms 不重试
// （deadline 已顺延）、1000ms 第二次重试、1500ms 耗尽 onAckTimeout。
void ack_timeout_boundary_499_500_501() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());
    CHECK_EQ(h.b().state(), SessionState::kConnected);

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    CHECK_EQ(h.a().stats().ackSent, 0u);
    CHECK_EQ(h.a().stats().ackReceived, 0u);

    h.clock.set(499);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 0u);  // 未到 500ms deadline

    h.clock.set(500);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 1u);  // 首次重试（attempts 1→2）
    CHECK_EQ(h.a().stats().ackFailures, 0u);

    h.clock.set(501);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 1u);  // deadline 已顺延到 1000ms

    h.clock.set(1000);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 2u);  // 第二次重试（attempts 2→3）

    h.clock.set(1500);
    h.tickA();
    CHECK_EQ(h.a().stats().ackFailures, 1u);  // 耗尽（1+2=3 次已发）
    CHECK_EQ(h.ackTimeoutCount(), 1u);

    h.clock.set(2000);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 2u);  // 耗尽后不再重试
    CHECK_EQ(h.a().stats().ackFailures, 1u);
}

// A2a：ACK 先于断开被处理 → onAck 恰好一次、pending 清空。
void ack_processed_then_disconnect() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::vector<std::pair<uint8_t, uint16_t>> reqs;
    h.cbB.onAckRequest = [&reqs](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
        reqs.emplace_back(t, s);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kMirror)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(reqs.size(), 1u);
    CHECK_EQ(h.b().acknowledge(reqs[0].second, 0, espview::proto::ErrorCode::kNone),
             SendResult::kOk);
    h.pumpA();  // ACK 先落地
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 1u);
    CHECK_EQ(h.b().stats().ackSent, 1u);

    h.a().onTransportDisconnected();
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);
    // 断开后再喂 ACK：消息被会话层丢弃（仅 HELLO 可触发被动恢复），不计数不回调。
    h.b().acknowledge(reqs[0].second, 0, espview::proto::ErrorCode::kNone);
    h.pumpA();
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 1u);
}

// A2b：断开先于 ACK 被处理 → ACK 被会话层丢弃（不计数不回调）。
void disconnect_then_ack_inflight() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::vector<std::pair<uint8_t, uint16_t>> reqs;
    h.cbB.onAckRequest = [&reqs](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
        reqs.emplace_back(t, s);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kSplit)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(reqs.size(), 1u);
    CHECK_EQ(h.b().acknowledge(reqs[0].second, 0, espview::proto::ErrorCode::kNone),
             SendResult::kOk);
    // ACK 字节已进 b2a_，但 A 尚未消费。
    CHECK(!h.b2a_.empty());

    // 先断开（pendingAck 清空、epoch++），再消费 ACK → handleMessage 丢弃。
    h.a().onTransportDisconnected();
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);
    h.pumpA();
    CHECK_EQ(h.a().stats().ackReceived, 0u);
    CHECK_EQ(h.onAckCount(), 0u);
    CHECK_EQ(h.b().stats().ackSent, 1u);
}

// A3：ACK 重试在途 + disconnect/reconnect —— 旧会话重试不得覆盖新会话的
// pendingAck 槽（Faraday「重试覆盖新 slot」；seq+epoch 复查）。
void ack_retry_does_not_overwrite_new_slot() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    // 会话 1：SET_MODE（seq=0）→ pendingAck{epoch:1, deadline:500}。
    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    const int sinkBase = h.sinkCallsA.load(std::memory_order_acquire);

    // 关闭 sink 保持闸门：让 tick 重试"发送在途"。
    h.sinkHoldA.close();
    h.clock.set(500);
    std::thread ticker([&h] { h.tickA(); });
    // 等 tick 进入发送段（sinkCallsA 增长）。
    while (h.sinkCallsA.load(std::memory_order_acquire) == sinkBase) {
        std::this_thread::yield();
    }

    // 会话 2：断开（epoch→2，pendingAck 清空）+ 重连握手（epoch→3）。
    // 注意：旧重试持有 sendMutex_（sink 内阻塞）——A 重连的 sendHello 走 tryTransmit
    // → try_lock 失败立即 kBackpressure 进 helloSlot，绝不阻塞（D→T blocking 边禁止）。
    h.a().onTransportDisconnected();
    CHECK_EQ(h.a().state(), SessionState::kDisconnected);
    h.a().onTransportConnected();
    CHECK_EQ(h.b().sendHello(), SendResult::kOk);
    h.pumpA();  // A 收 B 的 HELLO → 握手完成（A 自身 HELLO 在 helloSlot 暂存）
    CHECK_EQ(h.a().state(), SessionState::kConnected);

    // 放行旧重试：afterSend(seq+epoch 复查) 必须跳过——旧会话 epoch(1) != 3，
    // 且此刻 pendingAck_ 尚空：不递增 attempts、不复活任何登记。
    h.sinkHoldA.open();
    ticker.join();
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
    CHECK_EQ(h.ackTimeoutCount(), 0u);

    // 新会话 SET_MODE → 登记 pendingAck{epoch:3}；随后重试恰好一次（槽完好）。
    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kSplit)), SendResult::kOk);
    const uint64_t base = h.clock.now.load(std::memory_order_relaxed);
    h.clock.set(base + 501);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 1u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
}

// A4：duplicate ACK —— 同一 pending 至多触发一次 onAck；后续重复只计数。
void duplicate_ack_no_double_on_ack() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::vector<std::pair<uint8_t, uint16_t>> reqs;
    h.cbB.onAckRequest = [&reqs](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
        reqs.emplace_back(t, s);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(reqs.size(), 1u);
    CHECK_EQ(h.b().acknowledge(reqs[0].second, 0, espview::proto::ErrorCode::kNone),
             SendResult::kOk);
    CHECK_EQ(h.b().acknowledge(reqs[0].second, 0, espview::proto::ErrorCode::kNone),
             SendResult::kOk);  // 重复 ACK（B 侧 seq 递增，均可解码）
    h.pumpA();
    CHECK_EQ(h.a().stats().ackReceived, 2u);
    CHECK_EQ(h.onAckCount(), 1u);       // 第一次匹配 → onAck；第二次无 pending
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);

    h.clock.set(600);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 0u);  // pending 已清 → 无重试
    CHECK_EQ(h.ackTimeoutCount(), 0u);
}

// A5：wrong ackSeq —— 错配只 ++ackReceived，不清槽、不回调；重试照常。
void wrong_ack_seq_keeps_pending() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::vector<std::pair<uint8_t, uint16_t>> reqs;
    h.cbB.onAckRequest = [&reqs](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
        reqs.emplace_back(t, s);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kMirror)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(reqs.size(), 1u);
    // 回错 ackSeq（真实请求 seq + 1）：A 不应清 pending、不应回调。
    const uint16_t wrongSeq = static_cast<uint16_t>(reqs[0].second + 1);
    CHECK_EQ(h.b().acknowledge(wrongSeq, 0, espview::proto::ErrorCode::kNone), SendResult::kOk);
    h.pumpA();
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 0u);

    h.clock.set(600);
    h.tickA();  // pending 仍在 → 正常重试
    CHECK_EQ(h.a().stats().ackRetries, 1u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
}

// A6：stale ACK 跨会话 + seq 复用 —— 无 live pending 时 stale ACK 只计数、
// 不触发 onAck；live pending 至多匹配一次（重复 ACK 不双回调）。
void stale_ack_across_session_seq_reuse() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    std::vector<std::pair<uint8_t, uint16_t>> reqs;
    h.cbB.onAckRequest = [&reqs](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
        reqs.emplace_back(t, s);
    };
    h.init();
    CHECK(h.connectAndHandshake());

    // 会话 1：SET_MODE（seq=0）→ pendingAck{epoch:1}；B 的 onAckRequest 给出真实
    // ackSeq。stale ACK 不用 B.acknowledge 入队：FIFO 会把它排在 B 的新 HELLO
    // 之前，A 消费后基线推进导致 HELLO 被判 seq gap，握手无法完成——改用独立
    // 编码器在会话 1 生成真实 wire 字节，选定时刻直接喂给 A（精确 interleaving；
    // 字节本身即 wire 上的在途 ACK）。
    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    h.pumpB();
    CHECK_EQ(reqs.size(), 1u);
    espview::proto::SequenceCounter staleSeq;
    espview::proto::MessageEncoder staleEnc(staleSeq);
    const auto staleAck = [&h, &staleEnc, &reqs](int) {
        // 每次 encode 消耗一个新 wire seq（0,1,2,...），可被 A 递增基线解码。
        std::vector<std::vector<uint8_t>> pkts;
        // makeAck 直接返回 Message（非 optional；布局恒合法）。
        const espview::proto::Message msg = espview::proto::makeAck(
            reqs[0].second, 0, espview::proto::ErrorCode::kNone);
        CHECK_EQ(staleEnc.encode(msg, pkts), espview::proto::PacketError::kNone);
        for (const auto& p : pkts) {
            h.a().onTransportData(p.data(), p.size());
        }
    };

    // 断开（epoch→2、pendingAck 清空）→ A 重连（epoch→3）；B 对称重连复位
    // encoder/decoder（新会话 HELLO 从 seq 0 起，A 复位后的基线 0 才能解码）。
    h.a().onTransportDisconnected();
    h.a().onTransportConnected();
    h.b().onTransportDisconnected();
    h.b().onTransportConnected();
    h.pumpA();  // B 的 HELLO(0) → 握手完成（A 基线 0）
    CHECK_EQ(h.a().state(), SessionState::kConnected);
    h.pumpB();  // B 收 A 的 HELLO(0) → Connected（encoder 复位）

    // 场景 A：无 live pending 时 stale ACK（会话 1 的 wire 字节）→ 只计数不回调
    // （epoch 防护：旧会话 ACK 不得配对任何东西）。
    staleAck(0);
    CHECK_EQ(h.a().stats().ackReceived, 1u);
    CHECK_EQ(h.onAckCount(), 0u);

    // 会话 2：新 SET_MODE（seq 复用到 0）→ live pendingAck。
    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);
    CHECK_EQ(h.a().stats().ackReceived, 1u);  // 尚无新 ACK

    // 场景 B：相同 wire payload 的 ACK（seq 复用，wire 层无法区分归属）到达并
    // 匹配 live pending → onAck 恰好一次（跨会话 seq 复用是 wire 固有的歧义，
    // 无法避免——pending 的 epoch 就是当前会话，wire 上无纪元可携带）。
    staleAck(1);
    CHECK_EQ(h.a().stats().ackReceived, 2u);
    CHECK_EQ(h.onAckCount(), 1u);

    // 场景 C：重复 ACK（pending 已清）→ 只计数不回调。
    staleAck(2);
    CHECK_EQ(h.a().stats().ackReceived, 3u);
    CHECK_EQ(h.onAckCount(), 1u);

    h.clock.set(600);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, 0u);
    CHECK_EQ(h.a().stats().ackFailures, 0u);
    CHECK_EQ(h.ackTimeoutCount(), 0u);
}

// M8-A2 MED-1：旧重试在途 + 同会话同消息（type+payload 相同）换槽 —— 新槽
// attempts/ackRetries 绝不被旧重试误增；新请求保留完整重试预算。attempts 递增
// 已并入 afterSend 的 epoch+seq 守卫分支（旧 tick 复查只看 epoch+type+payload，
// 会误增换槽后新槽的 attempts——可观测判据：重试预算少一次）。
void ack_retry_does_not_corrupt_replaced_slot_attempts() {
    espview::proto::test::CoordinatedHarness h;
    openAllGates(h);
    h.init();
    CHECK(h.connectAndHandshake());

    // 会话内：SET_MODE（seq=0）→ 槽 {seq:0, epoch:1, attempts:1, deadline:500}。
    CHECK_EQ(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)), SendResult::kOk);

    // 重试在途：sink 保持（tick 重试持有 sendMutex_ 并阻塞在 sink）。
    h.sinkHoldA.close();
    h.clock.set(500);
    const int sinkBase = h.sinkCallsA.load(std::memory_order_acquire);
    std::thread ticker([&h] { h.tickA(); });
    while (h.sinkCallsA.load(std::memory_order_acquire) == sinkBase) {
        std::this_thread::yield();
    }
    // app 再次发送相同 SET_MODE（同消息换槽；sendMessage 阻塞在 sendMutex_ 上，
    // 旧重试放行后才完成——换槽 seq 必为新值 S2）。
    std::atomic<SendResult> appR{SendResult::kTransportError};
    std::thread app([&h, &appR] {
        appR.store(h.a().sendMessage(makeSetMode(DisplayMode::kWindow)),
                   std::memory_order_release);
    });
    // 放行旧重试 → 重试完成后 app 换槽。join 顺序不影响结论：attempts 递增
    // 只在「槽仍持有同一 epoch+seq」的守卫分支内，新槽（seq S2）结构上不可能
    // 被旧重试（retrySeq=S1）命中。
    h.sinkHoldA.open();
    ticker.join();
    app.join();
    CHECK_EQ(appR.load(std::memory_order_acquire), SendResult::kOk);

    // 旧重试至多结算 1 次（tick 先于 app 换槽时）——绝不影响新槽预算。
    const uint64_t retriesBase = h.a().stats().ackRetries;
    CHECK(retriesBase <= 1u);

    // 新请求保留完整重试预算：恰好可重试 2 次，第 3 次到期才耗尽。
    // （旧实现：新槽 attempts 被误增 → 第 1 次重试后即耗尽，ackFailures 提前。）
    h.clock.set(1000);  // 新槽 deadline = 500+500
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, retriesBase + 1u);   // 首次重试（预算 1/2）
    h.clock.set(1500);
    h.tickA();
    CHECK_EQ(h.a().stats().ackRetries, retriesBase + 2u);   // 第二次重试（预算 2/2）
    CHECK_EQ(h.a().stats().ackFailures, 0u);                // 预算完好：未提前耗尽
    CHECK_EQ(h.ackTimeoutCount(), 0u);
    h.clock.set(2000);
    h.tickA();
    CHECK_EQ(h.a().stats().ackFailures, 1u);                // 耗尽 → 恰好一次回调
    CHECK_EQ(h.ackTimeoutCount(), 1u);
}

}  // namespace
// 供 test_main.cpp 注册。
void runAckConcurrencyTests() {
    std::printf("  ack_timeout_boundary_499_500_501\n");
    ack_timeout_boundary_499_500_501();
    std::printf("  ack_processed_then_disconnect\n");
    ack_processed_then_disconnect();
    std::printf("  disconnect_then_ack_inflight\n");
    disconnect_then_ack_inflight();
    std::printf("  ack_retry_does_not_overwrite_new_slot\n");
    ack_retry_does_not_overwrite_new_slot();
    std::printf("  duplicate_ack_no_double_on_ack\n");
    duplicate_ack_no_double_on_ack();
    std::printf("  wrong_ack_seq_keeps_pending\n");
    wrong_ack_seq_keeps_pending();
    std::printf("  stale_ack_across_session_seq_reuse\n");
    stale_ack_across_session_seq_reuse();
    std::printf("  ack_retry_does_not_corrupt_replaced_slot_attempts\n");
    ack_retry_does_not_corrupt_replaced_slot_attempts();
}
