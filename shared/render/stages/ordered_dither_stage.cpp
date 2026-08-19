#include "ordered_dither_stage.h"

namespace espview {
namespace render {

StageResult OrderedDitherStage::run(const StageInput& in, StageOutput& out) {
    if (in.data == nullptr || out.data == nullptr || in.width <= 0 ||
        in.height <= 0 || in.format != inputFormat() ||
        out.format != outputFormat() ||
        out.capacityBytes < formatBufferBytes(out.format, in.width, in.height)) {
        return StageResult{};
    }
    for (int y = 0; y < in.height; ++y) {
        const uint8_t* src = in.data + static_cast<size_t>(y) * in.width;
        const int page = y >> 3;
        const uint8_t bit = static_cast<uint8_t>(1u << (y & 7));
        uint8_t* dst = out.data + static_cast<size_t>(page) * in.width;
        for (int x = 0; x < in.width; ++x) {
            const uint8_t lv = static_cast<uint8_t>(
                (static_cast<unsigned>(src[x]) * 16u + 127u) / 255u);
            if (lv > bayerAt(x, y)) {
                dst[x] |= bit;
            } else {
                dst[x] &= static_cast<uint8_t>(~bit);
            }
        }
    }
    StageResult r;
    r.ok = true;
    r.bytesWritten = formatBufferBytes(out.format, in.width, in.height);
    return r;
}

}  // namespace render
}  // namespace espview
