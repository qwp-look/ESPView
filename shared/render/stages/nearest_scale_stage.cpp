#include "nearest_scale_stage.h"

namespace espview {
namespace render {

StageResult NearestScaleStage::run(const StageInput& in, StageOutput& out) {
    if (in.data == nullptr || out.data == nullptr || in.width <= 0 ||
        in.height <= 0 || in.format != format_ || out.format != format_ ||
        out.width != outW_ || out.height != outH_ ||
        out.capacityBytes <
            formatBufferBytes(out.format, out.width, out.height)) {
        return StageResult{};
    }
    const size_t inStride = formatRowStrideBytes(format_, in.width);
    const size_t outStride = formatRowStrideBytes(format_, out.width);
    const uint8_t bpp = formatBitsPerPixel(format_);
    for (int oy = 0; oy < out.height; ++oy) {
        int sy = 0;
        if (out.height > 1) {
            sy = static_cast<int>((static_cast<int64_t>(oy) * in.height) / out.height);
        }
        if (sy >= in.height) {
            sy = in.height - 1;
        }
        const uint8_t* srcRow = in.data + static_cast<size_t>(sy) * inStride;
        uint8_t* dstRow = out.data + static_cast<size_t>(oy) * outStride;
        for (int ox = 0; ox < out.width; ++ox) {
            int sx = 0;
            if (out.width > 1) {
                sx = static_cast<int>((static_cast<int64_t>(ox) * in.width) / out.width);
            }
            if (sx >= in.width) {
                sx = in.width - 1;
            }
            const size_t srcIdx = static_cast<size_t>(sx) * (bpp / 8u);
            const size_t dstIdx = static_cast<size_t>(ox) * (bpp / 8u);
            for (uint8_t b = 0; b < bpp / 8u; ++b) {
                dstRow[dstIdx + b] = srcRow[srcIdx + b];
            }
        }
    }
    StageResult r;
    r.ok = true;
    r.bytesWritten = formatBufferBytes(out.format, out.width, out.height);
    return r;
}

}  // namespace render
}  // namespace espview
