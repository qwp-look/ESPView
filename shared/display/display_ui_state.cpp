// ESPView M7-C3 — DisplayUiState 实现（语义见 display_ui_state.h 文件头）。
// 纯 C++17，零 Qt / 零平台依赖；不触碰协议 wire（发送由接线方完成）。

#include "display_ui_state.h"

namespace espview {
namespace display {

bool DisplayUiState::setSelectedMode(DisplayRouteMode mode) {
    if (mode > DisplayRouteMode::kSplit) {
        lastError = "invalid display mode";
        return false;
    }
    if (!physicalAvailable && modeRequiresPhysical(mode)) {
        lastError = "physical display unavailable";
        return false;
    }
    selectedMode = mode;
    lastError.clear();
    refreshActive();
    return true;
}

bool DisplayUiState::applyRequested() {
    if (!applyEnabled) {
        lastError = "apply disabled (switching in progress)";
        return false;
    }
    if (!sessionConnected) {
        // 断开：允许选择，但绝不假装成功 —— 标记 Waiting for connection。
        waitingForConnection = true;
        lastError = "waiting for connection";
        return false;
    }
    if (!physicalAvailable && modeRequiresPhysical(selectedMode)) {
        lastError = "physical display unavailable";
        return false;
    }
    onSwitchStart();
    return true;
}

void DisplayUiState::onSwitchStart() {
    pendingApplyMode_ = selectedMode;  // 锁定本次实际发送的模式
    pendingInterruptedApply = false;
    switchingInProgress = true;
    applyEnabled = false;
    routerState = UiRouterState::kSwitching;
    fullResyncPending = false;
    waitingForConnection = false;
    lastError.clear();
    refreshActive();
}

void DisplayUiState::onAck(bool ok) {
    if (!switchingInProgress) {
        return;  // stale ACK（已超时/断线清理）：忽略
    }
    pendingInterruptedApply = false;
    switchingInProgress = false;
    applyEnabled = true;
    fullResyncPending = ok;  // ACK ok 后设备已应用 → 等 FULL resync 帧
    if (ok) {
        appliedMode = pendingApplyMode_;
        lastError.clear();
    } else {
        // 设备拒绝：回退选择到已应用模式，保留错误。
        selectedMode = appliedMode;
        lastError = "SET_MODE failed (ACK ERR)";
        refreshActive();
    }
    routerState = convergedState();
}

void DisplayUiState::onConnected() {
    pendingInterruptedApply = false;
    sessionConnected = true;
    waitingForConnection = false;
    fullResyncPending = true;   // 新会话必须 FULL resync
    switchingInProgress = false;
    applyEnabled = true;
    lastError.clear();
    routerState = convergedState();
}

void DisplayUiState::onDisconnected() {
    // P1-1：在飞 Apply（SET_MODE 已发、ACK 未回）被断线打断 → 记录
    // interrupted-apply，重连后由接线方自动补发（不能静默丢失切换意图）。
    pendingInterruptedApply = switchingInProgress;
    sessionConnected = false;
    switchingInProgress = false;
    applyEnabled = true;        // 断开时允许点击 Apply → waitingForConnection
    fullResyncPending = false;
    routerState = UiRouterState::kUnavailable;
    refreshActive();
}

void DisplayUiState::onFullCommit() {
    sessionConnected = true;
    fullResyncPending = false;
    switchingInProgress = false;
    applyEnabled = true;
    lastError.clear();
    routerState = convergedState();
    refreshActive();
}

void DisplayUiState::onPhysicalAvailable(bool available) {
    physicalAvailable = available;
    if (!available) {
        // capability 缺失：物理相关模式不可选 → 回退 VirtualOnly；已应用的
        // 物理模式显示 Unavailable。
        if (modeRequiresPhysical(selectedMode)) {
            selectedMode = DisplayRouteMode::kVirtualOnly;
            lastError = "physical display unavailable";
        }
        if (modeRequiresPhysical(appliedMode)) {
            routerState = UiRouterState::kUnavailable;
        } else {
            routerState = convergedState();
        }
        refreshActive();
        return;
    }
    lastError.clear();
    routerState = convergedState();
}

void DisplayUiState::onPhysicalDegraded(bool degraded) {
    physicalDegraded_ = degraded;
    if (!sessionConnected) {
        return;
    }
    routerState = convergedState();
}

UiRouterState DisplayUiState::convergedState() const {
    if (!sessionConnected) {
        return UiRouterState::kUnavailable;
    }
    if (!physicalAvailable && modeRequiresPhysical(appliedMode)) {
        return UiRouterState::kUnavailable;
    }
    if (physicalDegraded_ && modeRequiresPhysical(appliedMode)) {
        return UiRouterState::kDegraded;
    }
    return UiRouterState::kConnected;
}

void DisplayUiState::refreshActive() {
    switch (selectedMode) {
        case DisplayRouteMode::kVirtualOnly:
            virtualActive = true;
            physicalActive = false;
            break;
        case DisplayRouteMode::kPhysicalOnly:
            virtualActive = false;
            physicalActive = true;
            break;
        case DisplayRouteMode::kMirror:
        case DisplayRouteMode::kSplit:
            virtualActive = true;
            physicalActive = true;
            break;
        default:
            virtualActive = false;
            physicalActive = false;
            break;
    }
}

}  // namespace display
}  // namespace espview
