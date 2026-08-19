// ESPView M8-C (C1) — OrderedDitherStage: Gray8 -> Mono1（页式 1bpp）有序抖动。
// 4x4 Bayer 矩阵；规则（确定性，均值保持）：
//   lv = (gray * 16 + 127) / 255   // 0..16 级
//   on = lv > bayer(x,y)           // bayer 0..15
// gray=0 -> 恒 0；gray=255 -> 恒 1；gray=128 -> 每 4x4 块 8/16 亮。
// 输出为页式 kMono1（byte=8 垂直像素，bit0=页顶），与 OledFb 一致。
#pragma once

#include <cstdint>

#include "render_stage.h"

namespace espview {
namespace render {

// 4x4 Bayer 矩阵（0..15；均值保持规则见文件头）。
inline constexpr uint8_t kBayer4[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

class OrderedDitherStage : public IRenderStage {
public:
    const char* name() const override { return "ordered-dither"; }
    RenderFormat inputFormat() const override { return RenderFormat::kGray8; }
    RenderFormat outputFormat() const override { return RenderFormat::kMono1; }
    bool outputSize(int inW, int inH, int& outW, int& outH) const override {
        if (inW <= 0 || inH <= 0) {
            return false;
        }
        outW = inW;
        outH = inH;
        return true;
    }
    StageResult run(const StageInput& in, StageOutput& out) override;

    // 测试用：4x4 Bayer 矩阵（C++17：constexpr 数组在 namespace 作用域，不在函数内）。
    static constexpr uint8_t bayerAt(int x, int y) { return kBayer4[(y & 3)][(x & 3)]; }
};

}  // namespace render
}  // namespace espview
