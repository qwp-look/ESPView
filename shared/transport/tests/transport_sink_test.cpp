// ESPView M6-C — TransportSink / TxPolicy Host Tests。
//
// 覆盖 M6-C 任务书 §二十八 pacing 测试：
//   10. UART policy（paced：背压重试 + 睡眠间隔）
//   11. TCP policy（unpaced：单次尝试，不做 UART 式 sleep）
//   12. bounded send（sink 不缓冲；背压即返回，上层整帧丢弃）
//   13. 门忙时 trySend 立即背压（control traffic 不被大帧饿死/阻塞）
//   14. trySend 正常路径
//   15. UART 背压超时兜底（deadline → kBackpressure）

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "../transport_manager.h"
#include "../transport_sink.h"
#include "test_util.h"
#include "transport_test_util.h"

namespace {

using espview::transport::ITransport;
using espview::transport::SendStatus;
using espview::transport::TransportManager;
using espview::transport::TransportSink;
using espview::transport::TransportType;
using espview::transport::test::FakeTransport;
using espview::transport::test::tcpCaps;
using espview::transport::test::uartCaps;

struct FakeSetup {
    bool uartOpenOk = true;
    bool tcpOpenOk = true;
};

TransportManager makeManager(std::vector<std::shared_ptr<FakeTransport>>& owned,
                             FakeSetup& setup, TransportType initial) {
    return TransportManager(
        [&owned, &setup](TransportType t) -> std::shared_ptr<espview::transport::ITransport> {
            auto f = std::make_shared<FakeTransport>(t, t == TransportType::kUart ? uartCaps() : tcpCaps());
            f->setOpenResult(t == TransportType::kUart ? setup.uartOpenOk : setup.tcpOpenOk);
            owned.emplace_back(f);
            return f;  // shared_ptr: manager holds a reference
        },
        initial);
}

// 假时钟 + 假 sleep：sleep 推进时钟（等价时间流逝），并记录间隔。
struct ClockAndSleep {
    uint64_t now = 0;
    std::vector<uint32_t> sleeps;
    uint64_t operator()() { return now; }
    bool operator()(uint32_t ms) {
        sleeps.push_back(ms);
        now += ms;
        return true;  // M8-A5：Sleep 返回 bool（true=完成，false=被中断）
    }
};

}  // namespace

void runTransportSinkTests() {

    std::printf("[transport_sink]\n");

    // ---- 10. UART policy：背压重试至成功，间隔 = 5ms ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return true; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        owned[0]->setSendSequence({SendStatus::kBackpressure, SendStatus::kBackpressure,
                                   SendStatus::kBackpressure, SendStatus::kOk});
        const uint8_t pkt[] = {0xAA, 0xBB};
        CHECK_EQ(sink.send(pkt, sizeof(pkt)), SendStatus::kOk);
        CHECK_EQ(owned[0]->sendCount(), 4u);
        CHECK_EQ(cs.sleeps.size(), 3u);
        CHECK_EQ(cs.sleeps[0], 5u);
        CHECK_EQ(cs.sleeps[1], 5u);
        CHECK_EQ(cs.sleeps[2], 5u);
        mgr.close();
    }

    // ---- 11. TCP policy：单次尝试，背压立即返回，零 sleep ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kTcp);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return true; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        owned[0]->setSendSequence({SendStatus::kBackpressure});
        const uint8_t pkt[] = {0x11, 0x22};
        CHECK_EQ(sink.send(pkt, sizeof(pkt)), SendStatus::kBackpressure);
        CHECK_EQ(owned[0]->sendCount(), 1u);
        CHECK_EQ(cs.sleeps.size(), 0u);  // 无 UART 式 sleep
        mgr.close();
    }

    // ---- 12. UART 背压超时兜底：deadline 后返回 kBackpressure（bounded）----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return true; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        owned[0]->setSendResult(SendStatus::kBackpressure);  // 永远背压
        const uint8_t pkt[] = {0x01};
        CHECK_EQ(sink.send(pkt, sizeof(pkt)), SendStatus::kBackpressure);
        // 30s / 5ms = 6000 次 sleep + 首次发送；无无限缓冲/无限循环。
        CHECK_EQ(owned[0]->sendCount(), 6001u);
        CHECK_EQ(cs.sleeps.size(), 6000u);
        CHECK_EQ(cs.now, 30000u);
        mgr.close();
    }

    // ---- 13. 会话死亡（alive=false）→ kNotConnected，不触碰 Transport ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return false; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        const uint8_t pkt[] = {0x01};
        CHECK_EQ(sink.send(pkt, sizeof(pkt)), SendStatus::kNotConnected);
        CHECK_EQ(owned[0]->sendCount(), 0u);
        mgr.close();
    }

    // ---- 14. 门忙（大帧发送中）→ trySend 立即 kBackpressure，不阻塞 ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return true; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        // 模拟 TX 任务正持有发送门（大帧发送中）。
        espview::transport::ITransport* held = mgr.lockTransport();
        CHECK(held != nullptr);
        const uint8_t pkt[] = {0x55};
        const auto t0 = std::chrono::steady_clock::now();
        const SendStatus r = sink.trySend(pkt, sizeof(pkt));
        const auto t1 = std::chrono::steady_clock::now();
        CHECK_EQ(r, SendStatus::kBackpressure);
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        CHECK(elapsedUs < 20000);  // 立即返回（20ms 内；实际 ~0）
        CHECK_EQ(owned[0]->sendCount(), 0u);  // 未触碰 Transport
        mgr.unlockTransport();
        mgr.close();
    }

    // ---- 15. trySend 正常路径：门空闲 → kOk ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return true; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        const uint8_t pkt[] = {0x66, 0x77};
        CHECK_EQ(sink.trySend(pkt, sizeof(pkt)), SendStatus::kOk);
        CHECK_EQ(owned[0]->sendCount(), 1u);
        mgr.close();
    }

    // ---- 16. M8-A3：WouldBlock ≠ disconnect（kBackpressure 不改变会话/管理器状态）----
    // TCP 语义：send 超时 = would-block（背压），Transport 仍 connected、管理器
    // 仍 open；上层按 TxPolicy 整帧丢弃，绝不视为断线（kBackpressure ≠ kNotConnected）。
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kTcp);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return true; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        owned[0]->setSendResult(SendStatus::kBackpressure);
        const uint8_t pkt[] = {0x01, 0x02};
        CHECK_EQ(sink.send(pkt, sizeof(pkt)), SendStatus::kBackpressure);
        CHECK(mgr.isOpen());
        CHECK(owned[0]->isConnected());
        // 未投递任何断开/错误状态（stateLog 仍只有 open 时的 kConnected）。
        CHECK_EQ(owned[0]->stateLog().size(), 1u);
        CHECK_EQ(owned[0]->stateLog()[0], ITransport::State::kConnected);
        // 未连接 ≠ 背压：关闭后 trySend 返回 kNotConnected（区分两种语义；
        // 阻塞式 send 在未 open 时返回 kError，trySend 的 isOpen 检查给出
        // 明确的 kNotConnected）。
        mgr.close();
        CHECK_EQ(sink.trySend(pkt, sizeof(pkt)), SendStatus::kNotConnected);
    }

    // ---- 17. M8-A3：send failure 注入（kError）不被重试、不视为背压 ----
    // paced（UART）：kError 立即终止重试循环（非背压不 sleep）；上层按
    // Transport 层错误处理（可能触发重连策略），而非整帧背压重试。
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        ClockAndSleep cs;
        TransportSink sink(mgr, []() { return true; },
                           [&cs]() { return cs.now; }, [&cs](uint32_t ms) { cs(ms); return true; });
        owned[0]->setSendResult(SendStatus::kError);
        const uint8_t pkt[] = {0xAA};
        CHECK_EQ(sink.send(pkt, sizeof(pkt)), SendStatus::kError);
        CHECK_EQ(owned[0]->sendCount(), 1u);  // 单次尝试，无背压重试
        CHECK_EQ(cs.sleeps.size(), 0u);       // 无 UART 式 sleep
        mgr.close();
    }

    std::printf("[transport_sink] done\n");
}
