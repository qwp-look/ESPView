#include "luminance_stage.h"

#include "render_color.h"

namespace espview {
namespace render {

StageResult LuminanceStage::run(const StageInput& in, StageOutput& out) {
    if (in.data == nullptr || out.data == nullptr || in.width <= 0 ||
        in.height <= 0 || in.format != inputFormat() ||
        out.format != outputFormat() || out.capacityBytes <
            formatBufferBytes(out.format, in.width, in.height)) {
        return StageResult{};
    }
    const size_t rowBytes = static_cast<size_t>(in.width) * 2u;
    const size_t outRowBytes = static_cast<size_t>(in.width);
    for (int y = 0; y < in.height; ++y) {
        const uint8_t* src = in.data + static_cast<size_t>(y) * rowBytes;
        uint8_t* dst = out.data + static_cast<size_t>(y) * outRowBytes;
        for (int x = 0; x < in.width; ++x) {
            const uint16_t v = static_cast<uint16_t>(src[x * 2]) |
                               static_cast<uint16_t>(
                                   static_cast<uint16_t>(src[x * 2 + 1]) << 8);
            dst[x] = rgb565Luminance(v);
        }
    }
    StageResult r;
    r.ok = true;
    r.bytesWritten = static_cast<size_t>(in.height) * outRowBytes;
    return r;
}

}  // namespace render
}  // namespace espview
