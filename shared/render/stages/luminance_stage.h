// ESPView M8-C (C1) — LuminanceStage: RGB565 -> Gray8（pure C++17）。
// 亮度公式单一实现见 render_color.h（与 PhysicalRenderer::luminance 收敛）。
#pragma once

#include <cstdint>

#include "render_stage.h"

namespace espview {
namespace render {

class LuminanceStage : public IRenderStage {
public:
    const char* name() const override { return "luminance"; }
    RenderFormat inputFormat() const override { return RenderFormat::kRgb565; }
    RenderFormat outputFormat() const override { return RenderFormat::kGray8; }
    bool outputSize(int inW, int inH, int& outW, int& outH) const override {
        if (inW <= 0 || inH <= 0) {
            return false;
        }
        outW = inW;
        outH = inH;
        return true;
    }
    StageResult run(const StageInput& in, StageOutput& out) override;
};

}  // namespace render
}  // namespace espview
