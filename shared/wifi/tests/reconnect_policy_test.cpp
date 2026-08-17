// ESPView M8-A5 — ReconnectPolicy Host Tests（有界重连策略）。
//
// 覆盖 M8-A5 任务书 §三十二 Wi-Fi 项 21-26（host 侧策略级）：
//   connect/disconnect/retry/retry cancellation/stop during backoff/repeated start-stop。
// 纯策略单元测试：不依赖 ESP-IDF。

#include <cstdint>
#include <cstdio>

#include "../protocol/tests/test_util.h"
#include "reconnect_policy.h"

namespace {

using espview::wifi::ReconnectPolicy;
using espview::wifi::WifiFailureKind;

void testTransientBackoff() {
    ReconnectPolicy p;
    // 21. 连续 transient 失败：100 → 200 → 400 → 800 → 1600 → 3200 → 5000(cap) → 5000
    const uint32_t expected[] = {100, 200, 400, 800, 1600, 3200, 5000, 5000};
    for (uint32_t e : expected) {
        p.onAttempt(WifiFailureKind::kTransient);
        CHECK_EQ(p.nextDelayMs(), e);
    }
    CHECK(!p.mustStop());
}

void testTerminalBackoff() {
    ReconnectPolicy p;
    // 22. terminal：1s → 2s → 4s → 8s → 16s → 30s(cap) → 30s
    const uint32_t expected[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (uint32_t e : expected) {
        p.onAttempt(WifiFailureKind::kTerminal);
        CHECK_EQ(p.nextDelayMs(), e);
    }
    CHECK_EQ(p.attempts(), 7u);
}

void testSuccessReset() {
    ReconnectPolicy p;
    p.onAttempt(WifiFailureKind::kTerminal);
    p.onAttempt(WifiFailureKind::kTerminal);
    CHECK_EQ(p.nextDelayMs(), 2000u);
    // 连接成功 → 重置（恢复快速重试起点）
    p.onSuccess();
    CHECK_EQ(p.attempts(), 0u);
    CHECK_EQ(p.nextDelayMs(), ReconnectPolicy::kTransientBaseMs);
    p.onAttempt(WifiFailureKind::kTransient);
    CHECK_EQ(p.nextDelayMs(), 100u);
}

void testStop() {
    ReconnectPolicy p;
    // 23/25. stop during backoff：stop 后 mustStop 恒 true，不再安排 retry
    p.onAttempt(WifiFailureKind::kTransient);
    CHECK(!p.mustStop());
    p.stop();
    CHECK(p.mustStop());
    p.onAttempt(WifiFailureKind::kTransient);  // 即使误调也保持 stopped
    CHECK(p.mustStop());
}

void testRepeatedStartStop() {
    // 26. repeated start/stop：策略对象可重建；同一对象 stop 后必须新建或 reset 语义
    for (int i = 0; i < 1000; ++i) {
        ReconnectPolicy p;
        p.onAttempt(WifiFailureKind::kTransient);
        p.stop();
        CHECK(p.mustStop());
    }
    // 24. retry cancellation：stop 后调用方检查 mustStop 不再调度
    ReconnectPolicy q;
    q.onAttempt(WifiFailureKind::kTerminal);
    if (q.mustStop()) {
        CHECK(!"unreachable");
    }
    q.stop();
    CHECK(q.mustStop());
}

}  // namespace

void runReconnectPolicyTests() {
    std::printf("[wifi_reconnect_policy]\n");
    testTransientBackoff();
    testTerminalBackoff();
    testSuccessReset();
    testStop();
    testRepeatedStartStop();
}