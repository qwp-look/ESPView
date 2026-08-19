#include "bilinear_scale_stage.h"

#include <algorithm>

namespace espview {
namespace render {

StageResult BilinearScaleStage::run(const StageInput& in, StageOutput& out) {
    if (in.data == nullptr || out.data == nullptr || in.width <= 0 ||
        in.height <= 0 || in.format != inputFormat() ||
        out.format != outputFormat() || out.width != outW_ ||
        out.height != outH_ ||
        out.capacityBytes <
            formatBufferBytes(out.format, out.width, out.height)) {
        return StageResult{};
    }
    const int inW = in.width;
    const int inH = in.height;
    const int outW = out.width;
    const int outH = out.height;
    for (int oy = 0; oy < outH; ++oy) {
        int vy = 0;
        int wy = 0;
        if (inH > 1 && outH > 1) {
            const int v = (oy * (inH - 1) * 256) / (outH - 1);
            vy = v >> 8;
            wy = v & 0xFF;
        }
        if (vy >= inH - 1) {
            vy = inH - 1;
            wy = 0;
        }
        const int vy1 = std::min(vy + 1, inH - 1);
        const uint8_t* r0 = in.data + static_cast<size_t>(vy) * inW;
        const uint8_t* r1 = in.data + static_cast<size_t>(vy1) * inW;
        for (int ox = 0; ox < outW; ++ox) {
            int ux = 0;
            int wx = 0;
            if (inW > 1 && outW > 1) {
                const int u = (ox * (inW - 1) * 256) / (outW - 1);
                ux = u >> 8;
                wx = u & 0xFF;
            }
            if (ux >= inW - 1) {
                ux = inW - 1;
                wx = 0;
            }
            const int ux1 = std::min(ux + 1, inW - 1);
            const uint32_t a = r0[ux];
            const uint32_t b = r0[ux1];
            const uint32_t c = r1[ux];
            const uint32_t d = r1[ux1];
            const uint32_t top = a * (256u - wx) + b * wx;
            const uint32_t bot = c * (256u - wx) + d * wx;
            const uint32_t v = (top * (256u - wy) + bot * wy + 32768u) >> 16;
            out.data[static_cast<size_t>(oy) * outW + ox] =
                static_cast<uint8_t>(v > 255u ? 255u : v);
        }
    }
    StageResult r;
    r.ok = true;
    r.bytesWritten = static_cast<size_t>(outH) * outW;
    return r;
}

}  // namespace render
}  // namespace espview
