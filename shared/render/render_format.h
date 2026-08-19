// ESPView M8-C (C1) — RenderFormat: rendering pipeline pixel formats (pure C++17).
//
// 定位：Physical Display Rendering 的可组合 pipeline 的格式契约（task book M8-C
// §五/§六/§七/§八）。与 wire protocol 无关：协议像素格式（proto::PixelFormat）
// 是 wire 侧事实；这里定义的是「渲染中间/输出」的格式，pipeline 各 stage 用它做
// input/output format 校验与内存规划。
//
// v0.1 格式集合（只实现需要用到的，不做过度抽象）：
//   kRgb565 — 16bpp，行主序，小端 uint16，2 字节/像素（LVGL/源帧）
//   kGray8  — 8bpp 灰度，行主序，1 字节/像素（quality path 中间格式）
//   kMono1  — 1bpp 单色，页式（page-mode）：byte = 8 个垂直像素，bit0 = 页顶行，
//             与 shared/oled OledFb / SSD1306 GDDRAM 布局一致（fast path 直写，
//             零打包成本；quality path 的 dither stage 直接输出页式）。
// 未来 LCD（RGB888）等格式在需要时再扩展；本头不预留未实现枚举。
#pragma once

#include <cstddef>
#include <cstdint>

namespace espview {
namespace render {

enum class RenderFormat : uint8_t {
    kRgb565 = 0,
    kGray8 = 1,
    kMono1 = 2,  // 页式 1bpp（byte=8 垂直像素，bit0=页顶）
};

constexpr bool formatIsMono(RenderFormat f) { return f == RenderFormat::kMono1; }

// 每像素位数（kMono1 名义 1bpp；行步长用 formatRowStrideBytes）。
constexpr uint8_t formatBitsPerPixel(RenderFormat f) {
    switch (f) {
        case RenderFormat::kRgb565: return 16;
        case RenderFormat::kGray8: return 8;
        case RenderFormat::kMono1: return 1;
    }
    return 0;
}

// 行步长（字节）。kRgb565/kGray8 = width * bpp/8；kMono1 页式 = width
// （每行 1 字节/像素列，垂直 8 像素打包）。
constexpr size_t formatRowStrideBytes(RenderFormat f, int width) {
    if (width <= 0) {
        return 0;
    }
    switch (f) {
        case RenderFormat::kRgb565: return static_cast<size_t>(width) * 2u;
        case RenderFormat::kGray8: return static_cast<size_t>(width);
        case RenderFormat::kMono1: return static_cast<size_t>(width);  // 页式列字节
    }
    return 0;
}

// 输出缓冲容量：stride * height（页式 kMono1 需 height 为 8 的倍数，
// 不足时按 ceil 到页对齐计算）。
constexpr size_t formatBufferBytes(RenderFormat f, int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    if (formatIsMono(f)) {
        const int pages = (height + 7) / 8;
        return static_cast<size_t>(width) * static_cast<size_t>(pages);
    }
    return formatRowStrideBytes(f, width) * static_cast<size_t>(height);
}

inline const char* formatName(RenderFormat f) {
    switch (f) {
        case RenderFormat::kRgb565: return "rgb565";
        case RenderFormat::kGray8: return "gray8";
        case RenderFormat::kMono1: return "mono1-page";
    }
    return "unknown";
}

}  // namespace render
}  // namespace espview
