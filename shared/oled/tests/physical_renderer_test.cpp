// ESPView M7-C2 — PhysicalRenderer（RGB565 -> Mono1 页式 OledFb）Host Tests。
//
// 覆盖（任务 §21/§22）：
//   1. black frame -> fb 全 0；2. white frame -> fb 全 1（th=128）；
//   3. red/green/blue 单色帧确定性图案（luminance 锚点 R=76/G=150/B=29 @ th128）；
//   4. threshold 边界（th=0 / th=255 / 精确 Y 边界）；
//   5. crop：源 y<40 与 y>199 不产生像素；
//   6. aspect-preserving scale 映射点对点（水平 0..319->0..127、垂直 40..199->0..63）；
//   7. center crop 偏移（sy=40->oy=0、sy=199->oy=63）；
//   8. 边界像素（x=319、y=199 -> (127,63)）与越界裁剪；
//   9. 空 srcRect / w=0 / h=0 -> no-op；srcW/srcH<=0 -> clear；
//   10. 矩形增量：只更新交叠像素，其余 fb 字节不变；
//   11. 确定性：同输入两次渲染逐字节相等；
//   12. golden：全白=全 1、全黑=全 0、规则棋盘格 -> 手工构造 1KB 预期逐字节相等。
// 纯 C++17，零平台依赖；并入 shared/protocol host 套件。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "oled_fb.h"
#include "physical_renderer.h"
#include "test_util.h"

namespace {

using espview::oled::OledFb;
using espview::oled::PhysicalRenderer;
using espview::oled::RenderRect;

constexpr int kSrcW = 320;
constexpr int kSrcH = 240;
constexpr size_t kFrameBytes = static_cast<size_t>(kSrcW) * kSrcH * 2u;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kBlack = 0x0000;
constexpr int kCropY = 16;  // (240*2/5 - 64) / 2

// 测试侧的映射公式（DESIGN.md C2：ox = floor(sx*0.4), oy = floor(sy*0.4) - 16）。
constexpr int mapX(int sx) { return (sx * 2) / 5; }
constexpr int mapY(int sy) { return (sy * 2) / 5 - kCropY; }

std::vector<uint8_t> makeFrame(uint16_t fill) {
    std::vector<uint8_t> f(kFrameBytes, 0);
    for (size_t i = 0; i + 1 < f.size(); i += 2) {
        f[i] = static_cast<uint8_t>(fill & 0xFFu);
        f[i + 1] = static_cast<uint8_t>(fill >> 8);
    }
    return f;
}

void putPixel(std::vector<uint8_t>& f, int x, int y, uint16_t v) {
    const size_t off = static_cast<size_t>(y) * kSrcW * 2u + static_cast<size_t>(x) * 2u;
    f[off] = static_cast<uint8_t>(v & 0xFFu);
    f[off + 1] = static_cast<uint8_t>(v >> 8);
}

// M8-A4 紧凑缓冲契约：rgb565 = srcRect.w x srcRect.h 行主序（行步长 rect.w）。
// 从整帧抽取 rect 区域构造紧凑缓冲（与 LVGL flush_cb 的 band 缓冲同构）。
std::vector<uint8_t> makeRectBuffer(const std::vector<uint8_t>& frame,
                                    const espview::oled::RenderRect& r) {
    std::vector<uint8_t> buf(static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * 2u, 0);
    for (int dy = 0; dy < r.h; ++dy) {
        for (int dx = 0; dx < r.w; ++dx) {
            const int sx = r.x + dx;
            const int sy = r.y + dy;
            const size_t srcOff = (static_cast<size_t>(sy) * kSrcW + static_cast<size_t>(sx)) * 2u;
            const size_t dstOff = (static_cast<size_t>(dy) * static_cast<size_t>(r.w) + static_cast<size_t>(dx)) * 2u;
            buf[dstOff] = frame[srcOff];
            buf[dstOff + 1] = frame[srcOff + 1];
        }
    }
    return buf;
}

size_t countSetBits(const OledFb& fb) {
    size_t n = 0;
    for (size_t i = 0; i < OledFb::kSizeBytes; ++i) {
        uint8_t b = fb.data()[i];
        while (b != 0) {
            b &= static_cast<uint8_t>(b - 1);
            ++n;
        }
    }
    return n;
}

bool fbAllBytes(const OledFb& fb, uint8_t v) {
    for (size_t i = 0; i < OledFb::kSizeBytes; ++i) {
        if (fb.data()[i] != v) {
            return false;
        }
    }
    return true;
}

// ---- 1. 构造器 / 访问器 ----

void rendererBasics() {
    PhysicalRenderer def;
    CHECK_EQ(def.fbWidth(), 128);
    CHECK_EQ(def.fbHeight(), 64);
    CHECK_EQ(def.threshold(), uint8_t(128));

    PhysicalRenderer custom(96, 32, 200);
    CHECK_EQ(custom.fbWidth(), 96);
    CHECK_EQ(custom.fbHeight(), 32);
    CHECK_EQ(custom.threshold(), uint8_t(200));

    CHECK_EQ(PhysicalRenderer::kScaleNum, 2);
    CHECK_EQ(PhysicalRenderer::kScaleDen, 5);
}

// ---- 2. luminance / thresholded 工具 ----

void luminanceAndThresholdUnits() {
    CHECK_EQ(PhysicalRenderer::luminance(0xFFFF), uint8_t(255));  // 白
    CHECK_EQ(PhysicalRenderer::luminance(0x0000), uint8_t(0));    // 黑
    CHECK_EQ(PhysicalRenderer::luminance(0xF800), uint8_t(76));   // 红
    CHECK_EQ(PhysicalRenderer::luminance(0x07E0), uint8_t(150));  // 绿
    CHECK_EQ(PhysicalRenderer::luminance(0x001F), uint8_t(29));   // 蓝
    CHECK_EQ(PhysicalRenderer::luminance(0x7BEF), uint8_t(124));  // 暗灰（跨通道混合）

    CHECK_EQ(PhysicalRenderer::thresholded(0xFFFF, 128), uint8_t(1));
    CHECK_EQ(PhysicalRenderer::thresholded(0x0000, 128), uint8_t(0));
    CHECK_EQ(PhysicalRenderer::thresholded(0xF800, 128), uint8_t(0));
    CHECK_EQ(PhysicalRenderer::thresholded(0x07E0, 128), uint8_t(1));
    CHECK_EQ(PhysicalRenderer::thresholded(0x001F, 128), uint8_t(0));

    // 精确 Y 边界：Y=124 -> th=124 亮、th=125 灭。
    CHECK_EQ(PhysicalRenderer::thresholded(0x7BEF, 124), uint8_t(1));
    CHECK_EQ(PhysicalRenderer::thresholded(0x7BEF, 125), uint8_t(0));
}

// ---- 3/4. 整帧单色确定性图案与 threshold 边界 ----

void blackFrameAllZero() {
    PhysicalRenderer r;
    OledFb fb;
    const std::vector<uint8_t> frame = makeFrame(kBlack);
    r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
    CHECK(fbAllBytes(fb, 0x00));
    CHECK_EQ(countSetBits(fb), size_t(0));
}

void whiteFrameAllOne() {
    PhysicalRenderer r;
    OledFb fb;
    const std::vector<uint8_t> frame = makeFrame(kWhite);
    r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
    CHECK(fbAllBytes(fb, 0xFF));
    CHECK_EQ(countSetBits(fb), size_t(128 * 64));
}

void solidColorFrames() {
    PhysicalRenderer r;
    // 红 Y=76、蓝 Y=29 < 128 -> 全 0；绿 Y=150 >= 128 -> 全 1。
    {
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(0xF800);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
    }
    {
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(0x07E0);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0xFF));
    }
    {
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(0x001F);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
    }
    // 跨通道混合边界：0x8410（R5=16/G6=32/B5=16）Y=130 >= 128 -> 全 1；
    // 0x7BEF Y=124 < 128 -> 全 0。
    {
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(0x8410);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0xFF));
    }
    {
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(0x7BEF);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
    }
}

void thresholdBoundaries() {
    // th=0：任何像素（含黑）都亮。
    {
        PhysicalRenderer r(128, 64, 0);
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(kBlack);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0xFF));
    }
    // th=255：只有纯白（Y=255）亮；黑/红/绿全灭。
    {
        PhysicalRenderer r(128, 64, 255);
        {
            OledFb fb;
            const std::vector<uint8_t> frame = makeFrame(kBlack);
            r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
            CHECK(fbAllBytes(fb, 0x00));
        }
        {
            OledFb fb;
            const std::vector<uint8_t> frame = makeFrame(kWhite);
            r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
            CHECK(fbAllBytes(fb, 0xFF));
        }
        {
            OledFb fb;
            const std::vector<uint8_t> frame = makeFrame(0xF800);  // Y=76
            r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
            CHECK(fbAllBytes(fb, 0x00));
        }
        {
            OledFb fb;
            const std::vector<uint8_t> frame = makeFrame(0x07E0);  // Y=150
            r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
            CHECK(fbAllBytes(fb, 0x00));
        }
    }
    // 自定义阈值恰好等于亮度：红 th=76 -> 全 1；th=77 -> 全 0。
    {
        PhysicalRenderer r76(128, 64, 76);
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(0xF800);
        r76.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0xFF));
    }
    {
        PhysicalRenderer r77(128, 64, 77);
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(0xF800);
        r77.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
    }
}

// ---- 5. crop：源 y<40 与 y>199 不产生像素 ----

void cropTopBottomInvisible() {
    PhysicalRenderer r;
    // 只有 y∈[0,40) 的白带：全部映射到 oy<0，fb 全 0。
    {
        OledFb fb;
        std::vector<uint8_t> frame = makeFrame(kBlack);
        for (int y = 0; y < 40; ++y) {
            for (int x = 0; x < kSrcW; ++x) {
                putPixel(frame, x, y, kWhite);
            }
        }
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
    }
    // 只有 y∈[200,240) 的白带：全部映射到 oy>=64，fb 全 0。
    {
        OledFb fb;
        std::vector<uint8_t> frame = makeFrame(kBlack);
        for (int y = 200; y < kSrcH; ++y) {
            for (int x = 0; x < kSrcW; ++x) {
                putPixel(frame, x, y, kWhite);
            }
        }
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
    }
    // 可见带 y∈[40,199] 全白、其余全黑 -> fb 全 1（可见区恰好铺满 64 行）。
    {
        OledFb fb;
        std::vector<uint8_t> frame = makeFrame(kBlack);
        for (int y = 40; y <= 199; ++y) {
            for (int x = 0; x < kSrcW; ++x) {
                putPixel(frame, x, y, kWhite);
            }
        }
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0xFF));
    }
}

// ---- 6/7. aspect-preserving scale 与 center crop 点对点 ----

void scalePointToPointMath() {
    // 水平：所有 sx∈[0,320) 映射落在 0..127，且每个 oled 列都至少被覆盖。
    int minX = 1000, maxX = -1000;
    for (int sx = 0; sx < kSrcW; ++sx) {
        const int ox = mapX(sx);
        if (ox < minX) minX = ox;
        if (ox > maxX) maxX = ox;
    }
    CHECK_EQ(minX, 0);
    CHECK_EQ(maxX, 127);
    for (int ox = 0; ox < 128; ++ox) {
        bool covered = false;
        for (int sx = 0; sx < kSrcW; ++sx) {
            if (mapX(sx) == ox) {
                covered = true;
                break;
            }
        }
        CHECK(covered);
    }
    // 垂直：sy∈[40,199] 映射落在 0..63，且每个 oled 行都被覆盖。
    int minY = 1000, maxY = -1000;
    for (int sy = 40; sy <= 199; ++sy) {
        const int oy = mapY(sy);
        if (oy < minY) minY = oy;
        if (oy > maxY) maxY = oy;
    }
    CHECK_EQ(minY, 0);
    CHECK_EQ(maxY, 63);
    for (int oy = 0; oy < 64; ++oy) {
        bool covered = false;
        for (int sy = 40; sy <= 199; ++sy) {
            if (mapY(sy) == oy) {
                covered = true;
                break;
            }
        }
        CHECK(covered);
    }
    // 手工点对点边界（最近邻 3,2,3,2 分组）：
    CHECK_EQ(mapX(0), 0);
    CHECK_EQ(mapX(1), 0);
    CHECK_EQ(mapX(2), 0);
    CHECK_EQ(mapX(3), 1);
    CHECK_EQ(mapX(4), 1);
    CHECK_EQ(mapX(5), 2);
    CHECK_EQ(mapX(6), 2);
    CHECK_EQ(mapX(7), 2);
    CHECK_EQ(mapX(8), 3);
    CHECK_EQ(mapX(9), 3);
    CHECK_EQ(mapX(315), 126);
    CHECK_EQ(mapX(316), 126);
    CHECK_EQ(mapX(317), 126);
    CHECK_EQ(mapX(318), 127);
    CHECK_EQ(mapX(319), 127);
    // center crop：39 -> -1（不可见）、40 -> 0、43/44 -> 1、199 -> 63、200 -> 64（不可见）。
    CHECK_EQ(mapY(39), -1);
    CHECK_EQ(mapY(40), 0);
    CHECK_EQ(mapY(41), 0);
    CHECK_EQ(mapY(42), 0);
    CHECK_EQ(mapY(43), 1);
    CHECK_EQ(mapY(44), 1);
    CHECK_EQ(mapY(45), 2);
    CHECK_EQ(mapY(197), 62);
    CHECK_EQ(mapY(198), 63);
    CHECK_EQ(mapY(199), 63);
    CHECK_EQ(mapY(200), 64);
}

// 渲染 1x1 白色源像素：fb 必须恰好 1 个亮像素且位于映射坐标。
void renderAndCheckSinglePixel(PhysicalRenderer& r, std::vector<uint8_t>& frame,
                               int sx, int sy) {
    putPixel(frame, sx, sy, kWhite);
    OledFb fb;
    const std::vector<uint8_t> one = makeRectBuffer(frame, RenderRect{sx, sy, 1, 1});
    r.renderFrame(fb, kSrcW, kSrcH, one.data(), RenderRect{sx, sy, 1, 1});
    putPixel(frame, sx, sy, kBlack);
    CHECK_EQ(countSetBits(fb), size_t(1));
    CHECK(fb.getPixel(mapX(sx), mapY(sy)));
}

void scalePointToPointRender() {
    PhysicalRenderer r;
    std::vector<uint8_t> frame = makeFrame(kBlack);
    // 水平全扫（sy=40 顶行 / 120 中行 / 199 底行）。
    for (int sy : {40, 120, 199}) {
        for (int sx = 0; sx < kSrcW; ++sx) {
            renderAndCheckSinglePixel(r, frame, sx, sy);
        }
    }
    // 垂直全扫（sx=0 左列 / 160 中列 / 319 右列）。
    for (int sx : {0, 160, 319}) {
        for (int sy = 40; sy <= 199; ++sy) {
            renderAndCheckSinglePixel(r, frame, sx, sy);
        }
    }
}

// ---- 8. 边界像素与越界裁剪 ----

void boundaryPixels() {
    PhysicalRenderer r;
    std::vector<uint8_t> frame = makeFrame(kBlack);
    putPixel(frame, 319, 199, kWhite);
    {
        OledFb fb;
        const std::vector<uint8_t> one = makeRectBuffer(frame, RenderRect{319, 199, 1, 1});
        r.renderFrame(fb, kSrcW, kSrcH, one.data(), RenderRect{319, 199, 1, 1});
        CHECK_EQ(countSetBits(fb), size_t(1));
        CHECK(fb.getPixel(127, 63));
    }
    putPixel(frame, 319, 199, kBlack);

    // 四角映射：左上 (0,0)、右上 (127,0)、左下 (0,63)、右下 (127,63)。
    const struct { int sx, sy, ox, oy; } corners[] = {
        {0, 40, 0, 0}, {319, 40, 127, 0}, {0, 199, 0, 63}, {319, 199, 127, 63},
    };
    for (const auto& c : corners) {
        putPixel(frame, c.sx, c.sy, kWhite);
        OledFb fb;
        const std::vector<uint8_t> one = makeRectBuffer(frame, RenderRect{c.sx, c.sy, 1, 1});
        r.renderFrame(fb, kSrcW, kSrcH, one.data(), RenderRect{c.sx, c.sy, 1, 1});
        putPixel(frame, c.sx, c.sy, kBlack);
        CHECK_EQ(countSetBits(fb), size_t(1));
        CHECK(fb.getPixel(c.ox, c.oy));
    }

    // 越界矩形：完全在源外 -> no-op。
    {
        OledFb fb;
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{320, 0, 1, 1});
        CHECK(fbAllBytes(fb, 0x00));
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{-1, 40, 1, 1});
        CHECK(fbAllBytes(fb, 0x00));
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 240, 320, 1});
        CHECK(fbAllBytes(fb, 0x00));
    }
    // 右下角 2x2 越界：只渲染 (319,199) 白 -> (127,63)；(319,200) 映射到 oy=64 被裁剪。
    {
        putPixel(frame, 319, 199, kWhite);
        OledFb fb;
        const std::vector<uint8_t> two = makeRectBuffer(frame, RenderRect{319, 199, 2, 2});
        r.renderFrame(fb, kSrcW, kSrcH, two.data(), RenderRect{319, 199, 2, 2});
        putPixel(frame, 319, 199, kBlack);
        CHECK_EQ(countSetBits(fb), size_t(1));
        CHECK(fb.getPixel(127, 63));
    }
    // 部分越界到左侧：rect {-5,40,10,10} 与源交集 sx∈[0,5)、sy∈[40,50)
    // -> oled 区域 ox∈{0,1} x oy∈{0,1,2,3} 共 8 像素（白色源）。
    {
        std::vector<uint8_t> whiteFrame = makeFrame(kBlack);
        for (int y = 40; y < 50; ++y) {
            for (int x = 0; x < 5; ++x) {
                putPixel(whiteFrame, x, y, kWhite);
            }
        }
        OledFb fb;
        const std::vector<uint8_t> ten = makeRectBuffer(whiteFrame, RenderRect{-5, 40, 10, 10});
        r.renderFrame(fb, kSrcW, kSrcH, ten.data(), RenderRect{-5, 40, 10, 10});
        CHECK_EQ(countSetBits(fb), size_t(8));
        for (int ox = 0; ox <= 1; ++ox) {
            for (int oy = 0; oy <= 3; ++oy) {
                CHECK(fb.getPixel(ox, oy));
            }
        }
    }
}

// ---- 9. 空 / 非法 rect 与非法源尺寸 ----

void emptyAndInvalidRects() {
    PhysicalRenderer r;
    std::vector<uint8_t> frame = makeFrame(kWhite);

    // w=0 / h=0 / 负尺寸 / 完全在源外 -> no-op，fb 不变。
    {
        OledFb fb;
        fb.fill(true);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 40, 0, 160});
        CHECK(fbAllBytes(fb, 0xFF));
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 40, 160, 0});
        CHECK(fbAllBytes(fb, 0xFF));
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, -5, -5});
        CHECK(fbAllBytes(fb, 0xFF));
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{320, 240, 10, 10});
        CHECK(fbAllBytes(fb, 0xFF));
    }
    // 空指针 -> no-op（错误路径无异常）。
    {
        OledFb fb;
        fb.fill(true);
        r.renderFrame(fb, kSrcW, kSrcH, nullptr, RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0xFF));
    }
    // srcW/srcH <= 0 -> clear（全 0）。
    {
        OledFb fb;
        fb.fill(true);
        r.renderFrame(fb, 0, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
        fb.fill(true);
        r.renderFrame(fb, kSrcW, -1, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        CHECK(fbAllBytes(fb, 0x00));
    }
}

// ---- 10. 矩形增量：只更新交叠像素 ----

void rectIncremental() {
    PhysicalRenderer r;

    // 黑矩形覆盖左半源区（x∈[0,160) -> ox∈[0,64)、y∈[40,200) -> oy∈[0,64)）：
    // 左 64 列清 0，右 64 列保持原 0xFF。
    {
        OledFb fb;
        fb.fill(true);
        const std::vector<uint8_t> black = makeFrame(kBlack);
        const std::vector<uint8_t> blackRect = makeRectBuffer(black, RenderRect{0, 40, 160, 160});
        r.renderFrame(fb, kSrcW, kSrcH, blackRect.data(), RenderRect{0, 40, 160, 160});
        for (int page = 0; page < OledFb::kPageCount; ++page) {
            for (int x = 0; x < 64; ++x) {
                CHECK_EQ(fb.byteAt(page, x), uint8_t(0x00));
            }
            for (int x = 64; x < OledFb::kWidth; ++x) {
                CHECK_EQ(fb.byteAt(page, x), uint8_t(0xFF));
            }
        }
    }
    // 字节级保真：随机 fb 图案 + 黑矩形 -> 交叠区全 0、非交叠区逐字节不变。
    {
        OledFb fb;
        for (size_t i = 0; i < OledFb::kSizeBytes; ++i) {
            fb.data()[i] = static_cast<uint8_t>(static_cast<size_t>(i) * 37u + 11u);
        }
        uint8_t snapshot[OledFb::kSizeBytes];
        std::memcpy(snapshot, fb.data(), OledFb::kSizeBytes);
        const std::vector<uint8_t> black = makeFrame(kBlack);
        const std::vector<uint8_t> blackRect = makeRectBuffer(black, RenderRect{0, 40, 160, 160});
        r.renderFrame(fb, kSrcW, kSrcH, blackRect.data(), RenderRect{0, 40, 160, 160});
        for (int page = 0; page < OledFb::kPageCount; ++page) {
            for (int x = 0; x < 64; ++x) {
                CHECK_EQ(fb.byteAt(page, x), uint8_t(0x00));
            }
            for (int x = 64; x < OledFb::kWidth; ++x) {
                CHECK_EQ(fb.byteAt(page, x), snapshot[page * OledFb::kWidth + x]);
            }
        }
    }
    // 分组坍缩与增量覆盖：3x3 源像素 -> 2x2 oled 像素（sx0..2->ox0、sx3..5->ox1/2；
    // sy40..42->oy0、sy43..45->oy1/2），分块验证只更新交叠像素。
    {
        OledFb fb;
        std::vector<uint8_t> white = makeFrame(kBlack);
        for (int dy = 0; dy < 3; ++dy) {
            for (int dx = 0; dx < 3; ++dx) {
                putPixel(white, 0 + dx, 40 + dy, kWhite);
            }
        }
        const std::vector<uint8_t> w0 = makeRectBuffer(white, RenderRect{0, 40, 3, 3});
        r.renderFrame(fb, kSrcW, kSrcH, w0.data(), RenderRect{0, 40, 3, 3});
        CHECK_EQ(countSetBits(fb), size_t(1));
        CHECK(fb.getPixel(0, 0));
        // 第二块 {3,43,3,3}：sx 3/4/5 -> ox 1/1/2，sy 43/44/45 -> oy 1/1/2
        // -> 新增 (1,1)、(1,2)、(2,1)、(2,2)，(0,0) 保持。
        for (int dy = 0; dy < 3; ++dy) {
            for (int dx = 0; dx < 3; ++dx) {
                putPixel(white, 3 + dx, 43 + dy, kWhite);
            }
        }
        const std::vector<uint8_t> w1 = makeRectBuffer(white, RenderRect{3, 43, 3, 3});
        r.renderFrame(fb, kSrcW, kSrcH, w1.data(), RenderRect{3, 43, 3, 3});
        CHECK(fb.getPixel(1, 1));
        CHECK(fb.getPixel(1, 2));
        CHECK(fb.getPixel(2, 1));
        CHECK(fb.getPixel(2, 2));
        CHECK(fb.getPixel(0, 0));
        CHECK_EQ(countSetBits(fb), size_t(5));
        // 黑矩形覆盖 {0,40,3,3}：只清除 (0,0)，其余不变。
        const std::vector<uint8_t> black3 =
            makeRectBuffer(makeFrame(kBlack), RenderRect{0, 40, 3, 3});
        r.renderFrame(fb, kSrcW, kSrcH, black3.data(), RenderRect{0, 40, 3, 3});
        CHECK(!fb.getPixel(0, 0));
        CHECK(fb.getPixel(1, 1));
        CHECK(fb.getPixel(1, 2));
        CHECK(fb.getPixel(2, 1));
        CHECK(fb.getPixel(2, 2));
        CHECK_EQ(countSetBits(fb), size_t(4));
    }
}

// ---- M8-A4 回归：LVGL band 紧凑缓冲（OOB 越界读修复验证）----
// LVGL flush_cb 只持有 band 相对缓冲（320x24），rect.y > 0。旧实现按整帧绝对
// 坐标索引（sy*srcW + sx）会越界读（可达 ~138KB）；新契约按 rect 相对索引
// ((sy-rect.y)*rect.w + (sx-rect.x))。本测试喂 320x24 紧凑 band（y=24..47），
// 期望只读 band 内像素；可见区（sy>=40 -> oy>=0）映射正确。
void bandBufferCompactContract() {
    PhysicalRenderer r;
    constexpr int kBandW = 320;
    constexpr int kBandH = 24;
    // band = 全黑 320x24；在可见行（sy=40..47）置白（对应 oled oy=0..3）。
    std::vector<uint8_t> band(static_cast<size_t>(kBandW) * kBandH * 2u, 0);
    const auto putBand = [&](int lx, int ly, uint16_t v) {
        const size_t off = static_cast<size_t>(ly) * kBandW * 2u + static_cast<size_t>(lx) * 2u;
        band[off] = static_cast<uint8_t>(v & 0xFFu);
        band[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    for (int ly = 40 - 24; ly < 48 - 24; ++ly) {  // band 局部行 0..23
        for (int lx = 0; lx < kBandW; ++lx) {
            putBand(lx, ly, kWhite);
        }
    }
    OledFb fb;
    // rect = 全帧逻辑坐标 {0, 24, 320, 24}；rgb565 为该 band 的紧凑缓冲。
    r.renderFrame(fb, kSrcW, kSrcH, band.data(), RenderRect{0, 24, kBandW, kBandH});
    // 可见源行 sy=40..47 -> oy=0..2（floor(sy*0.4)-16）全白；sy=24..39 -> oy<0 被跳过。
    CHECK_EQ(countSetBits(fb), size_t(128 * 3));
    for (int oy = 0; oy <= 2; ++oy) {
        for (int ox = 0; ox < 128; ++ox) {
            CHECK(fb.getPixel(ox, oy));
        }
    }
    for (int oy = 3; oy < 64; ++oy) {
        for (int ox = 0; ox < 128; ++ox) {
            CHECK(!fb.getPixel(ox, oy));
        }
    }

    // band 中部 band（y=96..119，含可见行 96..119 -> oy 16..31）：全白。
    OledFb fb2;
    std::vector<uint8_t> band2(static_cast<size_t>(kBandW) * kBandH * 2u, 0);
    const auto putBand2 = [&](int lx, int ly, uint16_t v) {
        const size_t off = static_cast<size_t>(ly) * kBandW * 2u + static_cast<size_t>(lx) * 2u;
        band2[off] = static_cast<uint8_t>(v & 0xFFu);
        band2[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    for (int ly = 0; ly < kBandH; ++ly) {
        for (int lx = 0; lx < kBandW; ++lx) {
            putBand2(lx, ly, kWhite);
        }
    }
    r.renderFrame(fb2, kSrcW, kSrcH, band2.data(), RenderRect{0, 96, kBandW, kBandH});
    // sy=96..119 -> oy = floor(sy*0.4)-16 = 22..31（10 行）。
    CHECK_EQ(countSetBits(fb2), size_t(128 * 10));
    for (int oy = 22; oy <= 31; ++oy) {
        for (int ox = 0; ox < 128; ++ox) {
            CHECK(fb2.getPixel(ox, oy));
        }
    }
    for (int oy = 0; oy < 22; ++oy) {
        for (int ox = 0; ox < 128; ++ox) {
            CHECK(!fb2.getPixel(ox, oy));
        }
    }
}

// ---- 11. 确定性 ----

void determinism() {
    // 确定性伪随机帧（LCG，无平台随机性）。
    std::vector<uint8_t> frame(kFrameBytes, 0);
    uint32_t state = 0x12345678u;
    for (size_t i = 0; i + 1 < frame.size(); i += 2) {
        state = state * 1664525u + 1013904223u;
        const uint16_t v = static_cast<uint16_t>(state >> 16);
        frame[i] = static_cast<uint8_t>(v & 0xFFu);
        frame[i + 1] = static_cast<uint8_t>(v >> 8);
    }

    PhysicalRenderer r;
    OledFb a, b, c;
    r.renderFrame(a, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
    r.renderFrame(b, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
    CHECK(std::memcmp(a.data(), b.data(), OledFb::kSizeBytes) == 0);

    // 同 fb 二次渲染仍逐字节相等（幂等）。
    std::memcpy(c.data(), a.data(), OledFb::kSizeBytes);
    r.renderFrame(c, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
    CHECK(std::memcmp(a.data(), c.data(), OledFb::kSizeBytes) == 0);

    // 部分矩形渲染的确定性。
    OledFb d, e;
    const std::vector<uint8_t> part = makeRectBuffer(frame, RenderRect{37, 61, 200, 150});
    r.renderFrame(d, kSrcW, kSrcH, part.data(), RenderRect{37, 61, 200, 150});
    r.renderFrame(e, kSrcW, kSrcH, part.data(), RenderRect{37, 61, 200, 150});
    CHECK(std::memcmp(d.data(), e.data(), OledFb::kSizeBytes) == 0);

    // 两个参数相同的实例输出一致。
    PhysicalRenderer r2;
    OledFb f;
    r2.renderFrame(f, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
    CHECK(std::memcmp(a.data(), f.data(), OledFb::kSizeBytes) == 0);
}

// ---- 12. golden：手工构造 1KB 预期，逐字节比较 ----

void goldenCheckerboard() {
    // 源棋盘格：亮 iff ((sx/5) + (sy/5)) % 2 == 0（5 像素一亮暗交替）。
    std::vector<uint8_t> frame = makeFrame(kBlack);
    for (int sy = 0; sy < kSrcH; ++sy) {
        for (int sx = 0; sx < kSrcW; ++sx) {
            if (((sx / 5) + (sy / 5)) % 2 == 0) {
                putPixel(frame, sx, sy, kWhite);
            }
        }
    }
    // 手工构造预期：每组 5x5 源像素 -> 2x2 oled 像素，且组内值一致
    // （col 组 = floor(ox/2)、row 组 = floor(oy/2)，亮 iff 两组同奇偶）。
    uint8_t expected[OledFb::kSizeBytes];
    for (int page = 0; page < OledFb::kPageCount; ++page) {
        for (int x = 0; x < OledFb::kWidth; ++x) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int oy = page * 8 + bit;
                if (((x / 2) + (oy / 2)) % 2 == 0) {
                    byte |= static_cast<uint8_t>(1u << bit);
                }
            }
            expected[page * OledFb::kWidth + x] = byte;
        }
    }

    PhysicalRenderer r;
    OledFb fb;
    r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
    CHECK(std::memcmp(fb.data(), expected, OledFb::kSizeBytes) == 0);
    CHECK_EQ(countSetBits(fb), size_t(4096));  // 8192/2 亮像素
}

void goldenWhiteBlack() {
    PhysicalRenderer r;
    {
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(kWhite);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        uint8_t expected[OledFb::kSizeBytes];
        std::memset(expected, 0xFF, OledFb::kSizeBytes);
        CHECK(std::memcmp(fb.data(), expected, OledFb::kSizeBytes) == 0);
    }
    {
        OledFb fb;
        const std::vector<uint8_t> frame = makeFrame(kBlack);
        r.renderFrame(fb, kSrcW, kSrcH, frame.data(), RenderRect{0, 0, kSrcW, kSrcH});
        uint8_t expected[OledFb::kSizeBytes];
        std::memset(expected, 0x00, OledFb::kSizeBytes);
        CHECK(std::memcmp(fb.data(), expected, OledFb::kSizeBytes) == 0);
    }
}

}  // namespace

void runPhysicalRendererTests() {
    std::printf("[physical_renderer]\n");
    rendererBasics();
    luminanceAndThresholdUnits();
    blackFrameAllZero();
    whiteFrameAllOne();
    solidColorFrames();
    thresholdBoundaries();
    cropTopBottomInvisible();
    scalePointToPointMath();
    scalePointToPointRender();
    boundaryPixels();
    emptyAndInvalidRects();
    rectIncremental();
    bandBufferCompactContract();
    determinism();
    goldenCheckerboard();
    goldenWhiteBlack();
}

