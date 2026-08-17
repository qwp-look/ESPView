// ESPView M8-A4 — DisplayGeometry：虚拟显示几何唯一事实来源（纯 C++17）。
//
// 规范来源：docs/DESIGN.md §十一（Display dimensions）—— 320x240 不得散落
// 在多个文件；逻辑虚拟屏几何只在本头定义一次，display / esp32 / pc 消费方
// 一律引用本常量。OLED 物理几何（128x64）属于 shared/oled OledGeometry，
// 不得塞进本结构（§十一「不要把 OLED 128x64 塞进 320x240 generic DisplayInfo」）。
#pragma once

#include <cstdint>

namespace espview {
namespace display {

// 逻辑虚拟屏几何（v0.1 唯一值：320x240）。
struct DisplayGeometry {
    int width = 0;
    int height = 0;
};

// v0.1 唯一虚拟显示几何。协议只约束 width/height 1..4096（DESIGN.md E 节）；
// 本常量是 ESP32 LVGL / PC VirtualScreen / 输入坐标系 / RemoteDisplay 默认值
// 的单一事实来源。
inline constexpr DisplayGeometry kVirtualDisplayGeometry{320, 240};

}  // namespace display
}  // namespace espview
