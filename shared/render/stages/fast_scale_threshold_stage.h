// ESPView M8-C (C1) — FastScaleThresholdStage: RGB565 -> Mono1（页式）快速路径
// 专用 stage（classic ESP32 低 CPU / 低 RAM；task §八 允许的 hot-path 专用化）。
//
// 算法 = M7-C2 PhysicalRenderer 定稿映射的精确移植（source-driven、行主序
// last-write-wins、逐字节小端组合，strict aliasing 安全）：
//   ox = floor(logicalX * scaleNum / scaleDen)
//   oy = floor(logicalY * scaleNum / scaleDen) - cropY
//   cropY = (scaledH - targetH) / 2，scaledH = floor(logicalH * scaleNum / scaleDen)
// 输入契约 = M8-A4 紧凑 rect 缓冲：data 为 in.width x in.height 行主序 RGB565
// （行步长 = in.width * 2），(offsetX, offsetY) = 缓冲在逻辑帧中的原点。
// 越界逻辑像素（<0 或 >= logicalW/H）跳过；输出越界跳过；未覆盖的输出像素
// 保持原值（增量语义，与 PhysicalRenderer 一致；调用方负责首帧清屏）。
#pragma once

#include <cstdint>

#include "render_stage.h"

namespace espview {
namespace render {

struct FastScaleThresholdParams {
    int logicalW = 320;    // 逻辑帧宽（>0）
    int logicalH = 240;    // 逻辑帧高（>0）
    int scaleNum = 2;      // 降采样分子（>0）
    int scaleDen = 5;      // 降采样分母（>0）
    int targetW = 128;     // 输出宽（>0）
    int targetH = 64;      // 输出高（>0，建议 8 的倍数 = 页对齐）
    uint8_t threshold = 128;
};

class FastScaleThresholdStage : public IRenderStage {
public:
    explicit FastScaleThresholdStage(const FastScaleThresholdParams& params)
        : params_(params) {}
    const char* name() const override { return "fast-scale-threshold"; }
    RenderFormat inputFormat() const override { return RenderFormat::kRgb565; }
    RenderFormat outputFormat() const override { return RenderFormat::kMono1; }
    bool outputSize(int inW, int inH, int& outW, int& outH) const override;
    StageResult run(const StageInput& in, StageOutput& out) override;

    const FastScaleThresholdParams& params() const { return params_; }

private:
    FastScaleThresholdParams params_;
};

}  // namespace render
}  // namespace espview
