#include "fast_scale_threshold_stage.h"

#include "render_color.h"

namespace espview {
namespace render {

bool FastScaleThresholdStage::outputSize(int inW, int inH, int& outW,
                                         int& outH) const {
    (void)inW;
    (void)inH;
    if (params_.logicalW <= 0 || params_.logicalH <= 0 ||
        params_.scaleNum <= 0 || params_.scaleDen <= 0 ||
        params_.targetW <= 0 || params_.targetH <= 0) {
        return false;
    }
    outW = params_.targetW;
    outH = params_.targetH;
    return true;
}

StageResult FastScaleThresholdStage::run(const StageInput& in,
                                         StageOutput& out) {
    if (in.data == nullptr || out.data == nullptr || in.width <= 0 ||
        in.height <= 0 || in.format != inputFormat() ||
        out.format != outputFormat() ||
        out.capacityBytes <
            formatBufferBytes(out.format, params_.targetW, params_.targetH)) {
        return StageResult{};
    }
    const int logicalW = params_.logicalW;
    const int logicalH = params_.logicalH;
    const int scaleNum = params_.scaleNum;
    const int scaleDen = params_.scaleDen;
    const int targetW = params_.targetW;
    const int targetH = params_.targetH;
    const uint8_t th = params_.threshold;

    // M8-A4：center crop 按逻辑帧高计算（不假设 240）。
    const int scaledH = (logicalH * scaleNum) / scaleDen;
    const int cropY = scaledH > targetH ? (scaledH - targetH) / 2 : 0;

    const size_t rowStride = static_cast<size_t>(in.width) * 2u;
    for (int sy = 0; sy < in.height; ++sy) {
        const int logicalY = in.offsetY + sy;
        if (logicalY < 0 || logicalY >= logicalH) {
            continue;
        }
        const int oy = (logicalY * scaleNum) / scaleDen - cropY;
        if (oy < 0 || oy >= targetH) {
            continue;
        }
        const uint8_t* srcRow = in.data + static_cast<size_t>(sy) * rowStride;
        const size_t outIdx = static_cast<size_t>((oy >> 3) * targetW);
        const uint8_t outMask = static_cast<uint8_t>(1u << (oy & 7));
        for (int sx = 0; sx < in.width; ++sx) {
            const int logicalX = in.offsetX + sx;
            if (logicalX < 0 || logicalX >= logicalW) {
                continue;
            }
            const int ox = (logicalX * scaleNum) / scaleDen;
            if (ox < 0 || ox >= targetW) {
                continue;
            }
            const uint16_t v =
                static_cast<uint16_t>(srcRow[sx * 2]) |
                static_cast<uint16_t>(
                    static_cast<uint16_t>(srcRow[sx * 2 + 1]) << 8);
            uint8_t& dst = out.data[outIdx + static_cast<size_t>(ox)];
            if (rgb565Thresholded(v, th) != 0) {
                dst |= outMask;
            } else {
                dst &= static_cast<uint8_t>(~outMask);
            }
        }
    }
    StageResult r;
    r.ok = true;
    r.bytesWritten = formatBufferBytes(out.format, targetW, targetH);
    return r;
}

}  // namespace render
}  // namespace espview
