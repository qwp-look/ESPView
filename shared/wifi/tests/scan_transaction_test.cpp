// ESPView M7-E — ScanTransaction Host Tests（Agent K）。
//
// 规范来源：_m7e_contract.md「ScanTransaction」测试清单 1..10：
//   1 success（suspend->scan->done->resume 一次）
//   2 scan fail（onScanStarted(false) 与 onScanDone(false) 两种失败）
//   3 timeout（tick 驱动 + 直接 onTimeout；kPreparing 直接 kError）
//   4 double suspend（begin 幂等，不重复 suspend）
//   5 double resume（终态后再触发不重复 resume）
//   6 错误路径（suspend 失败 -> kError，不 resume；空回调保守失败）
//   7 retry（kIdle 后可再次 begin；kError 后可再次 begin）
//   8 session disconnect（恢复 + 停 kDisconnected，不自动回 Idle，需外部 begin）
//   9 最终 OLED 必回 Active（MockOLED 校验 suspend/resume 配对与 Active 终态）
//  10 超时恢复 + 可再次 begin
//
// MockOLED / MockWifi 仅存在于本测试，不进生产代码。纯 C++17，零平台依赖；
// 独立可执行（CMake 目标 scan_transaction_test，定义 SCAN_TRANSACTION_TEST_MAIN
// 提供 main），亦可并入 shared/protocol host 套件（去掉该定义后由 test_main.cpp
// 调用 runScanTransactionTests()）。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "scan_transaction.h"
#include "test_util.h"

namespace {

using espview::wifi::ScanPhase;
using espview::wifi::ScanTransaction;
using espview::wifi::ScanTransactionCallbacks;
using espview::wifi::scanPhaseName;

// ---- MockOLED：记录 pause/resume 配对；可注入暂停失败 ----
// pauseCount_ 计调用次数（含失败尝试）；resumeAfterWifiScan() 幂等（未挂起不计数）。
class MockOLED {
public:
    bool pauseResult = true;  // 注入 suspend 失败用

    bool pauseForWifiScan() {
        ++pauseCount_;
        if (!pauseResult) {
            return false;  // 暂停失败：不置 suspended
        }
        suspended_ = true;
        return true;
    }

    void resumeAfterWifiScan() {
        if (!suspended_) {
            return;  // 幂等：未挂起不计数
        }
        ++resumeCount_;
        suspended_ = false;
    }

    bool suspended() const { return suspended_; }
    int pauseCount() const { return pauseCount_; }
    int resumeCount() const { return resumeCount_; }
    bool backToActive() const { return !suspended_; }
    // 严格配对：仅用于所有 pause 均成功的会话（失败尝试不计入配对）。
    bool balanced() const { return !suspended_ && pauseCount_ == resumeCount_; }

private:
    int pauseCount_ = 0;
    int resumeCount_ = 0;
    bool suspended_ = false;
};

ScanTransactionCallbacks makeCallbacks(MockOLED& oled) {
    ScanTransactionCallbacks cb;
    cb.suspendDisplay = [&oled]() { return oled.pauseForWifiScan(); };
    cb.resumeDisplay = [&oled]() { oled.resumeAfterWifiScan(); };
    return cb;
}

// ---- MockWifi：模拟驱动（Agent C 侧）的调用序列 ----
class MockWifi {
public:
    MockWifi(MockOLED& oled, ScanTransaction& tx) : oled_(oled), tx_(tx) {}

    bool requestScan() {
        tx_.begin();
        return tx_.phase() != ScanPhase::kError;
    }
    void scanStarted(bool suspendedOk) { tx_.onScanStarted(suspendedOk); }
    void scanDone(bool ok) { tx_.onScanDone(ok); }
    void timeout() { tx_.onTimeout(); }
    void disconnect() { tx_.onDisconnect(); }
    void tick(uint64_t nowMs, uint64_t timeoutMs) { tx_.tick(nowMs, timeoutMs); }

    MockOLED& oled() { return oled_; }
    ScanTransaction& tx() { return tx_; }

private:
    MockOLED& oled_;
    ScanTransaction& tx_;
};

// ---- 1. success：suspend -> scan -> done -> resume 恰一次 ----
void successFlow() {
    MockOLED oled;
    ScanTransaction tx(makeCallbacks(oled));
    MockWifi wifi(oled, tx);

    CHECK(tx.phase() == ScanPhase::kIdle);
    CHECK(!tx.displaySuspended());
    CHECK(oled.backToActive());

    CHECK(wifi.requestScan());  // begin -> kPreparing，pause#1
    CHECK(tx.phase() == ScanPhase::kPreparing);
    CHECK(tx.displaySuspended());
    CHECK_EQ(oled.pauseCount(), 1);
    CHECK_EQ(oled.resumeCount(), 0);

    wifi.scanStarted(true);  // kDisplaySuspended
    CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
    CHECK(tx.displaySuspended());
    CHECK(oled.suspended());

    wifi.scanDone(true);  // kCollecting -> kRestoring -> kIdle，resume#1
    CHECK(tx.phase() == ScanPhase::kIdle);
    CHECK(!tx.displaySuspended());
    CHECK_EQ(oled.pauseCount(), 1);
    CHECK_EQ(oled.resumeCount(), 1);
    CHECK(oled.backToActive());
    CHECK(oled.balanced());
}

// ---- 2. scan fail：start 失败与结果失败都恢复恰一次 ----
void scanFailures() {
    // 2a. onScanStarted(false)：扫描未启动 -> kError + 恢复（begin 内 suspend 已成功）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        CHECK(wifi.requestScan());
        CHECK(tx.phase() == ScanPhase::kPreparing);
        wifi.scanStarted(false);
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());
        CHECK_EQ(oled.pauseCount(), 1);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.backToActive());
    }
    // 2b. onScanDone(false)：结果收集失败 -> kError（!ok 但已恢复）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        CHECK(wifi.requestScan());
        wifi.scanStarted(true);
        CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
        wifi.scanDone(false);
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());
        CHECK_EQ(oled.pauseCount(), 1);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.backToActive());
        CHECK(oled.balanced());
    }
    // 2c. kError 后可直接 retry（stale 事件不干扰新会话）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanDone(false);  // stale：非扫描窗口，no-op（仍在 kPreparing）
        CHECK(tx.phase() == ScanPhase::kPreparing);
        wifi.scanStarted(true);
        wifi.scanDone(false);
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(wifi.requestScan());  // retry from kError
        CHECK(tx.phase() == ScanPhase::kPreparing);
        wifi.scanStarted(true);
        wifi.scanDone(true);
        CHECK(tx.phase() == ScanPhase::kIdle);
        CHECK_EQ(oled.pauseCount(), 2);
        CHECK_EQ(oled.resumeCount(), 2);
        CHECK(oled.balanced());
    }
}

// ---- 3. timeout：tick 看门狗 + 直接 onTimeout；kPreparing 直接 kError ----
void timeoutFlow() {
    // 3a. tick 驱动：首 tick 锚定，跨过 timeoutMs 即超时
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanStarted(true);
        wifi.tick(1000, 500);  // 锚定起点
        CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
        wifi.tick(1400, 500);  // 400 < 500：未超时
        CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
        wifi.tick(1500, 500);  // 500 >= 500：超时
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());
        CHECK_EQ(oled.pauseCount(), 1);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.balanced());
    }
    // 3b. 直接 onTimeout（驱动看门狗路径）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanStarted(true);
        wifi.timeout();
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.balanced());
    }
    // 3c. kPreparing 直接 kError（begin 内 suspend 已成功，仍恢复——核心不变量）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        CHECK(tx.phase() == ScanPhase::kPreparing);
        wifi.timeout();
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());
        CHECK_EQ(oled.pauseCount(), 1);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.balanced());
    }
    // 3d. timeoutMs==0 禁用；tick 在 kIdle/kPreparing 不生效
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.tick(99999, 100);  // kIdle：no-op
        CHECK(tx.phase() == ScanPhase::kIdle);
        wifi.requestScan();  // kPreparing：tick no-op
        wifi.tick(99999, 100);
        CHECK(tx.phase() == ScanPhase::kPreparing);
        wifi.scanStarted(true);
        wifi.tick(5000, 0);  // timeoutMs==0：不超时
        CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
        wifi.scanDone(true);
        CHECK(tx.phase() == ScanPhase::kIdle);
        CHECK(oled.balanced());
    }
    // 3e. 时钟回拨防御
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanStarted(true);
        wifi.tick(2000, 500);
        wifi.tick(1000, 500);  // nowMs 回拨：不超时
        CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
        wifi.scanDone(true);
        CHECK(tx.phase() == ScanPhase::kIdle);
        CHECK(oled.balanced());
    }
}

// ---- 3b. begin() 返回挂起结果（M7-G：驱动据此决定是否启动扫描）----
void beginReturnsSuspendResult() {
    // 成功：true，kPreparing + 已挂起
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        CHECK(tx.begin());
        CHECK(tx.phase() == ScanPhase::kPreparing);
        CHECK(tx.displaySuspended());
        tx.onScanStarted(true);
        tx.onScanDone(true);
        CHECK(tx.phase() == ScanPhase::kIdle);
        CHECK(oled.balanced());
    }
    // 失败：false，kError + 未挂起（不 resume）
    {
        MockOLED oled;
        oled.pauseResult = false;
        ScanTransaction tx(makeCallbacks(oled));
        CHECK(!tx.begin());
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());
        CHECK_EQ(oled.pauseCount(), 1);
        CHECK_EQ(oled.resumeCount(), 0);
        CHECK(oled.backToActive());
    }
    // 活动相位重复 begin：false（no-op，不重复 suspend）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        CHECK(tx.begin());
        CHECK(!tx.begin());  // kPreparing：no-op
        CHECK_EQ(oled.pauseCount(), 1);
        tx.onScanStarted(true);
        CHECK(!tx.begin());  // kDisplaySuspended：no-op
        CHECK_EQ(oled.pauseCount(), 1);
        tx.onScanDone(true);
        CHECK(tx.phase() == ScanPhase::kIdle);
        CHECK(oled.balanced());
    }
}

// ---- 4. double suspend（begin 幂等，不重复 suspend）----
void doubleBeginIdempotent() {
    MockOLED oled;
    ScanTransaction tx(makeCallbacks(oled));
    MockWifi wifi(oled, tx);

    CHECK(wifi.requestScan());
    CHECK_EQ(oled.pauseCount(), 1);
    CHECK(tx.phase() == ScanPhase::kPreparing);

    tx.begin();  // double begin（活动相位）：no-op
    CHECK(tx.phase() == ScanPhase::kPreparing);
    CHECK_EQ(oled.pauseCount(), 1);
    CHECK(tx.displaySuspended());

    wifi.scanStarted(true);
    CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
    tx.begin();  // 扫描中 double begin：no-op
    CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
    CHECK_EQ(oled.pauseCount(), 1);

    wifi.scanStarted(true);  // double onScanStarted：no-op
    CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
    CHECK_EQ(oled.pauseCount(), 1);

    wifi.scanDone(true);
    CHECK(tx.phase() == ScanPhase::kIdle);
    CHECK_EQ(oled.pauseCount(), 1);
    CHECK_EQ(oled.resumeCount(), 1);
    CHECK(oled.balanced());
}

// ---- 5. double resume（终态后再触发不重复 resume）----
void doubleResumeIdempotent() {
    // 5a. 成功终态后再触发各类终态事件：resume 仍为 1
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanStarted(true);
        wifi.scanDone(true);
        CHECK_EQ(oled.resumeCount(), 1);
        wifi.timeout();       // kIdle：no-op
        wifi.scanDone(true);  // kIdle：no-op
        CHECK_EQ(oled.resumeCount(), 1);
        wifi.disconnect();    // 任何相位 -> kDisconnected；未挂起则不再 resume
        CHECK(tx.phase() == ScanPhase::kDisconnected);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.balanced());
    }
    // 5b. onTimeout 后再 onDisconnect：总共 resume 恰一次
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanStarted(true);
        wifi.timeout();
        CHECK_EQ(oled.resumeCount(), 1);
        wifi.disconnect();  // kError -> kDisconnected；未挂起不再 resume
        CHECK(tx.phase() == ScanPhase::kDisconnected);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(!tx.displaySuspended());
    }
    // 5c. double onDisconnect：第二次 no-op
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanStarted(true);
        wifi.disconnect();
        CHECK_EQ(oled.resumeCount(), 1);
        wifi.disconnect();
        CHECK(tx.phase() == ScanPhase::kDisconnected);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.backToActive());
    }
}

// ---- 6. 错误路径：suspend 失败 -> kError，不 resume；空回调保守失败 ----
void suspendFailure() {
    // 6a. pauseForWifiScan() 返回 false
    {
        MockOLED oled;
        oled.pauseResult = false;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);

        CHECK(!wifi.requestScan());  // begin -> kError
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());  // 不进入 suspended
        CHECK_EQ(oled.pauseCount(), 1);
        CHECK_EQ(oled.resumeCount(), 0);  // 从未挂起：不 resume
        CHECK(oled.backToActive());

        wifi.disconnect();  // 未挂起：不 resume
        CHECK(tx.phase() == ScanPhase::kDisconnected);
        CHECK_EQ(oled.resumeCount(), 0);
        CHECK(oled.backToActive());
    }
    // 6b. 修复后 begin 可恢复
    {
        MockOLED oled;
        oled.pauseResult = false;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        CHECK(!wifi.requestScan());
        CHECK(tx.phase() == ScanPhase::kError);
        oled.pauseResult = true;
        CHECK(wifi.requestScan());  // retry from kError
        CHECK(tx.phase() == ScanPhase::kPreparing);
        wifi.scanStarted(true);
        wifi.scanDone(true);
        CHECK(tx.phase() == ScanPhase::kIdle);
        CHECK_EQ(oled.resumeCount(), 1);
        CHECK(oled.backToActive());
    }
    // 6c. 回调未注入：suspendDisplay 为空视同暂停失败（保守，绝不无保护扫描）
    {
        ScanTransaction tx;  // 无回调
        CHECK(!tx.begin());
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(!tx.displaySuspended());
    }
}

// ---- 7. retry：kIdle 后可再次 begin；kError 后可再次 begin ----
void retry() {
    // 7a. 成功后再 begin（连续多会话）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        for (int session = 0; session < 2; ++session) {
            CHECK(wifi.requestScan());
            CHECK(tx.phase() == ScanPhase::kPreparing);
            wifi.scanStarted(true);
            wifi.scanDone(true);
            CHECK(tx.phase() == ScanPhase::kIdle);
        }
        CHECK_EQ(oled.pauseCount(), 2);
        CHECK_EQ(oled.resumeCount(), 2);
        CHECK(oled.balanced());
    }
    // 7b. 失败后再次 begin（kError -> 新会话）
    {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        MockWifi wifi(oled, tx);
        wifi.requestScan();
        wifi.scanStarted(true);
        wifi.scanDone(false);
        CHECK(tx.phase() == ScanPhase::kError);
        CHECK(wifi.requestScan());
        CHECK(tx.phase() == ScanPhase::kPreparing);
        wifi.scanStarted(true);
        wifi.scanDone(true);
        CHECK(tx.phase() == ScanPhase::kIdle);
        CHECK_EQ(oled.pauseCount(), 2);
        CHECK_EQ(oled.resumeCount(), 2);
        CHECK(oled.balanced());
    }
}

// ---- 8. session disconnect：恢复 + 停 kDisconnected，不自动回 Idle ----
void disconnectSession() {
    MockOLED oled;
    ScanTransaction tx(makeCallbacks(oled));
    MockWifi wifi(oled, tx);

    wifi.requestScan();
    wifi.scanStarted(true);
    CHECK(tx.displaySuspended());
    wifi.disconnect();
    CHECK(tx.phase() == ScanPhase::kDisconnected);
    CHECK(!tx.displaySuspended());
    CHECK_EQ(oled.pauseCount(), 1);
    CHECK_EQ(oled.resumeCount(), 1);
    CHECK(oled.balanced());

    // 不自动回 Idle；tick 不复活
    CHECK(tx.phase() != ScanPhase::kIdle);
    wifi.tick(5000, 100);
    CHECK(tx.phase() == ScanPhase::kDisconnected);
    CHECK_EQ(oled.resumeCount(), 1);

    // 外部显式 begin 恢复新会话（从 kDisconnected 启动）
    CHECK(wifi.requestScan());
    CHECK(tx.phase() == ScanPhase::kPreparing);
    CHECK(tx.displaySuspended());
    wifi.scanStarted(true);
    wifi.scanDone(true);
    CHECK(tx.phase() == ScanPhase::kIdle);
    CHECK_EQ(oled.pauseCount(), 2);
    CHECK_EQ(oled.resumeCount(), 2);
    CHECK(oled.balanced());
}

// ---- 9. 最终 OLED 必回 Active：各终态路径后 MockOLED 均回到 Active ----
void oledAlwaysBackActive() {
    struct Scenario {
        const char* name;
        void (*run)(MockOLED&, ScanTransaction&);
    };
    const Scenario scenarios[] = {
        {"success",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onScanStarted(true);
             tx.onScanDone(true);
         }},
        {"scan_fail",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onScanStarted(true);
             tx.onScanDone(false);
         }},
        {"scan_start_fail",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onScanStarted(false);
         }},
        {"timeout_tick",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onScanStarted(true);
             tx.tick(1000, 500);
             tx.tick(1600, 500);
         }},
        {"timeout_direct",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onScanStarted(true);
             tx.onTimeout();
         }},
        {"timeout_preparing",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onTimeout();
         }},
        {"disconnect",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onScanStarted(true);
             tx.onDisconnect();
         }},
        {"disconnect_from_preparing",
         [](MockOLED&, ScanTransaction& tx) {
             tx.begin();
             tx.onDisconnect();
         }},
        {"suspend_failure",
         [](MockOLED& oled, ScanTransaction& tx) {
             oled.pauseResult = false;
             tx.begin();
         }},
    };
    for (const Scenario& s : scenarios) {
        MockOLED oled;
        ScanTransaction tx(makeCallbacks(oled));
        s.run(oled, tx);
        CHECK_MSG(oled.backToActive(), s.name);
        CHECK_MSG(!tx.displaySuspended(), s.name);
        // 成功挂起的会话必须 resume 恰好一次；仅 suspend 失败场景允许 resume==0。
        if (std::strcmp(s.name, "suspend_failure") != 0) {
            CHECK_EQ(oled.pauseCount(), oled.resumeCount());
        } else {
            CHECK_EQ(oled.resumeCount(), 0);
        }
    }
}

// ---- 10. 超时恢复 + 可再次 begin ----
void timeoutThenRetry() {
    MockOLED oled;
    ScanTransaction tx(makeCallbacks(oled));
    MockWifi wifi(oled, tx);

    // 会话 1：tick 超时 -> kError + 恢复
    wifi.requestScan();
    wifi.scanStarted(true);
    wifi.tick(1000, 500);  // 锚定
    wifi.tick(1700, 500);  // 700 >= 500 -> 超时
    CHECK(tx.phase() == ScanPhase::kError);
    CHECK(!tx.displaySuspended());
    CHECK_EQ(oled.resumeCount(), 1);
    CHECK(oled.backToActive());

    // 会话 2：再次 begin 正常完成
    CHECK(wifi.requestScan());
    CHECK(tx.phase() == ScanPhase::kPreparing);
    wifi.scanStarted(true);
    wifi.tick(2000, 500);  // 新窗口重新锚定（旧时钟已重置）
    wifi.tick(2300, 500);  // 300 < 500：未误超时
    CHECK(tx.phase() == ScanPhase::kDisplaySuspended);
    wifi.scanDone(true);
    CHECK(tx.phase() == ScanPhase::kIdle);
    CHECK_EQ(oled.pauseCount(), 2);
    CHECK_EQ(oled.resumeCount(), 2);
    CHECK(oled.balanced());
}

// 附加：scanPhaseName 调试名（Agent C 日志用）与默认构造初态。
void phaseNameAndDefaults() {
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kIdle), "Idle") == 0);
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kPreparing), "Preparing") == 0);
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kDisplaySuspended), "DisplaySuspended") == 0);
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kScanning), "Scanning") == 0);
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kCollecting), "Collecting") == 0);
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kRestoring), "Restoring") == 0);
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kError), "Error") == 0);
    CHECK(std::strcmp(scanPhaseName(ScanPhase::kDisconnected), "Disconnected") == 0);
    CHECK(std::strcmp(scanPhaseName(static_cast<ScanPhase>(999)), "Unknown") == 0);

    ScanTransaction tx;
    CHECK(tx.phase() == ScanPhase::kIdle);
    CHECK(!tx.displaySuspended());
}

}  // namespace

// M7-E：供 shared/protocol host 套件调用（test_main.cpp 声明）；独立目标
// scan_transaction_test 经 SCAN_TRANSACTION_TEST_MAIN 提供 main 直接调用本函数。
void runScanTransactionTests() {
    std::printf("[scan_transaction] success\n");
    successFlow();
    std::printf("[scan_transaction] scan_fail\n");
    scanFailures();
    std::printf("[scan_transaction] timeout\n");
    timeoutFlow();
    std::printf("[scan_transaction] begin_return\n");
    beginReturnsSuspendResult();
    std::printf("[scan_transaction] double_begin\n");
    doubleBeginIdempotent();
    std::printf("[scan_transaction] double_resume\n");
    doubleResumeIdempotent();
    std::printf("[scan_transaction] error_path\n");
    suspendFailure();
    std::printf("[scan_transaction] retry\n");
    retry();
    std::printf("[scan_transaction] disconnect\n");
    disconnectSession();
    std::printf("[scan_transaction] oled_back_active\n");
    oledAlwaysBackActive();
    std::printf("[scan_transaction] timeout_then_retry\n");
    timeoutThenRetry();
    std::printf("[scan_transaction] phase_name\n");
    phaseNameAndDefaults();
}

#if defined(SCAN_TRANSACTION_TEST_MAIN)
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView shared/wifi scan_transaction host tests ==\n");
    runScanTransactionTests();
    std::printf("----\nchecks: %d, failures: %d\n", espview::proto::test::gChecks,
                espview::proto::test::gFailures);
    return espview::proto::test::gFailures == 0 ? 0 : 1;
}
#endif