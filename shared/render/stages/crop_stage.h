// ESPView M8-C (C1) — CropStage: 格式保持中心/显式裁剪（kRgb565/kGray8/kMono1）。
// 规则：out = in[y0..y0+h) x [x0..x0+w)；x0/y0/w/h 在构造期固定；
// 越界（x0+w > inW 等）在 outputSize 阶段拒绝。
#pragma once

#include <cstdint>

#include "render_stage.h"

namespace espview {
namespace render {

class CropStage : public IRenderStage {
public:
    CropStage(RenderFormat format, int x, int y, int w, int h)
        : format_(format), x_(x), y_(y), w_(w), h_(h) {}
    const char* name() const override { return "crop"; }
    RenderFormat inputFormat() const override { return format_; }
    RenderFormat outputFormat() const override { return format_; }
    bool outputSize(int inW, int inH, int& outW, int& outH) const override {
        if (inW <= 0 || inH <= 0 || w_ <= 0 || h_ <= 0 || x_ < 0 || y_ < 0 ||
            x_ + w_ > inW || y_ + h_ > inH) {
            return false;
        }
        outW = w_;
        outH = h_;
        return true;
    }
    StageResult run(const StageInput& in, StageOutput& out) override;

private:
    RenderFormat format_;
    int x_;
    int y_;
    int w_;
    int h_;
};

}  // namespace render
}  // namespace espview
