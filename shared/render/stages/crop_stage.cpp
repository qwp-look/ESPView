#include "crop_stage.h"

namespace espview {
namespace render {

StageResult CropStage::run(const StageInput& in, StageOutput& out) {
    if (in.data == nullptr || out.data == nullptr || in.format != format_ ||
        out.format != format_ || out.width != w_ || out.height != h_ ||
        out.capacityBytes < formatBufferBytes(out.format, out.width, out.height)) {
        return StageResult{};
    }
    const size_t inStride = formatRowStrideBytes(format_, in.width);
    const size_t outStride = formatRowStrideBytes(format_, out.width);
    const size_t bpp = formatBitsPerPixel(format_);
    for (int oy = 0; oy < out.height; ++oy) {
        const uint8_t* src =
            in.data + static_cast<size_t>(y_ + oy) * inStride +
            static_cast<size_t>(x_) * (bpp / 8u);
        uint8_t* dst = out.data + static_cast<size_t>(oy) * outStride;
        for (int ox = 0; ox < out.width; ++ox) {
            for (size_t b = 0; b < bpp / 8u; ++b) {
                dst[static_cast<size_t>(ox) * (bpp / 8u) + b] =
                    src[static_cast<size_t>(ox) * (bpp / 8u) + b];
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
