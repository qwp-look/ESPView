#include "pipeline_factory.h"

#include <memory>

#include "stages/bilinear_scale_stage.h"
#include "stages/crop_stage.h"
#include "stages/fast_scale_threshold_stage.h"
#include "stages/luminance_stage.h"
#include "stages/ordered_dither_stage.h"

namespace espview {
namespace render {

std::unique_ptr<RenderPipeline> createMonoPipeline(
    const MonoPipelineConfig& cfg, const char** errorOut) {
    if (errorOut != nullptr) {
        *errorOut = nullptr;
    }
    if (cfg.srcW <= 0 || cfg.srcH <= 0 || cfg.targetW <= 0 || cfg.targetH <= 0 ||
        cfg.scaleNum <= 0 || cfg.scaleDen <= 0) {
        if (errorOut != nullptr) {
            *errorOut = "invalid MonoPipelineConfig geometry";
        }
        return nullptr;
    }

    auto pipeline = std::make_unique<RenderPipeline>();
    if (!cfg.quality) {
        FastScaleThresholdParams p;
        p.logicalW = cfg.srcW;
        p.logicalH = cfg.srcH;
        p.scaleNum = cfg.scaleNum;
        p.scaleDen = cfg.scaleDen;
        p.targetW = cfg.targetW;
        p.targetH = cfg.targetH;
        p.threshold = cfg.threshold;
        pipeline->addStage(std::make_unique<FastScaleThresholdStage>(p));
        return pipeline;
    }

    // quality：等比适配 + center crop（与 fast 的 scale-to-width + 垂直 crop 同语义）。
    int fitW = 0;
    int fitH = 0;
    int cropX = 0;
    int cropY = 0;
    const int fitByW = (cfg.srcH * cfg.targetW) / cfg.srcW;  // 按宽缩放的高
    if (fitByW >= cfg.targetH) {
        fitW = cfg.targetW;
        fitH = fitByW;
        cropY = (fitH - cfg.targetH) / 2;
    } else {
        fitW = (cfg.srcW * cfg.targetH) / cfg.srcH;
        fitH = cfg.targetH;
        cropX = (fitW - cfg.targetW) / 2;
    }
    if (fitW <= 0 || fitH <= 0 || fitW < cfg.targetW || fitH < cfg.targetH) {
        if (errorOut != nullptr) {
            *errorOut = "quality fit geometry invalid";
        }
        return nullptr;
    }

    pipeline->addStage(std::make_unique<LuminanceStage>());
    pipeline->addStage(std::make_unique<BilinearScaleStage>(fitW, fitH));
    pipeline->addStage(std::make_unique<CropStage>(RenderFormat::kGray8, cropX,
                                                   cropY, cfg.targetW,
                                                   cfg.targetH));
    pipeline->addStage(std::make_unique<OrderedDitherStage>());
    return pipeline;
}

}  // namespace render
}  // namespace espview
