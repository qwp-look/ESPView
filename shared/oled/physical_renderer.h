// ESPView M7-C2 — PhysicalRenderer：RGB565 → Mono1（1bpp 页式 OledFb）渲染器。
//
// 映射策略（DESIGN.md C2 定稿，必须精确实现）：
//   320x240 RGB565 -> 128x96（aspect-preserving，scale = 0.4 = 2/5，最近邻）
//   -> center crop 垂直 -> 128x64（crop = (240*2/5 - 64)/2 = 16）。
// 即 src(sx, sy) 映射到 oled(floor(sx*0.4), floor(sy*0.4) - cropY)；
// 可见源区域 x∈[0,319]、y∈[40,199]。
//
// M8-A4 契约修正（OOB 越界读修复，Agent F/P Blocker）：rgb565 是「紧凑矩形
// 缓冲」——srcRect.w x srcRect.h 行主序，行步长 = srcRect.w，而非整帧步长。
// LVGL flush_cb 只持有 band 相对缓冲（如 320x24），按整帧绝对坐标索引任何
// y>0 的 band 都会越界读（可达 ~138KB）。索引一律 rect 相对：
//   px = ((sy - srcRect.y) * srcRect.w + (sx - srcRect.x)) * 2
// srcRect.x/y 仍是全帧逻辑坐标（scale 映射与裁剪用）；srcW/srcH 为全帧尺寸，
// 仅用于逻辑边界裁剪；scale/crop 按调用期 srcH 计算（不假设 240）。
// 纯 C++17、零平台依赖、无堆分配、错误路径无异常；strict aliasing 安全
// （RGB565 逐字节 LE 组合，不 reinterpret_cast 为 uint16_t*）。
#pragma once

#include <cstdint>

#include "oled_fb.h"

namespace espview {
namespace oled {

// 源矩形（增量渲染：只更新其映射到的 OLED 像素，其余像素不动）。
struct RenderRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

class PhysicalRenderer {
public:
    // 固定降采样比例：scale = 2/5（320x240 → 128x96）。
    static constexpr int kScaleNum = 2;
    static constexpr int kScaleDen = 5;

    // fbW/fbH = 目标 OLED 尺寸；crop 偏移按每次 renderFrame 的 srcH 计算。
    PhysicalRenderer(int fbW = OledFb::kWidth, int fbH = OledFb::kHeight,
                     uint8_t threshold = 128);

    // 把 srcRect 的 RGB565（紧凑 rect.w x rect.h 行主序，小端 uint16，逐字节
    // 访问）渲染进 1KB 页式 OledFb。映射：ox = floor(sx*0.4)，
    // oy = floor(sy*0.4) - cropY（128x64 目标时 cropY = 16）。
    // srcRect 越界裁剪（只处理交集）；w/h<=0 时 no-op；srcW/srcH<=0 时 clear。
    void renderFrame(OledFb& fb, int srcW, int srcH, const uint8_t* rgb565,
                     const RenderRect& srcRect);

    void clear(OledFb& fb);  // 全 0

    int fbWidth() const { return fbW_; }
    int fbHeight() const { return fbH_; }
    uint8_t threshold() const { return threshold_; }

    // 工具（测试用）：RGB565 -> 近似亮度。R5/G6/B5 先放大到 8bit 满量程
    // （31→255、63→255，等价于左移补位后的满量程归一），
    // Y = (299R + 587G + 114B + 500) / 1000（四舍五入）。
    // 锚点：白 0xFFFF→255、红 0xF800→76、绿 0x07E0→150、蓝 0x001F→29。
    static uint8_t luminance(uint16_t rgb565);
    // Y >= th -> 1，否则 0。
    static uint8_t thresholded(uint16_t rgb565, uint8_t th);

private:
    int fbW_;
    int fbH_;
    uint8_t threshold_;
};

}  // namespace oled
}  // namespace espview
