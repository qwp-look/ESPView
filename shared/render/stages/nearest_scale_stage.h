// ESPView M8-C (C1) — NearestScaleStage: 最近邻缩放（格式保持；kRgb565/kGray8）。
// 输出中心映射：out(x,y) = in(min((x*inW)/outW, inW-1), min((y*inH)/outH, inH-1))。
// 确定性整数运算，无浮点。
#pragma once

#include <cstdint>

#include "render_stage.h"

namespace espview {
namespace render {

class NearestScaleStage : public IRenderStage {
public:
    // format: 输入/输出格式（保持一致）；outW/outH: 目标几何（>0）。
    NearestScaleStage(RenderFormat format, int outW, int outH)
        : format_(format), outW_(outW), outH_(outH) {}
    const char* name() const override { return "nearest-scale"; }
    RenderFormat inputFormat() const override { return format_; }
    RenderFormat outputFormat() const override { return format_; }
    bool outputSize(int inW, int inH, int& outW, int& outH) const override {
        if (inW <= 0 || inH <= 0 || outW_ <= 0 || outH_ <= 0) {
            return false;
        }
        outW = outW_;
        outH = outH_;
        return true;
    }
    StageResult run(const StageInput& in, StageOutput& out) override;

private:
    RenderFormat format_;
    int outW_;
    int outH_;
};

}  // namespace render
}  // namespace espview
