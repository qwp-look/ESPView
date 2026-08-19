// ESPView M8-C (C1) — render_color: 颜色转换单一实现（pure C++17, header-only）。
//
// 收敛点：PhysicalRenderer::luminance 与 LuminanceStage 共用同一套 RGB565→
// 亮度公式（M8-C 审计「重复」项），避免两处公式漂移。锚点（M7-C2 定稿）：
//   白 0xFFFF -> 255、红 0xF800 -> 76、绿 0x07E0 -> 150、蓝 0x001F -> 29。
// R5/G6/B5 先放大到 8bit 满量程（31->255、63->255），
// Y = (299R + 587G + 114B + 500) / 1000（四舍五入）。
#pragma once

#include <cstdint>

namespace espview {
namespace render {

// RGB565（小端 uint16，逐字节组合安全）-> 8bit 亮度 0..255。
inline uint8_t rgb565Luminance(uint16_t rgb565) {
    const uint32_t r = ((rgb565 >> 11) & 0x1Fu) * 255u / 31u;
    const uint32_t g = ((rgb565 >> 5) & 0x3Fu) * 255u / 63u;
    const uint32_t b = (rgb565 & 0x1Fu) * 255u / 31u;
    return static_cast<uint8_t>((299u * r + 587u * g + 114u * b + 500u) / 1000u);
}

// Y >= th -> 1，否则 0。
inline uint8_t rgb565Thresholded(uint16_t rgb565, uint8_t th) {
    return rgb565Luminance(rgb565) >= th ? 1u : 0u;
}

}  // namespace render
}  // namespace espview
