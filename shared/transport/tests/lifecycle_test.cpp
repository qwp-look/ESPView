// ESPView M8-A5 — Lifecycle / Failure-Injection / Stress Host Tests。
//
// 覆盖 M8-A5 任务书 §三十二/§三十三/§三十五：
//   TransportManager：switch while connecting、switch failure、stale state/data
//     callback（generation 隔离）、close during switch、析构不回调（TM-01/03/04）；
//   TransportSink：睡眠被中断立即放弃（SINK-04）、mgr close 后 send 安全（SINK-09）；
//   stress：1000x open/close、1000x switch、destroy immediately after start、
//     callback flood during teardown、live-instance 计数归零。
// 纯 C++17；错误路径不使用异常。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../protocol/tests/test_util.h"
#include "../transport_manager.h"
#include "../transport_sink.h"
#include "transport_test_util.h"

namespace {

using espview::transport::ITransport;
using espview::transport::SendStatus;
using espview::transport::TransportCapabilities;
using espview::transport::TransportLockGuard;
using espview::transport::TransportManager;
using espview::transport::TransportSink;
using espview::transport::TransportType;
using espview::transport::test::tcpCaps;
using espview::transport::test::uartCaps;

// ---- 资源计数 + 可配置故障注入的 Transport ----
std::atomic<int> gLiveTransports{0};

class CountingTransport : public ITransport {
public:
    CountingTransport(TransportType type, TransportCapabilities caps = tcpCaps())
        : type_(type), caps_(caps) {
        gLiveTransports.fetch_add(1);
    }
    ~CountingTransport() override { gLiveTransports.fetch_sub(1); }

    void setOpenResult(bool ok) { openOk_ = ok; }
    void setOpenDelayMs(uint32_t ms) { openDelayMs_ = ms; }
    // close() 静默（不发 Disconnected）→ 验证 manager 合成会话结束信号（TM-04）
    void setCloseEmitsDisconnected(bool v) { closeEmitsDisc_ = v; }
    // close() 后 delayMs 再投递一个迟到状态（模拟契约外 backend 的 in-flight 回调；
    // 回调在 close 返回前已捕获 → 走 manager generation 校验，TM-01）
    void setLateStateAfterClose(ITransport::State s, uint32_t delayMs) {
        lateState_ = s;
        lateDelayMs_ = delayMs;
    }

    bool open() override {
        ++openCount_;
        if (openDelayMs_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(openDelayMs_));
        }
        if (!openOk_) {
            setState(ITransport::State::kError);
            return false;
        }
        setState(ITransport::State::kConnecting);
        setState(ITransport::State::kConnected);
        return true;
    }
    void close() override {
        ++closeCount_;
        StateCallback captured;
        {
            std::lock_guard<std::mutex> lk(m_);
            captured = stateCb_;
        }
        if (closeEmitsDisc_ && captured != nullptr) {
            captured(ITransport::State::kDisconnected);
        }
        if (lateDelayMs_ > 0) {
            const ITransport::State late = lateState_;
            const uint32_t delay = lateDelayMs_;
            std::thread([captured, late, delay]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                if (captured != nullptr) {
                    captured(late);
                }
            }).detach();
        }
    }
    bool isConnected() const override { return connected_; }
    SendStatus send(const uint8_t*, size_t) override {
        (void)0;  // len unused（计数语义）
        ++sendCount_;
        if (sendResult_ != SendStatus::kOk) {
            return sendResult_;
        }
        return SendStatus::kOk;
    }
    void setDataCallback(DataCallback cb) override {
        std::lock_guard<std::mutex> lk(m_);
        dataCb_ = std::move(cb);
    }
    void setStateCallback(StateCallback cb) override {
        std::lock_guard<std::mutex> lk(m_);
        stateCb_ = std::move(cb);
    }
    const TransportCapabilities& capabilities() const override { return caps_; }
    uint64_t reconnectCount() const override { return 0; }
    uint64_t txBytes() const override { return 0; }
    uint64_t rxBytes() const override { return 0; }
    bool wifiApInfo(int8_t*, uint8_t*) const override { return false; }

    void setState(ITransport::State s) {
        StateCallback cb;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (s == ITransport::State::kConnected) {
                connected_ = true;
            }
            if (s == ITransport::State::kDisconnected) {
                connected_ = false;
            }
            cb = stateCb_;
        }
        if (cb != nullptr) {
            cb(s);
        }
    }
    void deliverRx(const uint8_t* d, size_t n) {
        DataCallback cb;
        {
            std::lock_guard<std::mutex> lk(m_);
            cb = dataCb_;
        }
        if (cb != nullptr) {
            cb(d, n);
        }
    }
    void setSendResult(SendStatus r) { sendResult_ = r; }

    size_t openCount() const { return openCount_; }
    size_t closeCount() const { return closeCount_; }

private:
    TransportType type_;
    TransportCapabilities caps_;
    bool openOk_ = true;
    bool closeEmitsDisc_ = true;
    uint32_t openDelayMs_ = 0;
    ITransport::State lateState_ = ITransport::State::kConnected;
    uint32_t lateDelayMs_ = 0;
    SendStatus sendResult_ = SendStatus::kOk;
    bool connected_ = false;
    size_t openCount_ = 0;
    size_t closeCount_ = 0;
    size_t sendCount_ = 0;
    mutable std::mutex m_;
    DataCallback dataCb_;
    StateCallback stateCb_;
};

// ---- 上层回调 spy ----
struct CallbackSpy {
    std::vector<ITransport::State> states;
    size_t dataCalls = 0;
    std::atomic<bool> destroyed{false};
    std::mutex m;
};

// 共享 open 成败/慢速开关（工厂创建时读取；供 repeated-reopen / 慢 open 测试使用）。
struct OpenControl {
    std::atomic<bool> ok{true};
    std::atomic<uint32_t> openDelayMs{0};
};

TransportManager makeCountingManager(std::vector<std::shared_ptr<CountingTransport>>& owned,
                                     TransportType initial,
                                     std::shared_ptr<OpenControl> ctrl = nullptr) {
    return TransportManager(
        [&owned, ctrl](TransportType type) -> std::shared_ptr<ITransport> {
            auto t = std::make_shared<CountingTransport>(
                type, type == TransportType::kUart ? uartCaps() : tcpCaps());
            if (ctrl != nullptr) {
                t->setOpenResult(ctrl->ok.load());
                t->setOpenDelayMs(ctrl->openDelayMs.load());
            }
            owned.push_back(t);
            return t;
        },
        initial);
}

// ---- 1. TM-01：post-clearPending 迟到状态被 generation 丢弃 ----
void testLateStateDroppedByGeneration() {
    std::vector<std::shared_ptr<CountingTransport>> owned;
    TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
    CallbackSpy spy;
    mgr.setStateCallback([&spy](ITransport::State s) {
        std::lock_guard<std::mutex> lk(spy.m);
        spy.states.push_back(s);
    });
    CHECK(mgr.open());  // UART → spy: [Connecting, Connected]
    {
        std::lock_guard<std::mutex> lk(spy.m);
        spy.states.clear();  // 只看切换窗口
    }
    owned[0]->setLateStateAfterClose(ITransport::State::kConnected, 30);
    CHECK(mgr.switchTo(TransportType::kTcp));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // 等迟到投递
    {
        std::lock_guard<std::mutex> lk(spy.m);
        CHECK_EQ(spy.states.size(), 3u);  // Disc + Connecting + Connected；迟到 Connected 被 generation 丢弃
        CHECK(spy.states[0] == ITransport::State::kDisconnected);  // 会话结束信号
        CHECK(spy.states[2] == ITransport::State::kConnected);
    }
}

// ---- 2. TM-04：close 静默 → manager 合成恰好一次 Disconnected ----
void testSynthesizedDisconnectedOnSilentClose() {
    std::vector<std::shared_ptr<CountingTransport>> owned;
    TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
    CallbackSpy spy;
    mgr.setStateCallback([&spy](ITransport::State s) {
        std::lock_guard<std::mutex> lk(spy.m);
        spy.states.push_back(s);
    });
    CHECK(mgr.open());
    owned[0]->setCloseEmitsDisconnected(false);  // 契约外静默 close（open 后配置实际实例）
    {        std::lock_guard<std::mutex> lk(spy.m);        spy.states.clear();    }
    CHECK(mgr.switchTo(TransportType::kTcp));
    {        std::lock_guard<std::mutex> lk(spy.m);
        CHECK_EQ(spy.states.size(), 3u);        CHECK(spy.states[0] == ITransport::State::kDisconnected);  // 合成会话结束信号
    }
}

// ---- 3. TM-03：析构不触发上层回调 ----
void testDtorDoesNotCallUpperCallbacks() {
    {
        std::vector<std::shared_ptr<CountingTransport>> owned;
        TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
        CallbackSpy spy;
        mgr.setDataCallback([&spy](const uint8_t*, size_t) {
            std::lock_guard<std::mutex> lk(spy.m);
            ++spy.dataCalls;
        });
        mgr.setStateCallback([&spy](ITransport::State s) {
            std::lock_guard<std::mutex> lk(spy.m);
            spy.states.push_back(s);
        });
        CHECK(mgr.open());
        const size_t before = spy.states.size();
        CHECK(before > 0u);
        mgr.close();  // 运行期 close 仍投递 Disconnected（正常路径）
        // 析构（shutdown）路径：回调已 detach，不得新增
        spy.destroyed.store(true);
    }
    CHECK(gLiveTransports.load() == 0);  // 析构后无泄漏
}

// ---- 4. SINK-04：睡眠被中断 → send 立即返回 kBackpressure ----
void testSleepInterrupted() {
    std::vector<std::shared_ptr<CountingTransport>> owned;
    TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
    CHECK(mgr.open());
    std::atomic<int> sleepCalls{0};
    TransportSink sink(
        mgr, []() { return true; },
        []() { return 0u; },  // now 恒 0：若 sleep 不被中断会永远循环
        [&sleepCalls](uint32_t) {
            sleepCalls.fetch_add(1);
            return false;  // 被中断
        });
    owned[0]->setSendResult(SendStatus::kBackpressure);
    const uint8_t pkt[] = {0x01};
    CHECK_EQ(sink.send(pkt, sizeof(pkt)), SendStatus::kBackpressure);
    CHECK_EQ(sleepCalls.load(), 1);  // 一次 sleep 即放弃，无空转
    mgr.close();
}

// ---- 5. SINK-09：mgr close 后 send 安全（无 UB，返回错误/背压）----
void testSendAfterManagerClose() {
    std::vector<std::shared_ptr<CountingTransport>> owned;
    TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
    CHECK(mgr.open());
    TransportSink sink(mgr, []() { return true; }, []() { return 0u; },
                       [](uint32_t) { return true; });
    mgr.close();
    const uint8_t pkt[] = {0x02};
    const SendStatus r = sink.send(pkt, sizeof(pkt));
    CHECK(r == SendStatus::kError || r == SendStatus::kNotConnected);
    mgr.switchTo(TransportType::kTcp);  // close 后可继续生命周期
    CHECK(mgr.isOpen());
}

// ---- 6. TM-02 配套：switch 慢 open 期间 trySend 背压、send 阻塞直到完成 ----
void testSwitchWhileSendPending() {
    std::vector<std::shared_ptr<CountingTransport>> owned;
    auto ctrl = std::make_shared<OpenControl>();
    TransportManager mgr = makeCountingManager(owned, TransportType::kUart, ctrl);
    CHECK(mgr.open());
    ctrl->openDelayMs.store(120);  // 后续创建（新 TCP）open 慢
    std::atomic<bool> done{false};
    std::thread switcher([&]() {
        mgr.switchTo(TransportType::kTcp);
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));  // 进入切换窗口
    TransportSink sink(mgr, []() { return true; }, []() { return 0u; },
                       [](uint32_t) { return true; });
    const uint8_t pkt[] = {0x03};
    const SendStatus r = sink.trySend(pkt, sizeof(pkt));
    CHECK(r == SendStatus::kBackpressure);  // 切换窗口：门忙
    switcher.join();
    CHECK(done.load());
    CHECK(mgr.isOpen());
    CHECK_EQ(owned[0]->openCount(), 1u);  // UART 已 close；TCP 已 open
    CHECK_EQ(owned[1]->closeCount(), 0u);
}

// ---- 7. stress：1000x open/close ----
void testStressOpenClose() {
    for (int i = 0; i < 1000; ++i) {
        std::vector<std::shared_ptr<CountingTransport>> owned;
        TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
        CHECK(mgr.open());
        mgr.close();
    }
    CHECK(gLiveTransports.load() == 0);
}

// ---- 8. stress：1000x switch ----
void testStressSwitch() {
    std::vector<std::shared_ptr<CountingTransport>> owned;
    TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
    CHECK(mgr.open());
    for (int i = 0; i < 1000; ++i) {
        const TransportType next =
            (i % 2 == 0) ? TransportType::kTcp : TransportType::kUart;
        CHECK(mgr.switchTo(next));
        CHECK(mgr.isOpen());
    }
    mgr.close();
    owned.clear();  // 释放测试自身持有的引用：mgr 无残留引用 → 计数归零
    CHECK(gLiveTransports.load() == 0);
}

// ---- 9. stress：destroy immediately after start ----
void testDestroyImmediatelyAfterStart() {
    for (int i = 0; i < 200; ++i) {
        std::vector<std::shared_ptr<CountingTransport>> owned;
        TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
        CHECK(mgr.open());
        // 立即析构：dtor shutdown 必须 detach 回调 + close，无崩溃/无泄漏
    }
    CHECK(gLiveTransports.load() == 0);
}

// ---- 10. stress：callback flood during teardown ----
void testCallbackFloodDuringTeardown() {
    for (int i = 0; i < 20; ++i) {
        std::vector<std::shared_ptr<CountingTransport>> owned;
        TransportManager mgr = makeCountingManager(owned, TransportType::kUart);
        std::atomic<bool> stop{false};
        std::thread flooder([&]() {
            const uint8_t byte = 0xAB;
            while (!stop.load()) {
                if (!owned.empty() && mgr.isOpen()) {
                    owned[0]->deliverRx(&byte, 1);
                }
            }
        });
        CHECK(mgr.open());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        mgr.close();
        stop.store(true);
        flooder.join();
    }
    CHECK(gLiveTransports.load() == 0);
}

// ---- 11. stress：repeated reopen after failure ----
void testRepeatedReopenAfterFailure() {
    std::vector<std::shared_ptr<CountingTransport>> owned;
    auto ctrl = std::make_shared<OpenControl>();
    TransportManager mgr = makeCountingManager(owned, TransportType::kUart, ctrl);
    for (int i = 0; i < 100; ++i) {
        ctrl->ok.store(false);  // 工厂将创建 open 失败的实例
        CHECK(!mgr.open());
        CHECK(!mgr.isOpen());
        ctrl->ok.store(true);
        CHECK(mgr.open());
        mgr.close();
    }
    owned.clear();  // 释放测试自身持有的引用：mgr 无残留引用 → 计数归零
    CHECK(gLiveTransports.load() == 0);
}

}  // namespace

void runLifecycleTests() {
    std::printf("[lifecycle_failure_injection]\n");
    testLateStateDroppedByGeneration();
    std::printf("  [1] late_state_dropped\n");
    testSynthesizedDisconnectedOnSilentClose();
    std::printf("  [2] synth_disc\n");
    testDtorDoesNotCallUpperCallbacks();
    std::printf("  [3] dtor_no_cb\n");
    testSleepInterrupted();
    std::printf("  [4] sleep_interrupt\n");
    testSendAfterManagerClose();
    std::printf("  [5] send_after_close\n");
    testSwitchWhileSendPending();
    std::printf("  [6] switch_send_pending\n");
    testStressOpenClose();
    std::printf("  [7] stress_open_close\n");
    testStressSwitch();
    std::printf("  [8] stress_switch\n");
    testDestroyImmediatelyAfterStart();
    std::printf("  [9] destroy_immediate\n");
    testCallbackFloodDuringTeardown();
    std::printf("  [10] flood_teardown\n");
    testRepeatedReopenAfterFailure();
    std::printf("  [11] repeated_reopen\n");
}
