// ESPView M8-C (C1) — BilinearScaleStage: Gray8 双线性缩放（quality path）。
// 确定性定点整数实现：u = (x * (inW-1) * 256) / (outW-1)，权重 8bit；
// out = (4 邻域加权和 + 32768) >> 16。out(0,0)=in(0,0)、
// out(W-1,H-1)=in(inW-1,inH-1) 精确。outW/outH 为 1 时退化到最近邻角点。
#pragma once

#include <cstdint>

#include "render_stage.h"

namespace espview {
namespace render {

class BilinearScaleStage : public IRenderStage {
public:
    BilinearScaleStage(int outW, int outH) : outW_(outW), outH_(outH) {}
    const char* name() const override { return "bilinear-scale"; }
    RenderFormat inputFormat() const override { return RenderFormat::kGray8; }
    RenderFormat outputFormat() const override { return RenderFormat::kGray8; }
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
    int outW_;
    int outH_;
};

}  // namespace render
}  // namespace espview
