// ESPView — DisplayUiState Host Tests（M7-C3）
//
// 规范来源：M7-C 任务书 §二十二 1-10（VirtualOnly / PhysicalOnly / Mirror /
// Split / physical unavailable / physical degraded / disconnected /
// switching / FULL resync pending / apply disabled during switching）。
// 纯 host，零平台依赖；沿用 display_router_test.cpp / test_util.h 风格
// （CHECK / CHECK_EQ + 全局计数器）。
//
// 测试链路：DisplayUiState（UI 模型）→ 断言选择 / 应用 / 会话 / capability /
// 降级 / FULL resync / Apply 可用性的确定性转移。不触碰协议 wire
// （发送由接线方经 proto::makeSetMode 完成，本模型不生成 wire）。

#include <cstdio>
#include <string>

#include "display_router.h"
#include "display_ui_state.h"
#include "test_util.h"

namespace {

using espview::display::DisplayRouteMode;
using espview::display::DisplayUiState;
using espview::display::RouterState;
using espview::display::UiRouterState;
using espview::display::modeRequiresPhysical;
using espview::display::toUiRouterState;

// ---- 辅助：把状态推到「已连接 + 能力可用 + 已收敛」----
void readyState(DisplayUiState& s, DisplayRouteMode mode) {
    s.onPhysicalAvailable(true);
    s.onConnected();
    s.onFullCommit();
    if (mode != DisplayRouteMode::kVirtualOnly) {
        CHECK(s.setSelectedMode(mode));
        CHECK(s.applyRequested());  // 已连接 → 应发送
        s.onAck(true);
        s.onFullCommit();
    }
}

// §二十二-1：VirtualOnly（默认 + 应用 + 激活标志）。
void testVirtualOnly() {
    DisplayUiState s;
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kIdle));
    CHECK(s.virtualActive);
    CHECK(!s.physicalActive);
    CHECK(s.applyEnabled);
    CHECK(!s.sessionConnected);

    readyState(s, DisplayRouteMode::kVirtualOnly);
    CHECK(s.sessionConnected);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
    CHECK(!s.fullResyncPending);
    CHECK(s.virtualActive);
    CHECK(!s.physicalActive);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK(s.lastError.empty());
}

// §二十二-2：PhysicalOnly（capability 开 → 可选 / 应用 / 激活标志）。
void testPhysicalOnly() {
    DisplayUiState s;
    s.onPhysicalAvailable(true);
    CHECK(s.physicalAvailable);
    CHECK(s.setSelectedMode(DisplayRouteMode::kPhysicalOnly));
    CHECK(!s.virtualActive);
    CHECK(s.physicalActive);

    readyState(s, DisplayRouteMode::kPhysicalOnly);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
    CHECK(!s.fullResyncPending);
    CHECK(!s.virtualActive);
    CHECK(s.physicalActive);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kPhysicalOnly));
}

// §二十二-3：Mirror（双激活 + 应用）。
void testMirror() {
    DisplayUiState s;
    s.onPhysicalAvailable(true);
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.virtualActive);
    CHECK(s.physicalActive);

    readyState(s, DisplayRouteMode::kMirror);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
    CHECK(s.virtualActive);
    CHECK(s.physicalActive);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
}

// §二十二-4：Split（双激活 + 应用）。
void testSplit() {
    DisplayUiState s;
    s.onPhysicalAvailable(true);
    CHECK(s.setSelectedMode(DisplayRouteMode::kSplit));
    CHECK(s.virtualActive);
    CHECK(s.physicalActive);

    readyState(s, DisplayRouteMode::kSplit);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
    CHECK(s.virtualActive);
    CHECK(s.physicalActive);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kSplit));
}

// §二十二-5：physical unavailable（capability 门控：物理模式不可选 + Unavailable）。
void testPhysicalUnavailable() {
    DisplayUiState s;
    s.onPhysicalAvailable(false);
    CHECK(!s.physicalAvailable);

    // 物理相关模式一律拒绝，选择保持 VirtualOnly。
    CHECK(!s.setSelectedMode(DisplayRouteMode::kPhysicalOnly));
    CHECK(!s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(!s.setSelectedMode(DisplayRouteMode::kSplit));
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK(!s.lastError.empty());
    CHECK(s.virtualActive);
    CHECK(!s.physicalActive);

    // VirtualOnly 仍可选（成功时清错误）。
    CHECK(s.setSelectedMode(DisplayRouteMode::kVirtualOnly));
    CHECK(s.lastError.empty());

    // 已应用物理模式时 capability 丢失 → Unavailable + 选择回退 VirtualOnly。
    DisplayUiState t;
    t.onPhysicalAvailable(true);
    t.onConnected();
    CHECK(t.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(t.applyRequested());
    t.onAck(true);
    CHECK_EQ(static_cast<int>(t.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
    t.onPhysicalAvailable(false);
    CHECK_EQ(static_cast<int>(t.routerState), static_cast<int>(UiRouterState::kUnavailable));
    CHECK_EQ(static_cast<int>(t.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK(t.virtualActive);
    CHECK(!t.physicalActive);
}

// §二十二-6：physical degraded（运行时物理不可用 → Mirror 降级，Virtual 继续）。
void testPhysicalDegraded() {
    DisplayUiState s;
    readyState(s, DisplayRouteMode::kMirror);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    s.onPhysicalDegraded(true);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kDegraded));
    CHECK(s.virtualActive);   // virtual 继续收帧
    CHECK(s.physicalActive);  // 模式仍是 Mirror（激活标志按模式派生）
    CHECK(!s.fullResyncPending);
    CHECK(s.applyEnabled);

    // 降级解除 → 收敛回 Connected。
    s.onPhysicalDegraded(false);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    // VirtualOnly 模式下物理降级不影响状态。
    DisplayUiState t;
    readyState(t, DisplayRouteMode::kVirtualOnly);
    t.onPhysicalDegraded(true);
    CHECK_EQ(static_cast<int>(t.routerState), static_cast<int>(UiRouterState::kConnected));
}

// §二十二-7：disconnected（允许改选择；Apply = Waiting for connection，不假装成功）。
void testDisconnected() {
    DisplayUiState s;
    s.onPhysicalAvailable(true);
    s.onConnected();
    s.onFullCommit();
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));

    s.onDisconnected();
    CHECK(!s.sessionConnected);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kUnavailable));
    CHECK(s.applyEnabled);
    CHECK(!s.switchingInProgress);

    // 断开时仍允许改变选择。
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kMirror));

    // Apply → 标记 Waiting for connection，不进入切换、不改 appliedMode。
    CHECK(!s.applyRequested());
    CHECK(s.waitingForConnection);
    CHECK(!s.switchingInProgress);
    CHECK(s.applyEnabled);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kUnavailable));

    // 重连 → 恢复，等待标记清除，需要 FULL resync。
    s.onConnected();
    CHECK(s.sessionConnected);
    CHECK(!s.waitingForConnection);
    CHECK(s.fullResyncPending);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
}

// §二十二-8：switching（进入切换：状态/标志；ACK 收敛到实际发送的模式）。
void testSwitching() {
    DisplayUiState s;
    readyState(s, DisplayRouteMode::kVirtualOnly);
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));

    CHECK(s.applyRequested());
    CHECK(s.switchingInProgress);
    CHECK(!s.applyEnabled);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kSwitching));
    CHECK(!s.fullResyncPending);

    // 切换中改变选择允许（不破坏本次实际发送的模式）。
    CHECK(s.setSelectedMode(DisplayRouteMode::kSplit));

    s.onAck(true);
    CHECK(!s.switchingInProgress);
    CHECK(s.applyEnabled);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kSplit));
    CHECK(s.fullResyncPending);  // ACK ok → 等 FULL resync
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    // ACK fail：回退选择到 appliedMode，保留错误。
    DisplayUiState t;
    readyState(t, DisplayRouteMode::kVirtualOnly);
    CHECK(t.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(t.applyRequested());
    t.onAck(false);
    CHECK(!t.switchingInProgress);
    CHECK(t.applyEnabled);
    CHECK_EQ(static_cast<int>(t.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(t.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK(!t.fullResyncPending);
    CHECK(!t.lastError.empty());
}

// §二十二-9：FULL resync pending（切换/重连后挂起，FULL 提交后清除）。
void testFullResyncPending() {
    DisplayUiState s;
    s.onPhysicalAvailable(true);
    s.onConnected();
    CHECK(s.fullResyncPending);  // 新会话 → FULL resync pending
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    s.onFullCommit();
    CHECK(!s.fullResyncPending);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    // 切换成功 → 再次 pending。
    CHECK(s.setSelectedMode(DisplayRouteMode::kPhysicalOnly));
    CHECK(s.applyRequested());
    s.onAck(true);
    CHECK(s.fullResyncPending);
    s.onFullCommit();
    CHECK(!s.fullResyncPending);

    // 断开 → 清除 pending；重连 → 重新挂起。
    s.onDisconnected();
    CHECK(!s.fullResyncPending);
    s.onConnected();
    CHECK(s.fullResyncPending);
}

// §二十二-10：apply disabled during switching（防重复）。
void testApplyDisabledDuringSwitching() {
    DisplayUiState s;
    readyState(s, DisplayRouteMode::kVirtualOnly);
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.applyRequested());
    CHECK(s.switchingInProgress);
    CHECK(!s.applyEnabled);

    // 切换中再次 Apply 被拒绝，不重复发送。
    CHECK(!s.applyRequested());
    CHECK(!s.lastError.empty());
    CHECK(s.switchingInProgress);
    CHECK(!s.applyEnabled);

    s.onAck(true);
    CHECK(s.applyEnabled);
    CHECK(!s.switchingInProgress);
    CHECK(s.applyRequested());  // 恢复后可再次 Apply
}

// §16-5：selected != applied（选择先于应用 / 切换中改选 / ACK fail 回退）。
void testSelectedVsApplied() {
    // 场景 A：应用后未 Apply 的新选择 —— selected != applied，路由状态按 applied 收敛。
    DisplayUiState s;
    readyState(s, DisplayRouteMode::kMirror);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK(s.setSelectedMode(DisplayRouteMode::kSplit));
    CHECK_MSG(static_cast<int>(s.selectedMode) != static_cast<int>(s.appliedMode),
              "M7-C4: selected must differ from applied after un-applied selection");
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kSplit));
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK(!s.switchingInProgress);
    CHECK(!s.fullResyncPending);  // 未发送 SET_MODE，不挂起 FULL resync
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
    CHECK(s.virtualActive);   // 激活标志按 selectedMode 派生
    CHECK(s.physicalActive);

    // 场景 B：切换中改选 —— ACK 应用到本次实际发送的模式，selected 保持新值。
    DisplayUiState t;
    readyState(t, DisplayRouteMode::kVirtualOnly);
    CHECK(t.setSelectedMode(DisplayRouteMode::kPhysicalOnly));
    CHECK(t.applyRequested());
    CHECK(t.setSelectedMode(DisplayRouteMode::kMirror));  // 切换中改选
    t.onAck(true);
    CHECK_EQ(static_cast<int>(t.appliedMode), static_cast<int>(DisplayRouteMode::kPhysicalOnly));
    CHECK_MSG(static_cast<int>(t.selectedMode) != static_cast<int>(t.appliedMode),
              "M7-C4: selected/applied must diverge after mid-switch selection change");
    CHECK_EQ(static_cast<int>(t.selectedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK(!t.switchingInProgress);

    // 场景 C：ACK fail 回退 —— selected 收敛回 applied，二者相等。
    DisplayUiState u;
    readyState(u, DisplayRouteMode::kVirtualOnly);
    CHECK(u.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(u.applyRequested());
    u.onAck(false);
    CHECK_MSG(static_cast<int>(u.selectedMode) == static_cast<int>(u.appliedMode),
              "M7-C4: failed ACK must revert selection to applied mode");
    CHECK_EQ(static_cast<int>(u.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK(!u.fullResyncPending);
}

// §16-11：successful apply（ACK ok → appliedMode 更新 / 清错 / FULL resync pending）。
void testSuccessfulApply() {
    DisplayUiState s;
    s.onPhysicalAvailable(true);
    s.onConnected();
    s.onFullCommit();
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.applyRequested());  // 已连接 → 应发送
    CHECK(s.switchingInProgress);
    CHECK(!s.applyEnabled);
    CHECK(s.lastError.empty());
    s.onAck(true);
    CHECK(!s.switchingInProgress);
    CHECK(s.applyEnabled);
    CHECK_MSG(static_cast<int>(s.appliedMode) == static_cast<int>(DisplayRouteMode::kMirror),
              "M7-C4: successful ACK must commit applied mode");
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK(s.fullResyncPending);  // ACK ok → 等新 FULL resync 帧
    CHECK(s.lastError.empty());
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
    s.onFullCommit();
    CHECK(!s.fullResyncPending);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    // 成功路径可重复：再次 Apply 到另一模式。
    CHECK(s.setSelectedMode(DisplayRouteMode::kSplit));
    CHECK(s.applyRequested());
    s.onAck(true);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kSplit));
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
}

// §16-12：failed apply（ACK ERR → 选择回退 / 保留错误 / 可重试）。
void testFailedApply() {
    DisplayUiState s;
    readyState(s, DisplayRouteMode::kVirtualOnly);
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.applyRequested());
    CHECK(s.switchingInProgress);
    s.onAck(false);
    CHECK(!s.switchingInProgress);
    CHECK(s.applyEnabled);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_MSG(!s.fullResyncPending, "M7-C4: failed ACK must not arm FULL resync");
    CHECK_MSG(!s.lastError.empty(), "M7-C4: failed ACK must retain an error message");
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    // 错误可清除；清除后重试进入切换。
    s.clearError();
    CHECK(s.lastError.empty());
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.applyRequested());
    CHECK(s.switchingInProgress);
    s.onAck(true);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
}

// §16-13：watchdog timeout（模型无显式超时 API → 以「切换挂起超时 → 断开清理 +
// 迟到 ACK 忽略」代理验证：超时后不得改写 appliedMode、不得重新挂起 FULL resync）。
void testWatchdogTimeout() {
    DisplayUiState s;
    readyState(s, DisplayRouteMode::kVirtualOnly);
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.applyRequested());
    CHECK(s.switchingInProgress);
    CHECK(!s.applyEnabled);
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kSwitching));

    // 超时（无 ACK）：会话断开清理切换状态。
    s.onDisconnected();
    CHECK_MSG(!s.switchingInProgress, "M7-C4: timeout must clear switching state");
    CHECK_MSG(s.applyEnabled, "M7-C4: timeout must re-enable Apply");
    CHECK(!s.fullResyncPending);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kUnavailable));

    // 迟到的 ACK（超时后才到）被忽略：不改 appliedMode、不挂起 FULL resync。
    s.onAck(true);
    CHECK(!s.switchingInProgress);
    CHECK_MSG(!s.fullResyncPending, "M7-C4: stale ACK after timeout must be ignored");
    CHECK_MSG(static_cast<int>(s.appliedMode) == static_cast<int>(DisplayRouteMode::kVirtualOnly),
              "M7-C4: stale ACK after timeout must not commit applied mode");
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kUnavailable));

    // 重连后超时链路可重试（不会卡在禁用）。
    s.onConnected();
    CHECK(s.applyEnabled);
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.applyRequested());
    s.onAck(true);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));
}

// §16-14：capability —— physical available（物理模式可选 / 可应用）。
void testCapabilityGateAvailable() {
    DisplayUiState s;
    CHECK(!s.physicalAvailable);  // 默认无 capability
    s.onPhysicalAvailable(true);
    CHECK_MSG(s.physicalAvailable, "M7-C4: onPhysicalAvailable(true) must set flag");
    CHECK(s.lastError.empty());
    CHECK(s.setSelectedMode(DisplayRouteMode::kPhysicalOnly));
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.setSelectedMode(DisplayRouteMode::kSplit));
    CHECK(s.setSelectedMode(DisplayRouteMode::kVirtualOnly));
    CHECK(s.lastError.empty());

    // 已连接后物理模式可应用。
    s.onConnected();
    CHECK(s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(s.applyRequested());
    s.onAck(true);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK_EQ(static_cast<int>(s.routerState), static_cast<int>(UiRouterState::kConnected));

    // 能力从无到有：此前被拒绝的选择恢复可选。
    DisplayUiState t;
    CHECK(!t.setSelectedMode(DisplayRouteMode::kPhysicalOnly));  // 无能力时拒绝
    t.onPhysicalAvailable(true);
    CHECK_MSG(t.setSelectedMode(DisplayRouteMode::kPhysicalOnly),
              "M7-C4: physical mode must be selectable once capability is available");
    CHECK(t.lastError.empty());
}

// §16-15：capability —— physical unavailable（拒绝物理模式；断开优先于 capability）。
void testCapabilityGateUnavailable() {
    DisplayUiState s;
    s.onPhysicalAvailable(false);
    CHECK(!s.physicalAvailable);
    CHECK(!s.setSelectedMode(DisplayRouteMode::kPhysicalOnly));
    CHECK(!s.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(!s.setSelectedMode(DisplayRouteMode::kSplit));
    CHECK_EQ(static_cast<int>(s.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK(!s.lastError.empty());
    CHECK(s.virtualActive);
    CHECK(!s.physicalActive);

    // 断开 + 无能力：Apply 报 waiting for connection（断开优先），不进入切换。
    CHECK(!s.applyRequested());
    CHECK_MSG(s.waitingForConnection, "M7-C4: disconnected Apply must wait for connection");
    CHECK(!s.switchingInProgress);
    CHECK_EQ(static_cast<int>(s.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));

    // 已连接 + 无能力 + VirtualOnly：Apply 不需要物理能力 → 成功进入切换。
    DisplayUiState t;
    t.onPhysicalAvailable(false);
    t.onConnected();
    CHECK(t.applyRequested());
    CHECK(t.switchingInProgress);
    t.onAck(true);
    CHECK_EQ(static_cast<int>(t.appliedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(t.routerState), static_cast<int>(UiRouterState::kConnected));

    // 能力丢失 → 选择回退 VirtualOnly；已应用物理模式显示 Unavailable。
    DisplayUiState u;
    u.onPhysicalAvailable(true);
    u.onConnected();
    CHECK(u.setSelectedMode(DisplayRouteMode::kMirror));
    CHECK(u.applyRequested());
    u.onAck(true);
    CHECK_EQ(static_cast<int>(u.routerState), static_cast<int>(UiRouterState::kConnected));
    u.onPhysicalAvailable(false);
    CHECK_EQ(static_cast<int>(u.selectedMode), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(u.appliedMode), static_cast<int>(DisplayRouteMode::kMirror));
    CHECK_MSG(static_cast<int>(u.routerState) == static_cast<int>(UiRouterState::kUnavailable),
              "M7-C4: applied physical mode must show Unavailable when capability lost");
}

// 枚举对齐 / 辅助函数（DisplayRouteMode ↔ RouterState ↔ UiRouterState）。
void testEnumAlignment() {
    CHECK_EQ(static_cast<int>(toUiRouterState(RouterState::kIdle)),
             static_cast<int>(UiRouterState::kIdle));
    CHECK_EQ(static_cast<int>(toUiRouterState(RouterState::kSwitching)),
             static_cast<int>(UiRouterState::kSwitching));
    CHECK_EQ(static_cast<int>(toUiRouterState(RouterState::kConnected)),
             static_cast<int>(UiRouterState::kConnected));
    CHECK_EQ(static_cast<int>(toUiRouterState(RouterState::kDegraded)),
             static_cast<int>(UiRouterState::kDegraded));
    CHECK(!modeRequiresPhysical(DisplayRouteMode::kVirtualOnly));
    CHECK(modeRequiresPhysical(DisplayRouteMode::kPhysicalOnly));
    CHECK(modeRequiresPhysical(DisplayRouteMode::kMirror));
    CHECK(modeRequiresPhysical(DisplayRouteMode::kSplit));
    // 非法模式选择被拒绝。
    DisplayUiState s;
    CHECK(!s.setSelectedMode(static_cast<DisplayRouteMode>(7)));
    CHECK(!s.lastError.empty());
}

}  // namespace

void runDisplayUiStateTests() {
    std::printf("[display_ui_state] tests\n");
    testEnumAlignment();
    testVirtualOnly();       // §十六-1
    testPhysicalOnly();      // §二十二-2
    testMirror();            // §二十二-3
    testSplit();             // §二十二-4
    testPhysicalUnavailable();  // §二十二-5
    testPhysicalDegraded();  // §二十二-6
    testDisconnected();      // §二十二-7
    testSwitching();         // §二十二-8
    testFullResyncPending(); // §二十二-9
    testApplyDisabledDuringSwitching();  // §二十二-10
    testSelectedVsApplied();      // §16-5
    testSuccessfulApply();        // §16-11
    testFailedApply();            // §16-12
    testWatchdogTimeout();        // §16-13
    testCapabilityGateAvailable();    // §16-14
    testCapabilityGateUnavailable();  // §16-15
    std::printf("[display_ui_state] done\n");
}
