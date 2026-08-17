// ESPView M7-C2 — PhysicalRenderer 实现（RGB565 -> Mono1 页式 OledFb）。
//
// 映射（DESIGN.md C2 定稿）：scale = 2/5 最近邻降采样 + 垂直 center crop。
//   ox = floor(sx * 0.4) = (sx * 2) / 5        （整数除法，sx >= 0 时恒等于 floor）
//   oy = floor(sy * 0.4) - cropY               （cropY 按调用期 srcH 计算）
// 可见源区域 x∈[0,319]、y∈[40,199]（srcH=240 时）。矩形增量：只更新 srcRect
// 映射到的 OLED 像素；同一 OLED 像素被多个源像素映射时，行优先遍历中最后
// 写入者生效（组内各源像素对应同一 5x5 -> 2x2 区域，结果确定）。
//
// M8-A4 紧凑缓冲契约：rgb565 = srcRect.w x srcRect.h 行主序（行步长 rect.w），
// 索引 rect 相对：px = ((sy - srcRect.y) * srcRect.w + (sx - srcRect.x)) * 2。
// 之前按整帧绝对坐标索引（sy*srcW + sx）在 LVGL band 缓冲（320x24）下越界读
// 可达 ~138KB（Agent F/P Blocker）。
#include "physical_renderer.h"

#include <cstdint>

namespace espview {
namespace oled {

namespace {
}  // namespace

PhysicalRenderer::PhysicalRenderer(int fbW, int fbH, uint8_t threshold)
    : fbW_(fbW), fbH_(fbH), threshold_(threshold) {}

void PhysicalRenderer::clear(OledFb& fb) { fb.clear(); }

uint8_t PhysicalRenderer::luminance(uint16_t rgb565) {
    // R5/G6/B5 放大到 8bit 满量程（31->255、63->255，等价左移补位后归一）。
    const uint32_t r = ((rgb565 >> 11) & 0x1Fu) * 255u / 31u;
    const uint32_t g = ((rgb565 >> 5) & 0x3Fu) * 255u / 63u;
    const uint32_t b = (rgb565 & 0x1Fu) * 255u / 31u;
    // Y = (299R + 587G + 114B) / 1000，+500 四舍五入。
    return static_cast<uint8_t>((299u * r + 587u * g + 114u * b + 500u) / 1000u);
}

uint8_t PhysicalRenderer::thresholded(uint16_t rgb565, uint8_t th) {
    return luminance(rgb565) >= th ? 1u : 0u;
}

void PhysicalRenderer::renderFrame(OledFb& fb, int srcW, int srcH,
                                   const uint8_t* rgb565,
                                   const RenderRect& srcRect) {
    if (srcW <= 0 || srcH <= 0) {
        clear(fb);  // 源尺寸非法 -> 清屏
        return;
    }
    if (rgb565 == nullptr || srcRect.w <= 0 || srcRect.h <= 0) {
        return;  // no-op（错误路径无异常）
    }

    // 越界裁剪：只处理 srcRect 与 [0, srcW) x [0, srcH) 的交集。
    int64_t rx0 = srcRect.x;
    int64_t ry0 = srcRect.y;
    int64_t rx1 = static_cast<int64_t>(srcRect.x) + srcRect.w;
    int64_t ry1 = static_cast<int64_t>(srcRect.y) + srcRect.h;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > srcW) rx1 = srcW;
    if (ry1 > srcH) ry1 = srcH;
    if (rx0 >= rx1 || ry0 >= ry1) {
        return;  // 无交集
    }

    const int x0 = static_cast<int>(rx0);
    const int y0 = static_cast<int>(ry0);
    const int x1 = static_cast<int>(rx1);
    const int y1 = static_cast<int>(ry1);

    // M8-A4：center crop 按调用期 srcH 计算（不假设源高 240）。
    const int scaledH = (srcH * kScaleNum) / kScaleDen;
    const int cropY = scaledH > fbH_ ? (scaledH - fbH_) / 2 : 0;

    const int64_t bufW = srcRect.w;  // 紧凑缓冲行步长（像素）
    const int64_t bufX0 = srcRect.x;  // 紧凑缓冲原点 = srcRect 左上角
    const int64_t bufY0 = srcRect.y;
    const uint8_t th = threshold_;
    for (int sy = y0; sy < y1; ++sy) {
        const int oy = (sy * kScaleNum) / kScaleDen - cropY;
        if (oy < 0 || oy >= fbH_) {
            continue;
        }
        const int64_t relY = static_cast<int64_t>(sy) - bufY0;
        for (int sx = x0; sx < x1; ++sx) {
            const int ox = (sx * kScaleNum) / kScaleDen;
            if (ox < 0 || ox >= fbW_) {
                continue;
            }
            // 紧凑缓冲 rect 相对索引（不越界：裁剪保证 sx/sy 落在 srcRect 内）。
            const int64_t relX = static_cast<int64_t>(sx) - bufX0;
            const size_t px =
                static_cast<size_t>((relY * bufW + relX) * 2);
            // strict aliasing 安全：逐字节小端组合，不 reinterpret_cast。
            const uint16_t v = static_cast<uint16_t>(rgb565[px]) |
                               static_cast<uint16_t>(static_cast<uint16_t>(rgb565[px + 1]) << 8);
            fb.setPixel(ox, oy, thresholded(v, th) != 0);
        }
    }
}

}  // namespace oled
}  // namespace espview
