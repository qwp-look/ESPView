// ESPView M6-C — TransportManager Host Tests。
//
// 覆盖 M6-C 任务书 §二十八：1 select UART、2 select TCP、3 switch UART→TCP、
// 4 switch TCP→UART、5 switch while disconnected、6 switch failure、
// 7 幂等语义、8 发送门、9 data 转发/切换窗口。
// （8 FULL resync / 9 input state reset 的行为级验证在 transport_pipeline_test.cpp。）

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "../transport_manager.h"
#include "../transport_sink.h"
#include "test_util.h"
#include "transport_test_util.h"

namespace {

using espview::transport::ITransport;
using espview::transport::SendStatus;
using espview::transport::TransportCapabilities;
using espview::transport::TransportDiagSnapshot;
using espview::transport::TransportManager;
using espview::transport::TransportType;
using espview::transport::test::FakeTransport;
using espview::transport::test::tcpCaps;
using espview::transport::test::uartCaps;

// 共享开关：工厂创建 fake 时按其类型配置 open 成败。
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

// CS-3：异步迟到状态 fake —— close() 先投递 Disconnected，再经独立线程延迟
// ~20ms 投递 Connected，且 close() 阻塞到该迟到状态被投递（仍挂回调、仍在
// 切换窗口内），确定性复现"旧 Transport 在 closeLocked 期间产生的 stale 状态"。
class AsyncLateCloseFake : public espview::transport::test::FakeTransport {
public:
    using FakeTransport::FakeTransport;

    bool open() override {
        ++openCalls_;
        setState(State::kConnecting);
        setState(State::kConnected);
        return true;
    }

    void close() override {
        ++closeCalls_;
        setState(State::kDisconnected);
        std::thread t([this]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            setState(State::kConnected);  // 迟到状态（close() 返回前已投递）
        });
        t.join();
    }

    size_t closeCalls() const { return closeCalls_; }

private:
    size_t openCalls_ = 0;
    size_t closeCalls_ = 0;
};
}  // namespace

void runTransportManagerTests() {

    std::printf("[transport_manager]\n");

    // ---- 1. select UART（initial=UART）----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        CHECK_EQ(mgr.current(), TransportType::kUart);
        CHECK(!mgr.isOpen());
        CHECK(mgr.open());
        CHECK(mgr.isOpen());
        CHECK_EQ(owned.size(), 1u);
        CHECK_EQ(owned[0]->openCount(), 1u);
        CHECK_EQ(owned[0]->type(), TransportType::kUart);
        CHECK_EQ(states.size(), 1u);
        CHECK_EQ(states[0], ITransport::State::kConnected);
        CHECK_EQ(mgr.capabilities().paced, true);
        mgr.close();
        CHECK(!mgr.isOpen());
        CHECK_EQ(owned[0]->closeCount(), 1u);
    }

    // ---- 2. select TCP（initial=TCP）----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kTcp);
        CHECK_EQ(mgr.current(), TransportType::kTcp);
        CHECK(mgr.open());
        CHECK_EQ(owned[0]->type(), TransportType::kTcp);
        CHECK_EQ(mgr.capabilities().paced, false);
        mgr.close();
    }

    // ---- 2b. diagSnapshot（M7-B：OLED provider / statsLoop 快照）----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        mgr.open();
        owned[0]->setRxBytes(456);
        owned[0]->setReconnectCount(2);
        owned[0]->setApInfo(-55, 6);
        owned[0]->send(reinterpret_cast<const uint8_t*>("x"), 1);  // txBytes=1
        const TransportDiagSnapshot s1 = mgr.diagSnapshot();
        CHECK_EQ(s1.type, TransportType::kUart);
        CHECK(s1.connected);
        CHECK_EQ(s1.reconnectCount, 2u);
        CHECK_EQ(s1.txBytes, 1u);
        CHECK_EQ(s1.rxBytes, 456u);
        CHECK_EQ(s1.rssi, -55);
        CHECK_EQ(s1.channel, 6u);

        owned[0]->clearApInfo();
        const TransportDiagSnapshot s2 = mgr.diagSnapshot();
        CHECK_EQ(s2.rssi, -128);
        CHECK_EQ(s2.channel, 0u);

        mgr.close();
        const TransportDiagSnapshot s3 = mgr.diagSnapshot();
        CHECK(!s3.connected);
        CHECK_EQ(s3.reconnectCount, 0u);
        CHECK_EQ(s3.txBytes, 0u);
        CHECK_EQ(s3.rxBytes, 0u);
        CHECK_EQ(s3.rssi, -128);

        CHECK(mgr.switchTo(TransportType::kTcp));
        const TransportDiagSnapshot s4 = mgr.diagSnapshot();
        CHECK_EQ(s4.type, TransportType::kTcp);
        CHECK(s4.connected);
    }

    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        CHECK(mgr.open());
        states.clear();
        CHECK(mgr.switchTo(TransportType::kTcp));
        CHECK_EQ(mgr.current(), TransportType::kTcp);
        CHECK_EQ(mgr.switchCount(), 1u);
        CHECK_EQ(owned.size(), 2u);
        CHECK_EQ(owned[0]->closeCount(), 1u);  // 旧 UART 已 close
        CHECK_EQ(owned[1]->openCount(), 1u);   // 新 TCP 已 open
        // 切换：旧 close 的 Disconnected + 新 open 的 Connected（stale 状态不进入；CS-3）
        CHECK_EQ(states.size(), 2u);
        CHECK_EQ(states[0], ITransport::State::kDisconnected);  // 旧 close
        CHECK_EQ(states[1], ITransport::State::kConnected);     // 新 open
        CHECK(mgr.isOpen());
        mgr.close();
    }

    // ---- 4. switch TCP→UART ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kTcp);
        CHECK(mgr.open());
        CHECK(mgr.switchTo(TransportType::kUart));
        CHECK_EQ(mgr.current(), TransportType::kUart);
        CHECK_EQ(owned[0]->closeCount(), 1u);
        CHECK_EQ(owned[1]->type(), TransportType::kUart);
        mgr.close();
    }

    // ---- 5. switch while disconnected（首个 open 失败后仍可切换）----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        setup.uartOpenOk = false;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(!mgr.open());
        CHECK(!mgr.isOpen());
        CHECK_EQ(mgr.switchFailures(), 0u);  // open 失败不计 switch failure
        CHECK(mgr.switchTo(TransportType::kTcp));  // 仍可切换
        CHECK(mgr.isOpen());
        CHECK_EQ(mgr.current(), TransportType::kTcp);
        CHECK_EQ(owned[1]->openCount(), 1u);
        mgr.close();
    }

    // ---- 6. switch failure：新 Transport open 失败，reopen 可恢复 ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        setup.tcpOpenOk = false;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        CHECK(!mgr.switchTo(TransportType::kTcp));
        CHECK_EQ(mgr.switchFailures(), 1u);
        CHECK(!mgr.isOpen());
        CHECK_EQ(owned[0]->closeCount(), 1u);  // 旧已关闭
        CHECK_EQ(owned[1]->openCount(), 1u);   // 新尝试过 open（失败）
        CHECK_EQ(mgr.current(), TransportType::kTcp);
        setup.tcpOpenOk = true;
        CHECK(mgr.reopen());                   // 重试当前类型
        CHECK(mgr.isOpen());
        CHECK_EQ(owned.size(), 3u);
        CHECK_EQ(owned[2]->openCount(), 1u);
        mgr.close();
    }

    // ---- 7. 幂等：重复 open / 同类型 switchTo / 重复 close ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        CHECK(mgr.open());  // 幂等：不再新建
        CHECK_EQ(owned.size(), 1u);
        CHECK(mgr.switchTo(TransportType::kUart));  // 同类型已 open：幂等
        CHECK_EQ(owned.size(), 1u);
        mgr.close();
        mgr.close();  // 幂等
        CHECK_EQ(owned[0]->closeCount(), 1u);
    }

    // ---- 8. 发送门：lockTransport/unlockTransport 与切换互斥 ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        espview::transport::ITransport* t = mgr.lockTransport();
        CHECK(t != nullptr);
        const uint8_t b = 0xAB;
        CHECK_EQ(t->send(&b, 1), SendStatus::kOk);
        mgr.unlockTransport();
        mgr.close();
        CHECK(mgr.lockTransport() == nullptr);
    }

    // ---- 9. data 回调转发；切换后新 Transport 数据正常 ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        std::vector<uint8_t> rx;
        mgr.setDataCallback([&rx](const uint8_t* d, size_t n) { rx.insert(rx.end(), d, d + n); });
        CHECK(mgr.open());
        const uint8_t hello[] = {1, 2, 3};
        owned[0]->deliverRx(hello, sizeof(hello));
        CHECK_EQ(rx.size(), 3u);
        CHECK(mgr.switchTo(TransportType::kTcp));
        const uint8_t hello2[] = {9, 9};
        owned[1]->deliverRx(hello2, sizeof(hello2));
        CHECK_EQ(rx.size(), 5u);
        mgr.close();
    }

    // ---- 10. switchTo 幂等：切换后 current()/capabilities 反映新类型 ----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());
        CHECK(mgr.switchTo(TransportType::kTcp));
        CHECK(mgr.switchTo(TransportType::kTcp));  // 已在目标：幂等
        CHECK_EQ(mgr.switchCount(), 1u);
        mgr.close();
    }

    // ---- 11. M6-D §十八.8：rapid duplicate switch（快速连续切换，状态/计数一致）----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        CHECK(mgr.open());
        CHECK_EQ(states.size(), 1u);
        states.clear();  // 只观察切换阶段的状态序列
        CHECK(mgr.switchTo(TransportType::kTcp));
        CHECK(mgr.switchTo(TransportType::kUart));
        CHECK(mgr.switchTo(TransportType::kTcp));
        CHECK(mgr.switchTo(TransportType::kUart));
        CHECK_EQ(mgr.switchCount(), 4u);
        CHECK_EQ(mgr.current(), TransportType::kUart);
        CHECK(mgr.isOpen());
        CHECK_EQ(owned.size(), 5u);  // 初始 + 4 次切换各新建
        // 每次切换 = Disconnected + Connected 各一次（顺序固定，无乱序残留；CS-3 stale 丢弃）
        CHECK_EQ(states.size(), 8u);
        for (size_t i = 0; i < states.size(); i += 2) {
            CHECK_EQ(states[i], ITransport::State::kDisconnected);
            CHECK_EQ(states[i + 1], ITransport::State::kConnected);
        }
        mgr.close();
    }

    // ---- 12. M6-D §十八.14：stale state cleared（旧 Transport 切换后残留状态不转发）----
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        CHECK(mgr.open());
        states.clear();
        CHECK(mgr.switchTo(TransportType::kTcp));
        states.clear();
        // 旧 UART Transport 已 detach：其后续状态回调不得到达上层
        owned[0]->setState(ITransport::State::kError);
        owned[0]->setState(ITransport::State::kConnected);
        CHECK_EQ(states.size(), 0u);  // stale 状态被隔离
        // 新 TCP Transport 正常转发
        owned[1]->setState(ITransport::State::kDisconnected);
        CHECK_EQ(states.size(), 1u);
        CHECK_EQ(states[0], ITransport::State::kDisconnected);
        mgr.close();
    }

    // ---- 13. CS-3：closeLocked 期间旧 Transport 的迟到状态不得重放进新会话 ----
    {
        std::vector<std::shared_ptr<AsyncLateCloseFake>> owned;
        TransportManager mgr(
            [&owned](TransportType t) -> std::shared_ptr<espview::transport::ITransport> {
                auto f = std::make_shared<AsyncLateCloseFake>(
                    t, t == TransportType::kUart ? uartCaps() : tcpCaps());
                owned.emplace_back(f);
                return f;  // shared_ptr: manager holds a reference
            },
            TransportType::kUart);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        CHECK(mgr.open());
        CHECK_EQ(owned.size(), 1u);
        CHECK_EQ(states.size(), 2u);  // 初始 open：Connecting + Connected
        CHECK_EQ(states[0], ITransport::State::kConnecting);
        CHECK_EQ(states[1], ITransport::State::kConnected);
        states.clear();
        CHECK(mgr.switchTo(TransportType::kTcp));
        // 旧 UART close() 的迟到 Connected 被 clearPending 丢弃（stale）；会话结束的
        // Disconnected 保留；新 TCP 的 Connecting/Connected 按序投递，无 stale 状态。
        CHECK_EQ(states.size(), 3u);
        CHECK_EQ(states[0], ITransport::State::kDisconnected);
        CHECK_EQ(states[1], ITransport::State::kConnecting);
        CHECK_EQ(states[2], ITransport::State::kConnected);
        CHECK_EQ(mgr.current(), TransportType::kTcp);
        CHECK(mgr.isOpen());
        // 证明测试覆盖了迟到路径：旧 fake 确实投递了 Disconnected + 迟到 Connected。
        CHECK_EQ(owned[0]->stateLog().size(), 4u);
        CHECK_EQ(owned[0]->stateLog()[0], ITransport::State::kConnecting);
        CHECK_EQ(owned[0]->stateLog()[1], ITransport::State::kConnected);
        CHECK_EQ(owned[0]->stateLog()[2], ITransport::State::kDisconnected);
        CHECK_EQ(owned[0]->stateLog()[3], ITransport::State::kConnected);
        mgr.close();  // 独立 close：Disconnected 直接投递（行为不变）
    }

    // ---- 14. M7-B：diagSnapshot vs switchTo 并发压力（值语义快照不越界）----
    // 一个线程循环 switchTo(UART→TCP→UART…)，另一个线程循环 diagSnapshot() 并
    // 校验返回值不越界（type 为合法枚举、tx/rx 任意 uint64）；验证快照为锁内
    // 值语义拷贝，不暴露裸指针（原 transport() 锁外解引用在 switchTo 并发下 UAF）。
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        CHECK(mgr.open());  // switchTo 要求 initial open
        std::atomic<bool> stopFlag{false};
        std::atomic<bool> invalidSeen{false};
        std::atomic<uint64_t> sampleCount{0};
        std::thread switcher([&]() {
            while (!stopFlag.load(std::memory_order_relaxed)) {
                mgr.switchTo(TransportType::kTcp);
                mgr.switchTo(TransportType::kUart);
            }
        });
        std::thread sampler([&]() {
            while (!stopFlag.load(std::memory_order_relaxed)) {
                const TransportDiagSnapshot s = mgr.diagSnapshot();
                // 快照值语义：type 必须落在合法枚举内；tx/rx 为任意 uint64（只读）。
                if (s.type != TransportType::kUart && s.type != TransportType::kTcp) {
                    invalidSeen.store(true, std::memory_order_relaxed);
                }
                const uint64_t tx = s.txBytes;
                const uint64_t rx = s.rxBytes;
                (void)tx;
                (void)rx;
                sampleCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
        // 跑足够轮数（约 2 秒；两线程并发 switchTo/diagSnapshot）。
        std::this_thread::sleep_for(std::chrono::seconds(2));
        stopFlag.store(true, std::memory_order_relaxed);
        switcher.join();
        sampler.join();
        CHECK(!invalidSeen.load(std::memory_order_relaxed));
        CHECK(sampleCount.load(std::memory_order_relaxed) >= 100u);  // 并发采样确实发生
        mgr.close();
    }

    // ---- 15. M8-A3：UART reconnect 序列（close → reopen 语义）----
    // close() 后可再次 open/reopen；工厂每次创建全新 Transport 实例（旧实例
    // close + 回调 detach），状态按 Disconnected → Connecting/Connected 投递，
    // 旧实例迟到状态不得到达上层（stale 隔离）。
    {
        std::vector<std::shared_ptr<FakeTransport>> owned;
        FakeSetup setup;
        TransportManager mgr = makeManager(owned, setup, TransportType::kUart);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        CHECK(mgr.open());
        CHECK_EQ(owned.size(), 1u);
        states.clear();

        mgr.close();  // 断开：Disconnected 直接投递（非切换缓冲路径）
        CHECK(!mgr.isOpen());
        CHECK_EQ(states.size(), 1u);
        CHECK_EQ(states[0], ITransport::State::kDisconnected);
        states.clear();

        // reopen：工厂新建实例 → open → Connected 缓冲后冲刷。
        CHECK(mgr.reopen());
        CHECK(mgr.isOpen());
        CHECK_EQ(owned.size(), 2u);              // 新实例
        CHECK_EQ(owned[1]->openCount(), 1u);
        CHECK_EQ(owned[0]->closeCount(), 1u);    // 旧实例已 close
        CHECK_EQ(states.size(), 1u);
        CHECK_EQ(states[0], ITransport::State::kConnected);
        CHECK_EQ(mgr.current(), TransportType::kUart);

        // 旧实例后续状态不得到达上层（回调已 detach）。
        states.clear();
        owned[0]->setState(ITransport::State::kError);
        owned[0]->setState(ITransport::State::kConnected);
        CHECK_EQ(states.size(), 0u);
        mgr.close();
    }

    // ---- 16. M8-A3：adopt（PC TCP Server accept 路径）----
    // adopt 已激活的 Transport：不调用 open()，直接挂回调并设为当前；
    // 已 open 时拒绝（不做隐式替换）；adopt 后发送门/能力/diag 全部可用。
    {
        TransportManager mgr(
            [](TransportType) -> std::shared_ptr<espview::transport::ITransport> {
                return nullptr;  // adopt 路径不使用工厂
            },
            TransportType::kTcp);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        auto accepted = std::make_shared<FakeTransport>(TransportType::kTcp, tcpCaps());
        accepted->setConnectedState(true);  // M8-A3：adopt 校验 isConnected()（attach 已激活）
        accepted->setState(ITransport::State::kConnected);  // attach 已完成（回调未挂，丢弃）
        CHECK(mgr.adopt(TransportType::kTcp, accepted));
        CHECK(mgr.isOpen());
        CHECK_EQ(mgr.current(), TransportType::kTcp);
        CHECK_EQ(accepted->openCount(), 0u);  // 未调用 open()
        CHECK(mgr.capabilities().paced == false);
        // 已 open：再次 adopt 拒绝（调用方须先 close）。
        auto second = std::make_shared<FakeTransport>(TransportType::kTcp, tcpCaps());
        CHECK(!mgr.adopt(TransportType::kTcp, second));
        CHECK_EQ(second->openCount(), 0u);
        // adopt 后发送门可用（发送走 current_）。
        CHECK(mgr.lockTransport() != nullptr);
        mgr.unlockTransport();
        mgr.close();
    }

    // ---- 17. M8-A3：adopt 拒绝未激活 Transport（身份级断言：状态不变）----
    // adopt 校验 t->isConnected()：未连接（accept 后立即断开等）必须拒绝，
    // 且不改变 manager 任何状态；后续激活的 transport 仍可正常 adopt。
    {
        TransportManager mgr(
            [](TransportType) -> std::shared_ptr<espview::transport::ITransport> {
                return nullptr;  // adopt 路径不使用工厂
            },
            TransportType::kTcp);
        std::vector<ITransport::State> states;
        mgr.setStateCallback([&states](ITransport::State s) { states.push_back(s); });
        CHECK(!mgr.isOpen());
        auto dead = std::make_shared<FakeTransport>(TransportType::kTcp, tcpCaps());
        CHECK(!dead->isConnected());
        CHECK(!mgr.adopt(TransportType::kTcp, dead));  // 未激活：拒绝
        CHECK(!mgr.isOpen());
        CHECK(mgr.transport() == nullptr);       // current 未被替换
        CHECK_EQ(states.size(), 0u);             // 无状态冲刷（失败不改状态）
        auto live = std::make_shared<FakeTransport>(TransportType::kTcp, tcpCaps());
        live->setConnectedState(true);
        CHECK(mgr.adopt(TransportType::kTcp, live));  // 激活后可正常 adopt
        CHECK(mgr.isOpen());
        CHECK(mgr.transport() == live.get());
        mgr.close();
    }

    std::printf("[transport_manager] done\n");
}
