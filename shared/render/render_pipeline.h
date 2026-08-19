// ESPView M8-C (C1) — RenderPipeline: 可组合渲染 pipeline（pure C++17）。
//
// 定位（task book §五/§六/§八/§三十）：
//   Logical Scene / 源帧 -> RenderPipeline -> Stage 1..N -> Physical Output
//   - build()：校验 stage 格式链（stage[i].inputFormat == stage[i-1].outputFormat）、
//     按源几何推演每级输出几何、一次性分配中间 scratch（init-time allocation；
//     run 期零 malloc/free）；
//   - plan()：PipelinePlan（stage 序列 + 每级几何 + scratch 总字节），供诊断/
//     benchmark 使用；
//   - run()：顺序执行 stages，中间结果写入 pipeline 自有 scratch，最终写入
//     调用方输出缓冲（容量不足 -> ok=false）。
//
// 动态分辨率：pipeline 以「源几何 + 目标几何」为单位构建；源分辨率变化时
// 由上层重建 pipeline（build 是 init-time 成本，不是 per-frame）。
// 错误路径不使用异常。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "render_format.h"
#include "render_stage.h"

namespace espview {
namespace render {

// build 成功的 pipeline 计划（只读快照；诊断 / benchmark 用）。
struct PipelinePlanEntry {
    const char* name = nullptr;
    RenderFormat inputFormat = RenderFormat::kRgb565;
    RenderFormat outputFormat = RenderFormat::kMono1;
    int inputWidth = 0;
    int inputHeight = 0;
    int outputWidth = 0;
    int outputHeight = 0;
    size_t outputBytes = 0;  // 本 stage 输出的缓冲字节数
};

struct PipelinePlan {
    std::vector<PipelinePlanEntry> stages;
    size_t totalScratchBytes = 0;  // 中间 scratch 总字节（不含最终输出）
    size_t totalOutputBytes = 0;   // 最终输出字节数
};

class RenderPipeline {
public:
    RenderPipeline() = default;
    ~RenderPipeline() = default;
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;

    // 追加 stage（pipeline 拥有；顺序 = 执行顺序）。build 前调用。
    void addStage(std::unique_ptr<IRenderStage> stage);

    // 校验格式链并规划 scratch。源几何 = (inW, inH)。
    // 失败（空链 / 格式不匹配 / 几何非法 / 输出越界）返回 false，valid()==false。
    bool build(int inW, int inH);

    bool valid() const { return valid_; }
    const PipelinePlan& plan() const { return plan_; }
    size_t scratchBytes() const { return scratch_.size(); }

    // 执行。in 必须与 build 时的源几何/首 stage 输入格式一致；
    // out.capacityBytes 必须容纳最终输出（plan().stages.back().outputBytes）。
    StageResult run(const StageInput& in, StageOutput& out);

private:
    std::vector<std::unique_ptr<IRenderStage>> stages_;
    std::vector<uint8_t> scratch_;           // 全部中间输出（连续分配，build 期）
    std::vector<StageOutput> scratchViews_;  // 每级中间输出视图（数据指针 + 几何）
    PipelinePlan plan_;
    int srcW_ = 0;
    int srcH_ = 0;
    bool valid_ = false;
};

}  // namespace render
}  // namespace espview
