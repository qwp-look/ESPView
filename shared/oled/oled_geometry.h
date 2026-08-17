// ESPView M8-A4 — OledGeometry：OLED 物理几何唯一事实来源（纯 C++17）。
//
// 规范来源：docs/DESIGN.md §十九（OLED Geometry）—— 128/64/8/1024 不得散落
// 在 oled_fb / oled_preview / capability snapshot 等多处；本头只定义一次，
// 消费方一律引用 kDefaultOledGeometry。SSD1306/SH1106 均为 128x64 页式
// 1bpp（132 列 GDDRAM 差异由上传层处理，见 oled_cmd.cpp，不属几何事实）。
// 注意：PHYSICAL_PREVIEW wire 定长常量（1032B = 8B 头 + 1024B 像素）属
// shared/protocol（冻结），本头不重复定义；kSizeBytes 与 wire 的
// kPhysicalPreviewPixelBytes 数值一致是几何事实，非 wire 布局。
#pragma once

#include <cstddef>
#include <cstdint>

namespace espview {
namespace oled {

// OLED 页式 framebuffer 几何（128x64 1bpp：8 pages × 128B = 1KB）。
struct OledGeometry {
    int width = 0;        // 列数（水平像素）
    int height = 0;       // 行数（垂直像素）
    int pageCount = 0;    // 页数（height / 8）
    size_t sizeBytes = 0; // pageCount * width（字节数）
};

// v0.1 唯一 OLED 几何。SSD1306 / SH1106 逻辑屏均为 128x64。
inline constexpr OledGeometry kDefaultOledGeometry{128, 64, 8, 1024};

}  // namespace oled
}  // namespace espview
