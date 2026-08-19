// ESPView M8-C (C1) — ThresholdStage: Gray8 -> Mono1（页式 1bpp）阈值量化。
// 规则：Y >= threshold -> 1（与 PhysicalRenderer::thresholded 一致）。
#pragma once

#include <cstdint>

#include "render_stage.h"

namespace espview {
namespace render {

class ThresholdStage : public IRenderStage {
public:
    explicit ThresholdStage(uint8_t threshold = 128) : threshold_(threshold) {}
    const char* name() const override { return "threshold"; }
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

private:
    uint8_t threshold_;
};

}  // namespace render
}  // namespace espview
