// ESPView — DisplayManager（DESIGN.md D.2）
//
// v0.1（M5-A）实现语义：
//   - 编译期模式：ESPVIEW_DEFAULT_MODE（默认 WINDOW）。addBackend() 注册
//     WINDOW 后端；active() 返回之；Application 与 LVGL 只经 active() 转发，
//     不直接绑定具体后端；
//   - setMode() 接口保留（M6 runtime DisplayMode 预留）：v0.1 只有
//     kWindow 可切换成功，kDevice / kMirror 返回 kNotSupported；
//   - 运行时切模式后需全屏置脏触发一次重绘（未来 M6）。
// 纯 C++17，零平台依赖。

#pragma once

#include <memory>
#include <vector>

#include "display.h"

namespace espview {
namespace display {

class DisplayManager {
public:
    // 注册后端。v0.1：只接受一个 WINDOW 后端（后注册者覆盖前注册者）。
    void addBackend(std::shared_ptr<IDisplay> backend);

    // Application 唯一入口（DESIGN.md D.2：对 Application 透明）。
    IDisplay& active();
    const IDisplay& active() const;
    bool hasActive() const { return !backends_.empty(); }

    // v0.1 编译期模式：kWindow 成功；kDevice/kMirror 返回 kNotSupported。
    DisplayStatus setMode(DisplayMode mode);
    DisplayMode activeMode() const { return mode_; }

private:
    std::vector<std::shared_ptr<IDisplay>> backends_;
    DisplayMode mode_ = DisplayMode::kWindow;
};

}  // namespace display
}  // namespace espview
