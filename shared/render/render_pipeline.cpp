// ESPView M8-C (C1) — RenderPipeline 实现（见 render_pipeline.h）。
#include "render_pipeline.h"

namespace espview {
namespace render {

void RenderPipeline::addStage(std::unique_ptr<IRenderStage> stage) {
    if (valid_) {
        return;  // build 后不可再改（契约：先组链再 build）
    }
    if (stage) {
        stages_.push_back(std::move(stage));
    }
}

bool RenderPipeline::build(int inW, int inH) {
    valid_ = false;
    plan_ = PipelinePlan{};
    scratch_.clear();
    scratchViews_.clear();

    if (stages_.empty() || inW <= 0 || inH <= 0) {
        return false;
    }

    // 格式链校验 + 几何推演。
    int curW = inW;
    int curH = inH;
    size_t scratchTotal = 0;
    for (size_t i = 0; i < stages_.size(); ++i) {
        const IRenderStage& s = *stages_[i];
        if (i > 0 && s.inputFormat() != stages_[i - 1]->outputFormat()) {
            return false;  // 格式不匹配 -> build 拒绝（task §七）
        }
        int outW = 0;
        int outH = 0;
        if (!s.outputSize(curW, curH, outW, outH)) {
            return false;
        }
        if (outW <= 0 || outH <= 0) {
            return false;
        }
        const size_t outBytes = formatBufferBytes(s.outputFormat(), outW, outH);
        PipelinePlanEntry e;
        e.name = s.name();
        e.inputFormat = s.inputFormat();
        e.outputFormat = s.outputFormat();
        e.inputWidth = curW;
        e.inputHeight = curH;
        e.outputWidth = outW;
        e.outputHeight = outH;
        e.outputBytes = outBytes;
        plan_.stages.push_back(e);
        if (i + 1 < stages_.size()) {
            scratchTotal += outBytes;
        } else {
            plan_.totalOutputBytes = outBytes;
        }
        curW = outW;
        curH = outH;
    }

    // 中间 scratch 连续分配（init-time；run 期零分配）。
    scratch_.assign(scratchTotal, 0u);
    scratchViews_.resize(stages_.size() - 1);
    size_t offset = 0;
    for (size_t i = 0; i + 1 < stages_.size(); ++i) {
        const PipelinePlanEntry& e = plan_.stages[i];
        StageOutput view;
        view.format = e.outputFormat;
        view.width = e.outputWidth;
        view.height = e.outputHeight;
        view.data = scratch_.data() + offset;
        view.capacityBytes = e.outputBytes;
        scratchViews_[i] = view;
        offset += e.outputBytes;
    }
    plan_.totalScratchBytes = scratchTotal;
    srcW_ = inW;
    srcH_ = inH;
    valid_ = true;
    return true;
}

StageResult RenderPipeline::run(const StageInput& in, StageOutput& out) {
    if (!valid_ || in.data == nullptr || out.data == nullptr) {
        return StageResult{};
    }
    if (in.format != stages_[0]->inputFormat() || in.width <= 0 ||
        in.height <= 0 || in.width > srcW_ || in.height > srcH_) {
        return StageResult{};
    }
    const size_t inBytes = formatBufferBytes(in.format, in.width, in.height);
    if (in.bytes < inBytes) {
        return StageResult{};  // 输入缓冲不足
    }
    const size_t finalBytes = plan_.totalOutputBytes;
    if (out.capacityBytes < finalBytes ||
        out.format != stages_.back()->outputFormat()) {
        return StageResult{};
    }

    StageInput cur = in;
    for (size_t i = 0; i < stages_.size(); ++i) {
        StageOutput dst;
        if (i + 1 < stages_.size()) {
            dst = scratchViews_[i];
        } else {
            dst = out;
            dst.width = plan_.stages.back().outputWidth;
            dst.height = plan_.stages.back().outputHeight;
            dst.capacityBytes = finalBytes;
        }
        const StageResult r = stages_[i]->run(cur, dst);
        if (!r.ok) {
            return r;
        }
        cur.format = dst.format;
        cur.width = dst.width;
        cur.height = dst.height;
        cur.data = dst.data;
        cur.bytes = r.bytesWritten;
    }
    StageResult ok;
    ok.ok = true;
    ok.bytesWritten = finalBytes;
    return ok;
}

}  // namespace render
}  // namespace espview

