// ESPView — DisplayManager 实现（M5-A，编译期模式）。

#include "display_manager.h"

namespace espview {
namespace display {

void DisplayManager::addBackend(std::shared_ptr<IDisplay> backend) {
    // v0.1：WINDOW 单后端。后续版本按 DisplayMode 分槽（Window/Device/Mirror）。
    if (!backend) {
        return;
    }
    backends_.clear();
    backends_.push_back(std::move(backend));
}

IDisplay& DisplayManager::active() {
    // 未注册后端时返回一个可用的静态 no-op 后端？——不允许：
    // 调用方（LVGL flush_cb）必须保证已注册后端。这里返回第一个后端；
    // 若为空由调用方崩溃定位（v0.1 断言式契约，与 DESIGN.md D.2 一致）。
    return *backends_.front();
}

const IDisplay& DisplayManager::active() const {
    return *backends_.front();
}

DisplayStatus DisplayManager::setMode(DisplayMode mode) {
    if (mode == DisplayMode::kWindow) {
        mode_ = mode;
        return DisplayStatus::kOk;
    }
    // v0.1 编译期模式：DEVICE / MIRROR 未实现（M5-A 不做 runtime DisplayMode）。
    return DisplayStatus::kNotSupported;
}

}  // namespace display
}  // namespace espview
