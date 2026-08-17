// ESPView — DisplayRouter 实现（M7-C1）。语义见 display_router.h 文件头。
#include "display_router.h"

#include <utility>

namespace espview {
namespace display {

void DisplayRouter::attachVirtual(std::shared_ptr<IDisplaySink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    virtual_ = std::move(sink);
}

void DisplayRouter::attachPhysical(std::shared_ptr<IDisplaySink> sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    physical_ = std::move(sink);
}

void DisplayRouter::setStaleClearCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    staleClearCb_ = std::move(cb);
}

void DisplayRouter::setFullResyncCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    fullResyncCb_ = std::move(cb);
}

DisplayRouteMode DisplayRouter::mode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mode_;
}

RouterState DisplayRouter::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::shared_ptr<IDisplaySink> DisplayRouter::virtualSink() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return virtual_;
}

std::shared_ptr<IDisplaySink> DisplayRouter::physicalSink() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return physical_;
}

DisplayStatus DisplayRouter::setMode(DisplayRouteMode mode) {
    const uint8_t m = static_cast<uint8_t>(mode);
    if (m > static_cast<uint8_t>(DisplayRouteMode::kSplit)) {
        return DisplayStatus::kInvalidParam;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    // 必需 sink 缺失 = 配置错误：不进入切换，状态保持不变。
    if (mode != DisplayRouteMode::kPhysicalOnly && !virtual_) {
        return DisplayStatus::kInvalidParam;
    }
    if (mode != DisplayRouteMode::kVirtualOnly && !physical_) {
        return DisplayStatus::kInvalidParam;
    }

    state_ = RouterState::kSwitching;
    disableAllLocked();              // 1) 全部 disable（幂等）
    if (staleClearCb_) {             // 2) stale clear 钩子
        staleClearCb_();
    }
    // 3) 按模式 enable
    const bool virtualOn = mode != DisplayRouteMode::kPhysicalOnly;
    const bool physicalOn = mode != DisplayRouteMode::kVirtualOnly;
    if (virtualOn && virtual_) {
        (void)virtual_->setEnabled(true);
    }
    if (physicalOn && physical_) {
        (void)physical_->setEnabled(true);
    }
    mode_ = mode;
    if (fullResyncCb_) {             // 4) 请求 FULL resync
        fullResyncCb_();
    }
    reconcileStateLocked();          // 5) CONNECTED / DEGRADED
    return DisplayStatus::kOk;
}

DisplayStatus DisplayRouter::writeRect(const Rect& rect, const uint8_t* pixels) {
    return writeRectDetailed(rect, pixels).overall;
}

DisplayRouter::RouteWriteResult DisplayRouter::writeRectDetailed(
    const Rect& rect, const uint8_t* pixels) {
    RouteWriteResult out;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RouterState::kConnected && state_ != RouterState::kDegraded) {
        out.overall = DisplayStatus::kNotEnabled;  // kIdle / kSwitching：输出未就绪
        return out;
    }
    // Split：应用帧只走 virtual（物理侧经 presentScene 收独立场景帧）。
    const bool virtualGetsApp = mode_ != DisplayRouteMode::kPhysicalOnly;
    const bool physicalGetsApp = mode_ == DisplayRouteMode::kPhysicalOnly ||
                                 mode_ == DisplayRouteMode::kMirror;

    DisplayStatus firstError = DisplayStatus::kNotConnected;
    bool anyAccepted = false;
    if (virtualGetsApp && virtual_ && virtual_->isAvailable()) {
        const DisplayStatus s = virtual_->present(rect, pixels);
        out.virtualInPath = true;
        out.virtualStatus = s;
        if (s == DisplayStatus::kOk) {
            anyAccepted = true;
        } else if (firstError == DisplayStatus::kNotConnected) {
            firstError = s;
        }
    }
    if (physicalGetsApp && physical_ && physical_->isAvailable()) {
        const DisplayStatus s = physical_->present(rect, pixels);
        out.physicalStatus = s;
        if (s == DisplayStatus::kOk) {
            anyAccepted = true;
        } else if (firstError == DisplayStatus::kNotConnected) {
            firstError = s;
        }
    }
    reconcileStateLocked();
    out.overall = anyAccepted ? DisplayStatus::kOk : firstError;
    return out;
}

DisplayStatus DisplayRouter::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != RouterState::kConnected && state_ != RouterState::kDegraded) {
        return DisplayStatus::kNotEnabled;  // kIdle / kSwitching
    }
    // 与 writeRect 相同的目标集（Split 只 virtual；物理场景帧的 flush 由
    // 场景提交方在 C2 负责 —— 本阶段只做状态语义）。
    const bool virtualGetsApp = mode_ != DisplayRouteMode::kPhysicalOnly;
    const bool physicalGetsApp = mode_ == DisplayRouteMode::kPhysicalOnly ||
                                 mode_ == DisplayRouteMode::kMirror;

    DisplayStatus firstError = DisplayStatus::kNotConnected;
    bool anyAccepted = false;
    if (virtualGetsApp && virtual_ && virtual_->isAvailable()) {
        const DisplayStatus s = virtual_->flush();
        if (s == DisplayStatus::kOk) {
            anyAccepted = true;
        } else if (firstError == DisplayStatus::kNotConnected) {
            firstError = s;
        }
    }
    if (physicalGetsApp && physical_ && physical_->isAvailable()) {
        const DisplayStatus s = physical_->flush();
        if (s == DisplayStatus::kOk) {
            anyAccepted = true;
        } else if (firstError == DisplayStatus::kNotConnected) {
            firstError = s;
        }
    }
    reconcileStateLocked();
    return anyAccepted ? DisplayStatus::kOk : firstError;
}

DisplayStatus DisplayRouter::presentScene(PhysicalScene scene, const Rect& rect,
                                          const uint8_t* pixels) {
    if (scene != PhysicalScene::kDiagnostics && scene != PhysicalScene::kApplication) {
        return DisplayStatus::kInvalidParam;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ != DisplayRouteMode::kSplit) {
        return DisplayStatus::kNotSupported;  // 场景概念仅属 Split（C2 扩展点）
    }
    if (state_ != RouterState::kConnected && state_ != RouterState::kDegraded) {
        return DisplayStatus::kNotEnabled;
    }
    if (!physical_ || !physical_->isAvailable()) {
        reconcileStateLocked();
        return DisplayStatus::kNotConnected;
    }
    const DisplayStatus s = physical_->present(rect, pixels);
    reconcileStateLocked();
    return s;
}

void DisplayRouter::refreshState() {
    std::lock_guard<std::mutex> lock(mutex_);
    reconcileStateLocked();
}

void DisplayRouter::disableAllLocked() {
    if (virtual_) {
        (void)virtual_->setEnabled(false);
    }
    if (physical_) {
        (void)physical_->setEnabled(false);
    }
}

void DisplayRouter::reconcileStateLocked() {
    const bool virtualOn = mode_ != DisplayRouteMode::kPhysicalOnly;
    const bool physicalOn = mode_ != DisplayRouteMode::kVirtualOnly;
    const bool virtualOk = !virtualOn || (virtual_ && virtual_->isAvailable());
    const bool physicalOk = !physicalOn || (physical_ && physical_->isAvailable());
    state_ = (virtualOk && physicalOk) ? RouterState::kConnected : RouterState::kDegraded;
}

}  // namespace display
}  // namespace espview