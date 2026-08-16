// ESPView M7-E — ScanTransaction 实现（纯 C++17，零平台依赖）。
// 见 scan_transaction.h 的相位转移说明。

#include "scan_transaction.h"

#include <utility>

namespace espview {
namespace wifi {

const char* scanPhaseName(ScanPhase phase) {
    switch (phase) {
        case ScanPhase::kIdle:
            return "Idle";
        case ScanPhase::kPreparing:
            return "Preparing";
        case ScanPhase::kDisplaySuspended:
            return "DisplaySuspended";
        case ScanPhase::kScanning:
            return "Scanning";
        case ScanPhase::kCollecting:
            return "Collecting";
        case ScanPhase::kRestoring:
            return "Restoring";
        case ScanPhase::kError:
            return "Error";
        case ScanPhase::kDisconnected:
            return "Disconnected";
    }
    return "Unknown";
}

ScanTransaction::ScanTransaction(ScanTransactionCallbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

void ScanTransaction::setCallbacks(ScanTransactionCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

bool ScanTransaction::begin() {
    // 幂等：仅从终态（kIdle/kError/kDisconnected）启动；活动相位重复 begin 为 no-op。
    switch (phase_) {
        case ScanPhase::kIdle:
        case ScanPhase::kError:
        case ScanPhase::kDisconnected:
            break;
        default:
            return false;
    }
    phase_ = ScanPhase::kPreparing;
    suspended_ = false;
    resetWindowClock();
    // 同步调用 suspendDisplay：成功 -> 已挂起（返回 true）；失败 -> kError，
    // 本会话从未挂起（不恢复），返回 false（驱动不得无保护启动扫描）。
    if (callbacks_.suspendDisplay && callbacks_.suspendDisplay()) {
        suspended_ = true;
        return true;
    }
    phase_ = ScanPhase::kError;
    return false;
}

void ScanTransaction::onScanStarted(bool suspendedOk) {
    if (phase_ != ScanPhase::kPreparing) {
        return;  // 幂等/防御：非 Preparing 忽略（double onScanStarted 安全）。
    }
    if (!suspendedOk) {
        // begin 内 suspend 已成功，仍须恢复显示恰一次（核心不变量）。
        phase_ = ScanPhase::kError;
        restoreDisplayOnce();
        resetWindowClock();
        return;
    }
    phase_ = ScanPhase::kDisplaySuspended;
    resetWindowClock();  // 扫描窗口起点由首次 tick 锚定
}

void ScanTransaction::onScanDone(bool ok) {
    if (phase_ != ScanPhase::kDisplaySuspended && phase_ != ScanPhase::kScanning) {
        return;  // 幂等/防御：非扫描窗口忽略（stale onScanDone 安全）。
    }
    phase_ = ScanPhase::kCollecting;   // 结果收集中（瞬时）
    phase_ = ScanPhase::kRestoring;    // 恢复显示中（瞬时；resumeDisplay 恰一次）
    restoreDisplayOnce();
    phase_ = ok ? ScanPhase::kIdle : ScanPhase::kError;
    resetWindowClock();
}

void ScanTransaction::onTimeout() {
    switch (phase_) {
        case ScanPhase::kPreparing:
            // 契约：kPreparing 直接 kError（不经中间相位）。begin 内 suspend 已成功，
            // 按核心不变量仍恢复显示恰一次。
        case ScanPhase::kDisplaySuspended:
        case ScanPhase::kScanning:
        case ScanPhase::kCollecting:
            phase_ = ScanPhase::kError;
            restoreDisplayOnce();
            resetWindowClock();
            return;
        default:
            return;  // 空闲/终态：no-op（double resume 幂等）。
    }
}

void ScanTransaction::onDisconnect() {
    // 任何相位 -> kDisconnected；若当前挂起则恢复恰一次（不变量）。
    phase_ = ScanPhase::kDisconnected;
    restoreDisplayOnce();
    resetWindowClock();
}

void ScanTransaction::tick(uint64_t nowMs, uint64_t timeoutMs) {
    const bool inWindow = phase_ == ScanPhase::kDisplaySuspended ||
                          phase_ == ScanPhase::kScanning ||
                          phase_ == ScanPhase::kCollecting;
    if (!inWindow) {
        return;  // 空闲/Preparing/终态不做超时（Preparing 超时由驱动直接调 onTimeout）。
    }
    if (timeoutMs == 0) {
        return;  // 0 = 不启用超时
    }
    if (scanStartMs_ == 0) {
        scanStartMs_ = nowMs;  // 首次 tick 锚定扫描窗口起点
        return;
    }
    if (nowMs < scanStartMs_) {
        return;  // 时钟回拨防御
    }
    if (nowMs - scanStartMs_ >= timeoutMs) {
        onTimeout();
    }
}

ScanPhase ScanTransaction::phase() const {
    return phase_;
}

bool ScanTransaction::displaySuspended() const {
    return suspended_;
}

void ScanTransaction::restoreDisplayOnce() {
    if (!suspended_) {
        return;
    }
    // 先执行回调再清标志：回调执行期间 displaySuspended() 仍为 true（恢复尚未完成）。
    if (callbacks_.resumeDisplay) {
        callbacks_.resumeDisplay();
    }
    suspended_ = false;
}

void ScanTransaction::resetWindowClock() {
    scanStartMs_ = 0;
}

}  // namespace wifi
}  // namespace espview