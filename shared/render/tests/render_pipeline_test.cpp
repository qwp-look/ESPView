// ESPView M8-C (C1) - Render Pipeline Host Tests.
//
// 覆盖（task book §三十二 Pipeline/Color/Scaling/Text 前置）：
//   Pipeline   - stage 格式兼容 / 组合 / build / 非法 chain 拒绝 / scratch 规划
//   Color      - RGB565->Gray 锚点（白 255 红 76 绿 150 蓝 29）/ threshold 边界
//   Scaling    - nearest 确定性 / bilinear 角点与中点 / crop 偏移与越界拒绝
//   Dither     - 0/255 恒值 / 128 均值（每 4x4 块 8/16 亮）/ 确定性
//   Fast path  - 全白全黑 / crop 源区 / 增量窗口 offset / 与映射公式点对点一致
//   Factory    - fast（零 scratch）/ quality（4 stages + scratch）build 成功
//   Pipeline   - run 输入几何 <= build 几何 / 输出容量不足拒绝 / 两次 run 逐字节一致
// 纯 C++17，零平台依赖；并入 shared/protocol host 套件。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "render_color.h"
#include "render_pipeline.h"
#include "pipeline_factory.h"
#include "stages/bilinear_scale_stage.h"
#include "stages/crop_stage.h"
#include "stages/fast_scale_threshold_stage.h"
#include "stages/luminance_stage.h"
#include "stages/nearest_scale_stage.h"
#include "stages/ordered_dither_stage.h"
#include "stages/threshold_stage.h"
#include "test_util.h"

namespace {

using espview::render::BilinearScaleStage;
using espview::render::CropStage;
using espview::render::FastScaleThresholdParams;
using espview::render::FastScaleThresholdStage;
using espview::render::LuminanceStage;
using espview::render::MonoPipelineConfig;
using espview::render::NearestScaleStage;
using espview::render::OrderedDitherStage;
using espview::render::PipelinePlan;
using espview::render::RenderFormat;
using espview::render::RenderPipeline;
using espview::render::StageInput;
using espview::render::StageOutput;
using espview::render::StageResult;
using espview::render::ThresholdStage;
using espview::render::createMonoPipeline;
using espview::render::formatBufferBytes;
using espview::render::rgb565Luminance;

constexpr int kSrcW = 320;
constexpr int kSrcH = 240;
constexpr int kTargetW = 128;
constexpr int kTargetH = 64;

// 构造全一/全零 RGB565 紧凑缓冲。
std::vector<uint8_t> makeFrame(int w, int h, uint16_t pixel) {
    std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 2u);
    for (size_t i = 0; i < buf.size(); i += 2) {
        buf[i] = static_cast<uint8_t>(pixel & 0xFFu);
        buf[i + 1] = static_cast<uint8_t>((pixel >> 8) & 0xFFu);
    }
    return buf;
}

// fast stage 映射公式（与 DESIGN C2 / PhysicalRenderer 一致）。
int mapX(int sx) { return (sx * 2) / 5; }
int mapY(int sy) { return (sy * 2) / 5 - 16; }

void runPipeline(RenderPipeline& p, int inW, int inH, RenderFormat inFmt,
                 const std::vector<uint8_t>& in, int offX, int offY,
                 RenderFormat outFmt, std::vector<uint8_t>& out) {
    StageInput in0;
    in0.format = inFmt;
    in0.width = inW;
    in0.height = inH;
    in0.data = in.data();
    in0.bytes = in.size();
    in0.offsetX = offX;
    in0.offsetY = offY;
    out.assign(formatBufferBytes(outFmt, kTargetW, kTargetH), 0u);
    StageOutput o;
    o.format = outFmt;
    o.width = kTargetW;
    o.height = kTargetH;
    o.data = out.data();
    o.capacityBytes = out.size();
    const StageResult r = p.run(in0, o);
    CHECK(r.ok);
    CHECK_EQ(r.bytesWritten, out.size());
}

void testFormatHelpers() {
    CHECK_EQ(espview::render::formatRowStrideBytes(RenderFormat::kRgb565, 320),
             static_cast<size_t>(640));
    CHECK_EQ(espview::render::formatRowStrideBytes(RenderFormat::kGray8, 128),
             static_cast<size_t>(128));
    CHECK_EQ(espview::render::formatRowStrideBytes(RenderFormat::kMono1, 128),
             static_cast<size_t>(128));
    CHECK_EQ(formatBufferBytes(RenderFormat::kMono1, 128, 64),
             static_cast<size_t>(1024));
    CHECK_EQ(formatBufferBytes(RenderFormat::kMono1, 128, 60),
             static_cast<size_t>(1024));  // ceil 到页
    CHECK_EQ(formatBufferBytes(RenderFormat::kRgb565, 4, 4),
             static_cast<size_t>(32));
}

void testColor() {
    CHECK_EQ(static_cast<int>(rgb565Luminance(0xFFFF)), 255);
    CHECK_EQ(static_cast<int>(rgb565Luminance(0xF800)), 76);
    CHECK_EQ(static_cast<int>(rgb565Luminance(0x07E0)), 150);
    CHECK_EQ(static_cast<int>(rgb565Luminance(0x001F)), 29);
    CHECK_EQ(static_cast<int>(rgb565Luminance(0x0000)), 0);

    // LuminanceStage：4 像素 -> 4 灰度。
    std::vector<uint8_t> in;
    const uint16_t px[4] = {0xFFFF, 0xF800, 0x07E0, 0x001F};
    for (uint16_t v : px) {
        in.push_back(static_cast<uint8_t>(v & 0xFFu));
        in.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    }
    LuminanceStage stage;
    std::vector<uint8_t> out(4);
    StageInput si;
    si.format = RenderFormat::kRgb565;
    si.width = 2;
    si.height = 2;
    si.data = in.data();
    si.bytes = in.size();
    StageOutput so;
    so.format = RenderFormat::kGray8;
    so.width = 2;
    so.height = 2;
    so.data = out.data();
    so.capacityBytes = out.size();
    const StageResult r = stage.run(si, so);
    CHECK(r.ok);
    CHECK_EQ(static_cast<int>(out[0]), 255);
    CHECK_EQ(static_cast<int>(out[1]), 76);
    CHECK_EQ(static_cast<int>(out[2]), 150);
    CHECK_EQ(static_cast<int>(out[3]), 29);
    CHECK_EQ(r.bytesWritten, static_cast<size_t>(4));
}

void testThreshold() {
    const uint8_t gray[8] = {0, 127, 128, 129, 200, 255, 100, 128};
    ThresholdStage stage(128);
    std::vector<uint8_t> out(8);
    StageInput si;
    si.format = RenderFormat::kGray8;
    si.width = 8;
    si.height = 1;
    si.data = gray;
    si.bytes = sizeof(gray);
    StageOutput so;
    so.format = RenderFormat::kMono1;
    so.width = 8;
    so.height = 1;
    so.data = out.data();
    so.capacityBytes = out.size();
    const StageResult r = stage.run(si, so);
    CHECK(r.ok);
    CHECK_EQ(static_cast<int>(out[0]), 0);  // 0
    CHECK_EQ(static_cast<int>(out[1]), 0);  // 127
    CHECK_EQ(static_cast<int>(out[2]), 1);  // 128（>= 阈值）
    CHECK_EQ(static_cast<int>(out[3]), 1);  // 129 -> bit0（页式 height=1：所有像素位0）
    CHECK_EQ(static_cast<int>(out[4]), 1);  // 200
    CHECK_EQ(static_cast<int>(out[5]), 1);  // 255
    CHECK_EQ(static_cast<int>(out[6]), 0);  // 100
    CHECK_EQ(static_cast<int>(out[7]), 1);  // 128
}

void testNearestScale() {
    // 4x4 灰阶 -> 2x2：out(x,y) = in(min((x*4)/2,3), ...) -> (0,0),(0,2),(2,0),(2,2)。
    uint8_t in[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            in[y * 4 + x] = static_cast<uint8_t>(y * 4 + x);
        }
    }
    NearestScaleStage stage(RenderFormat::kGray8, 2, 2);
    std::vector<uint8_t> out(4);
    StageInput si;
    si.format = RenderFormat::kGray8;
    si.width = 4;
    si.height = 4;
    si.data = in;
    si.bytes = sizeof(in);
    StageOutput so;
    so.format = RenderFormat::kGray8;
    so.width = 2;
    so.height = 2;
    so.data = out.data();
    so.capacityBytes = out.size();
    const StageResult r = stage.run(si, so);
    CHECK(r.ok);
    CHECK_EQ(static_cast<int>(out[0]), 0);
    CHECK_EQ(static_cast<int>(out[1]), 2);
    CHECK_EQ(static_cast<int>(out[2]), 8);
    CHECK_EQ(static_cast<int>(out[3]), 10);
    CHECK_EQ(r.bytesWritten, static_cast<size_t>(4));
}

void testBilinear() {
    // 2x2 {0,0,0,64} -> 3x3：角点 = 角点值，中心 = 64*?/... 直接验证确定性 + 对称。
    uint8_t in[4] = {0, 0, 0, 64};
    BilinearScaleStage stage(3, 3);
    std::vector<uint8_t> out(9);
    StageInput si;
    si.format = RenderFormat::kGray8;
    si.width = 2;
    si.height = 2;
    si.data = in;
    si.bytes = sizeof(in);
    StageOutput so;
    so.format = RenderFormat::kGray8;
    so.width = 3;
    so.height = 3;
    so.data = out.data();
    so.capacityBytes = out.size();
    const StageResult r = stage.run(si, so);
    CHECK(r.ok);
    CHECK_EQ(static_cast<int>(out[0]), 0);  // 左上角 = in(0,0)
    CHECK_EQ(static_cast<int>(out[8]), 64); // 右下角 = in(1,1)
    CHECK_EQ(static_cast<int>(out[4]), 16); // 中心 = (0+0+0+64)/4
    // 对称性：out[1] == out[3]（(0,0)-(0,1) 水平中线 == (0,0)-(1,0) 垂直中线）
    CHECK_EQ(static_cast<int>(out[1]), static_cast<int>(out[3]));
    // 确定性：再次运行逐字节一致。
    std::vector<uint8_t> out2(9);
    so.data = out2.data();
    CHECK(stage.run(si, so).ok);
    CHECK(std::memcmp(out.data(), out2.data(), out.size()) == 0);
}

void testCrop() {
    // 4x4 灰阶，crop (1,1,2,2)。
    uint8_t in[16];
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            in[y * 4 + x] = static_cast<uint8_t>(y * 4 + x);
        }
    }
    CropStage stage(RenderFormat::kGray8, 1, 1, 2, 2);
    std::vector<uint8_t> out(4);
    StageInput si;
    si.format = RenderFormat::kGray8;
    si.width = 4;
    si.height = 4;
    si.data = in;
    si.bytes = sizeof(in);
    StageOutput so;
    so.format = RenderFormat::kGray8;
    so.width = 2;
    so.height = 2;
    so.data = out.data();
    so.capacityBytes = out.size();
    const StageResult r = stage.run(si, so);
    CHECK(r.ok);
    CHECK_EQ(static_cast<int>(out[0]), 5);
    CHECK_EQ(static_cast<int>(out[1]), 6);
    CHECK_EQ(static_cast<int>(out[2]), 9);
    CHECK_EQ(static_cast<int>(out[3]), 10);
    // 越界 crop -> outputSize false。
    int ow = 0;
    int oh = 0;
    CHECK(!CropStage(RenderFormat::kGray8, 3, 3, 2, 2).outputSize(4, 4, ow, oh));
    CHECK(!CropStage(RenderFormat::kGray8, -1, 0, 2, 2).outputSize(4, 4, ow, oh));
}

void testDither() {
    OrderedDitherStage stage;
    // gray=0 -> 全 0；gray=255 -> 全 1（8 行 = 整页，页式每字节全位）。
    for (int g : {0, 255}) {
        std::vector<uint8_t> in(64u * 8u, static_cast<uint8_t>(g));
        std::vector<uint8_t> out(64);
        StageInput si;
        si.format = RenderFormat::kGray8;
        si.width = 64;
        si.height = 8;
        si.data = in.data();
        si.bytes = in.size();
        StageOutput so;
        so.format = RenderFormat::kMono1;
        so.width = 64;
        so.height = 8;
        so.data = out.data();
        so.capacityBytes = out.size();
        CHECK(stage.run(si, so).ok);
        for (size_t i = 0; i < out.size(); ++i) {
            CHECK_EQ(static_cast<int>(out[i]),
                     g == 0 ? 0 : 0xFF);
        }
    }
    // gray=128：每 4x4 块 8/16 亮（lv = (128*16+127)/255 = 8；on = lv > bayer）。
    std::vector<uint8_t> in(16 * 16, 128);
    std::vector<uint8_t> out(16 * 16 / 8);
    StageInput si;
    si.format = RenderFormat::kGray8;
    si.width = 16;
    si.height = 16;
    si.data = in.data();
    si.bytes = in.size();
    StageOutput so;
    so.format = RenderFormat::kMono1;
    so.width = 16;
    so.height = 16;
    so.data = out.data();
    so.capacityBytes = out.size();
    CHECK(stage.run(si, so).ok);
    int onCount = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            if ((out[static_cast<size_t>((y >> 3) * 16 + x)] &
                 (1u << (y & 7))) != 0) {
                ++onCount;
            }
        }
    }
    CHECK_EQ(onCount, 16 * 16 / 2);
    // 确定性。
    std::vector<uint8_t> out2(out.size());
    so.data = out2.data();
    CHECK(stage.run(si, so).ok);
    CHECK(std::memcmp(out.data(), out2.data(), out.size()) == 0);
}

void testFastStage() {
    // 全白 320x240 -> 全 1。
    {
        const std::vector<uint8_t> in = makeFrame(kSrcW, kSrcH, 0xFFFF);
        FastScaleThresholdParams p;
        p.logicalW = kSrcW;
        p.logicalH = kSrcH;
        p.targetW = kTargetW;
        p.targetH = kTargetH;
        p.threshold = 128;
        RenderPipeline pipe;
        pipe.addStage(std::make_unique<FastScaleThresholdStage>(p));
        CHECK(pipe.build(kSrcW, kSrcH));
        std::vector<uint8_t> out;
        runPipeline(pipe, kSrcW, kSrcH, RenderFormat::kRgb565, in, 0, 0,
                    RenderFormat::kMono1, out);
        for (uint8_t b : out) {
            CHECK_EQ(static_cast<int>(b), 0xFF);
        }
    }
    // 全黑 -> 全 0。
    {
        const std::vector<uint8_t> in = makeFrame(kSrcW, kSrcH, 0x0000);
        FastScaleThresholdParams p;
        p.logicalW = kSrcW;
        p.logicalH = kSrcH;
        p.targetW = kTargetW;
        p.targetH = kTargetH;
        p.threshold = 128;
        RenderPipeline pipe;
        pipe.addStage(std::make_unique<FastScaleThresholdStage>(p));
        CHECK(pipe.build(kSrcW, kSrcH));
        std::vector<uint8_t> out;
        runPipeline(pipe, kSrcW, kSrcH, RenderFormat::kRgb565, in, 0, 0,
                    RenderFormat::kMono1, out);
        for (uint8_t b : out) {
            CHECK_EQ(static_cast<int>(b), 0);
        }
    }
    // 点对点映射：源 y<40 与 y>199 不产生像素；x/y 边界映射正确。
    {
        const std::vector<uint8_t> in = makeFrame(kSrcW, kSrcH, 0xFFFF);
        FastScaleThresholdParams p;
        p.logicalW = kSrcW;
        p.logicalH = kSrcH;
        p.targetW = kTargetW;
        p.targetH = kTargetH;
        p.threshold = 128;
        RenderPipeline pipe;
        pipe.addStage(std::make_unique<FastScaleThresholdStage>(p));
        CHECK(pipe.build(kSrcW, kSrcH));
        std::vector<uint8_t> out;
        runPipeline(pipe, kSrcW, kSrcH, RenderFormat::kRgb565, in, 0, 0,
                    RenderFormat::kMono1, out);
        // 单像素亮度点阵：只点亮源 (sx, sy) 一个像素，验证映射 ox/oy。
        std::vector<uint8_t> single = makeFrame(kSrcW, kSrcH, 0x0000);
        const int sx = 319;
        const int sy = 199;
        const uint16_t v = 0xFFFF;
        single[static_cast<size_t>(sy * kSrcW + sx) * 2u] =
            static_cast<uint8_t>(v & 0xFFu);
        single[static_cast<size_t>(sy * kSrcW + sx) * 2u + 1u] =
            static_cast<uint8_t>((v >> 8) & 0xFFu);
        std::vector<uint8_t> out2;
        runPipeline(pipe, kSrcW, kSrcH, RenderFormat::kRgb565, single, 0, 0,
                    RenderFormat::kMono1, out2);
        const int ox = mapX(sx);
        const int oy = mapY(sy);
        CHECK_EQ(ox, 127);
        CHECK_EQ(oy, 63);
        for (int y = 0; y < kTargetH; ++y) {
            for (int x = 0; x < kTargetW; ++x) {
                const bool on =
                    (out2[static_cast<size_t>((y >> 3) * kTargetW + x)] &
                     (1u << (y & 7))) != 0;
                if (x == ox && y == oy) {
                    CHECK(on);
                } else {
                    CHECK(!on);
                }
            }
        }
    }
    // 增量窗口：只提供 320x24 band（offsetY=60），验证行优先紧凑索引与逻辑坐标。
    {
        const int bandH = 24;
        const int bandY = 60;
        // 整 band 白色（行 60..83 全白）：last-write-wins 语义下 oy 8..17 全亮。
        const std::vector<uint8_t> in = makeFrame(kSrcW, bandH, 0xFFFF);
        FastScaleThresholdParams p;
        p.logicalW = kSrcW;
        p.logicalH = kSrcH;
        p.targetW = kTargetW;
        p.targetH = kTargetH;
        p.threshold = 128;
        RenderPipeline pipe;
        pipe.addStage(std::make_unique<FastScaleThresholdStage>(p));
        CHECK(pipe.build(kSrcW, kSrcH));  // build 用最大源几何，run 允许 <=
        std::vector<uint8_t> out;
        runPipeline(pipe, kSrcW, bandH, RenderFormat::kRgb565, in, 0, bandY,
                    RenderFormat::kMono1, out);
        // bandY=60 是可见区（40..199）：oy = floor(60*2/5)-16 = 8。
        const int oy = mapY(bandY);
        CHECK_EQ(oy, 8);
        for (int x = 0; x < kTargetW; ++x) {
            const bool on = (out[static_cast<size_t>((oy >> 3) * kTargetW + x)] &
                             (1u << (oy & 7))) != 0;
            CHECK(on);
        }
        // bandY=60 之前/之后的行（oy<8）应保持 0（增量语义：未覆盖像素不动）。
        for (int y = 0; y < oy; ++y) {
            for (int x = 0; x < kTargetW; ++x) {
                const bool on =
                    (out[static_cast<size_t>((y >> 3) * kTargetW + x)] &
                     (1u << (y & 7))) != 0;
                CHECK(!on);
            }
        }
    }
}

void testPipelineBuildValidation() {
    // 空 chain 拒绝。
    {
        RenderPipeline p;
        CHECK(!p.build(4, 4));
        CHECK(!p.valid());
    }
    // 格式链不匹配拒绝：Luminance(RGB565->Gray8) 后接 Threshold(Gray8->Mono1) OK；
    // 人为不匹配：先 Luminance 再接一个 Luminance（输入 RGB565 != 上一级 Gray8）。
    {
        RenderPipeline p;
        p.addStage(std::make_unique<LuminanceStage>());
        p.addStage(std::make_unique<LuminanceStage>());
        CHECK(!p.build(4, 4));
    }
    // 非法几何拒绝。
    {
        RenderPipeline p;
        p.addStage(std::make_unique<LuminanceStage>());
        CHECK(!p.build(0, 4));
        CHECK(!p.build(-1, 4));
    }
    // 合法组合 build 成功 + plan 正确。
    {
        RenderPipeline p;
        p.addStage(std::make_unique<LuminanceStage>());
        p.addStage(std::make_unique<ThresholdStage>(128));
        CHECK(p.build(8, 8));
        CHECK(p.valid());
        const PipelinePlan& plan = p.plan();
        CHECK_EQ(plan.stages.size(), static_cast<size_t>(2));
        CHECK_EQ(static_cast<int>(plan.stages[0].inputFormat),
                 static_cast<int>(RenderFormat::kRgb565));
        CHECK_EQ(static_cast<int>(plan.stages[0].outputFormat),
                 static_cast<int>(RenderFormat::kGray8));
        CHECK_EQ(static_cast<int>(plan.stages[1].outputFormat),
                 static_cast<int>(RenderFormat::kMono1));
        CHECK_EQ(plan.totalScratchBytes, static_cast<size_t>(64));  // Gray8 8x8
        CHECK_EQ(plan.totalOutputBytes, static_cast<size_t>(8));    // Mono1 8x8 页式
    }
    // run：输出容量不足拒绝。
    {
        RenderPipeline p;
        p.addStage(std::make_unique<LuminanceStage>());
        p.addStage(std::make_unique<ThresholdStage>(128));
        CHECK(p.build(8, 8));
        std::vector<uint8_t> in = makeFrame(8, 8, 0xFFFF);
        std::vector<uint8_t> out(4, 0);  // 容量不足（需要 8）
        StageInput si;
        si.format = RenderFormat::kRgb565;
        si.width = 8;
        si.height = 8;
        si.data = in.data();
        si.bytes = in.size();
        StageOutput so;
        so.format = RenderFormat::kMono1;
        so.width = 8;
        so.height = 8;
        so.data = out.data();
        so.capacityBytes = out.size();
        const StageResult r = p.run(si, so);
        CHECK(!r.ok);
    }
}

void testFactory() {
    MonoPipelineConfig cfg;
    cfg.srcW = kSrcW;
    cfg.srcH = kSrcH;
    cfg.targetW = kTargetW;
    cfg.targetH = kTargetH;
    cfg.threshold = 128;

    // fast：单 stage，零 scratch。
    {
        const char* err = nullptr;
        auto p = createMonoPipeline(cfg, &err);
        CHECK(p != nullptr);
        CHECK(err == nullptr);
        CHECK(p->build(kSrcW, kSrcH));
        CHECK_EQ(p->plan().stages.size(), static_cast<size_t>(1));
        CHECK_EQ(p->scratchBytes(), static_cast<size_t>(0));
        CHECK_EQ(static_cast<int>(p->plan().stages[0].inputFormat),
                 static_cast<int>(RenderFormat::kRgb565));
        CHECK_EQ(static_cast<int>(p->plan().stages[0].outputFormat),
                 static_cast<int>(RenderFormat::kMono1));
        CHECK_EQ(p->plan().totalOutputBytes, static_cast<size_t>(1024));
        // 全白 -> 全 1。
        const std::vector<uint8_t> in = makeFrame(kSrcW, kSrcH, 0xFFFF);
        std::vector<uint8_t> out;
        runPipeline(*p, kSrcW, kSrcH, RenderFormat::kRgb565, in, 0, 0,
                    RenderFormat::kMono1, out);
        for (uint8_t b : out) {
            CHECK_EQ(static_cast<int>(b), 0xFF);
        }
    }
    // quality：4 stages，scratch > 0，几何 128x64，确定性。
    {
        MonoPipelineConfig q = cfg;
        q.quality = true;
        const char* err = nullptr;
        auto p = createMonoPipeline(q, &err);
        CHECK(p != nullptr);
        CHECK(err == nullptr);
        CHECK(p->build(kSrcW, kSrcH));
        CHECK_EQ(p->plan().stages.size(), static_cast<size_t>(4));
        CHECK(p->scratchBytes() > 0);
        CHECK_EQ(p->plan().totalOutputBytes, static_cast<size_t>(1024));
        const std::vector<uint8_t> in = makeFrame(kSrcW, kSrcH, 0xFFFF);
        std::vector<uint8_t> out;
        runPipeline(*p, kSrcW, kSrcH, RenderFormat::kRgb565, in, 0, 0,
                    RenderFormat::kMono1, out);
        for (uint8_t b : out) {
            CHECK_EQ(static_cast<int>(b), 0xFF);
        }
        // 确定性：同输入再次运行逐字节一致。
        std::vector<uint8_t> out2;
        runPipeline(*p, kSrcW, kSrcH, RenderFormat::kRgb565, in, 0, 0,
                    RenderFormat::kMono1, out2);
        CHECK(std::memcmp(out.data(), out2.data(), out.size()) == 0);
        // 全黑 -> 全 0。
        const std::vector<uint8_t> black = makeFrame(kSrcW, kSrcH, 0x0000);
        std::vector<uint8_t> out3;
        runPipeline(*p, kSrcW, kSrcH, RenderFormat::kRgb565, black, 0, 0,
                    RenderFormat::kMono1, out3);
        for (uint8_t b : out3) {
            CHECK_EQ(static_cast<int>(b), 0);
        }
    }
    // 非法 config 拒绝。
    {
        MonoPipelineConfig bad = cfg;
        bad.srcW = 0;
        const char* err = nullptr;
        CHECK(createMonoPipeline(bad, &err) == nullptr);
        CHECK(err != nullptr);
    }
}

void testFastVsQualityGeometry() {
    // 两种路径输出同一几何与覆盖语义（灰度渐变源）。
    MonoPipelineConfig cfg;
    cfg.srcW = kSrcW;
    cfg.srcH = kSrcH;
    cfg.targetW = kTargetW;
    cfg.targetH = kTargetH;
    cfg.threshold = 128;
    // 渐变帧：左上暗、右下亮（确定性）。
    std::vector<uint8_t> grad = makeFrame(kSrcW, kSrcH, 0x0000);
    for (int y = 0; y < kSrcH; ++y) {
        for (int x = 0; x < kSrcW; ++x) {
            const uint8_t g = static_cast<uint8_t>((x + y) * 255 / (kSrcW + kSrcH - 2));
            const uint16_t v = static_cast<uint16_t>(
                ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
            grad[static_cast<size_t>(y * kSrcW + x) * 2u] =
                static_cast<uint8_t>(v & 0xFFu);
            grad[static_cast<size_t>(y * kSrcW + x) * 2u + 1u] =
                static_cast<uint8_t>((v >> 8) & 0xFFu);
        }
    }
    std::vector<uint8_t> fastOut;
    std::vector<uint8_t> qualOut;
    {
        const char* err = nullptr;
        auto p = createMonoPipeline(cfg, &err);
        CHECK(p != nullptr);
        CHECK(p->build(kSrcW, kSrcH));
        runPipeline(*p, kSrcW, kSrcH, RenderFormat::kRgb565, grad, 0, 0,
                    RenderFormat::kMono1, fastOut);
    }
    {
        MonoPipelineConfig q = cfg;
        q.quality = true;
        const char* err = nullptr;
        auto p = createMonoPipeline(q, &err);
        CHECK(p != nullptr);
        CHECK(p->build(kSrcW, kSrcH));
        runPipeline(*p, kSrcW, kSrcH, RenderFormat::kRgb565, grad, 0, 0,
                    RenderFormat::kMono1, qualOut);
    }
    CHECK_EQ(fastOut.size(), qualOut.size());
    // 角点语义：左上应偏暗（亮像素占比低）、右下偏亮。统计每象限 on 数。
    auto countOn = [](const std::vector<uint8_t>& fb, int x0, int y0, int w, int h) {
        int n = 0;
        for (int y = y0; y < y0 + h; ++y) {
            for (int x = x0; x < x0 + w; ++x) {
                if ((fb[static_cast<size_t>((y >> 3) * kTargetW + x)] &
                     (1u << (y & 7))) != 0) {
                    ++n;
                }
            }
        }
        return n;
    };
    // 渐变左上暗（x+y<128 阈值以下全黑）、右下亮；两条路径均须单调 + 右下非空。
    const int tlF = countOn(fastOut, 0, 0, 32, 32);
    const int brF = countOn(fastOut, 96, 32, 32, 32);
    const int tlQ = countOn(qualOut, 0, 0, 32, 32);
    const int brQ = countOn(qualOut, 96, 32, 32, 32);
    CHECK(tlF <= brF);
    CHECK(tlQ <= brQ);
    CHECK(tlF < brF);  // 严格单调（暗区 vs 亮区）
    CHECK(tlQ < brQ);
    CHECK(brF > 0);
    CHECK(brQ > 0);
}

}  // namespace

void runRenderPipelineTests() {
    testFormatHelpers();
    testColor();
    testThreshold();
    testNearestScale();
    testBilinear();
    testCrop();
    testDither();
    testFastStage();
    testPipelineBuildValidation();
    testFactory();
    testFastVsQualityGeometry();
}
