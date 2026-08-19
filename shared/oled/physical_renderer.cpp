// ESPView M7-C2 / M8-C (C2) — PhysicalRenderer 实现（RGB565 -> Mono1 页式 OledFb）。
//
// M8-C (C2)：委托到 fast pipeline（FastScaleThresholdStage）。映射与
// last-write-wins 语义与 DESIGN.md C2 定稿字节精确等价：
//   ox = floor(sx * 0.4) = (sx * 2) / 5
//   oy = floor(sy * 0.4) - cropY（cropY 按调用期 srcH 计算）
// 可见源区域 x∈[0,319]、y∈[40,199]（srcH=240 时）。矩形增量：只更新 srcRect
// 映射到的 OLED 像素；同一 OLED 像素被多个源像素映射时，行优先遍历中最后
// 写入者生效（组内各源像素对应同一 5x5 -> 2x2 区域，结果确定）。
//
// M8-A4 紧凑缓冲契约：rgb565 = srcRect.w x srcRect.h 行主序（行步长 rect.w），
// 索引 rect 相对：px = ((sy - srcRect.y) * srcRect.w + (sx - srcRect.x)) * 2。
// 之前按整帧绝对坐标索引（sy*srcW + sx）在 LVGL band 缓冲（320x24）下越界读
// 可达 ~138KB（Agent F/P Blocker）。
#include "physical_renderer.h"

#include <cstdint>

#include "render_color.h"

namespace espview {
namespace oled {

namespace {
}  // namespace

PhysicalRenderer::PhysicalRenderer(int fbW, int fbH, uint8_t threshold)
    : fbW_(fbW), fbH_(fbH), threshold_(threshold) {}

void PhysicalRenderer::clear(OledFb& fb) { fb.clear(); }

uint8_t PhysicalRenderer::luminance(uint16_t rgb565) {
    return render::rgb565Luminance(rgb565);
}

uint8_t PhysicalRenderer::thresholded(uint16_t rgb565, uint8_t th) {
    return render::rgb565Thresholded(rgb565, th);
}

void PhysicalRenderer::ensurePipeline(int srcW, int srcH) {
    if (pipeline_ && lastSrcW_ == srcW && lastSrcH_ == srcH) {
        return;  // 复用已构建 pipeline（run 期零分配）
    }
    render::FastScaleThresholdParams p;
    p.logicalW = srcW;
    p.logicalH = srcH;
    p.scaleNum = kScaleNum;
    p.scaleDen = kScaleDen;
    p.targetW = fbW_;
    p.targetH = fbH_;
    p.threshold = threshold_;
    auto pipe = std::make_unique<render::RenderPipeline>();
    pipe->addStage(std::make_unique<render::FastScaleThresholdStage>(p));
    if (pipe->build(srcW, srcH)) {
        pipeline_ = std::move(pipe);
        lastSrcW_ = srcW;
        lastSrcH_ = srcH;
    }
}

void PhysicalRenderer::renderFrame(OledFb& fb, int srcW, int srcH,
                                   const uint8_t* rgb565,
                                   const RenderRect& srcRect) {
    if (srcW <= 0 || srcH <= 0) {
        clear(fb);  // 源尺寸非法 -> 清屏
        return;
    }
    if (rgb565 == nullptr || srcRect.w <= 0 || srcRect.h <= 0) {
        return;  // no-op（错误路径无异常）
    }
    // 越界裁剪：srcRect 必须与 [0, srcW) x [0, srcH) 相交才有输出。
    // stage 内部逐像素做逻辑坐标裁剪（等价于原实现的交集裁剪 + 行主序
    // last-write-wins），这里只需拒绝「完全越界」的 rect（无交集时 stage
    // 自然不产生输出，行为一致：fb 保持原值）。
    if (srcRect.x + srcRect.w <= 0 || srcRect.y + srcRect.h <= 0 ||
        srcRect.x >= srcW || srcRect.y >= srcH) {
        return;  // 无交集
    }

    ensurePipeline(srcW, srcH);
    if (!pipeline_) {
        return;  // pipeline 构建失败（几何非法等）：no-op
    }

    render::StageInput in;
    in.format = render::RenderFormat::kRgb565;
    in.width = srcRect.w;
    in.height = srcRect.h;
    in.data = rgb565;
    in.bytes = static_cast<size_t>(srcRect.w) * static_cast<size_t>(srcRect.h) * 2u;
    in.offsetX = srcRect.x;
    in.offsetY = srcRect.y;

    render::StageOutput out;
    out.format = render::RenderFormat::kMono1;
    out.width = fbW_;
    out.height = fbH_;
    out.data = fb.data();
    out.capacityBytes = OledFb::kSizeBytes;

    (void)pipeline_->run(in, out);  // 失败 no-op（错误路径无异常；fb 保持原值）
}

}  // namespace oled
}  // namespace espview
