// ESPView M8-C (C1) — PipelineFactory: profile/config -> RenderPipeline（pure C++17）。
//
// 定位（task §九/§十三）：算法选择由 profile -> pipeline factory/config 统一决定，
// 不在业务代码散落 `#if ESP32`。两条 v0.1 组合：
//   fast（classic ESP32，低 CPU/低 RAM）：
//     RGB565 -> FastScaleThresholdStage（2/5 最近邻 + center crop + 阈值，单 stage
//     直通，零中间 scratch）-> Mono1（页式）
//   quality（ESP32-S3，可选 PSRAM）：
//     RGB565 -> Luminance -> Gray8 -> BilinearScale（等比适配）-> Crop（center）
//     -> OrderedDither（4x4 Bayer）-> Mono1（页式）
// 两条组合输出同一几何（targetW x targetH 页式 kMono1），逻辑行为一致。
#pragma once

#include <cstdint>
#include <memory>

#include "render_pipeline.h"

namespace espview {
namespace render {

struct MonoPipelineConfig {
    int srcW = 320;          // 逻辑源帧宽
    int srcH = 240;          // 逻辑源帧高
    int targetW = 128;       // 物理输出宽
    int targetH = 64;        // 物理输出高（页式建议 8 的倍数）
    uint8_t threshold = 128; // 阈值（fast 直通 + quality dither 前参考）
    bool quality = false;    // false = fast（classic）；true = quality（S3）
    int scaleNum = 2;        // fast path 降采样分子
    int scaleDen = 5;        // fast path 降采样分母
};

// 创建 mono pipeline；失败返回 nullptr 并（可选）写错误描述。
// 成功后调用 pipeline->build(srcW, srcH)；build 失败返回 false（同 nullptr 语义，
// 由调用方检查 build 返回值）。
std::unique_ptr<RenderPipeline> createMonoPipeline(const MonoPipelineConfig& cfg,
                                                   const char** errorOut = nullptr);

// 便捷组合名（诊断/日志用）。
inline const char* monoPipelineName(const MonoPipelineConfig& cfg) {
    return cfg.quality ? "quality" : "fast";
}

}  // namespace render
}  // namespace espview
