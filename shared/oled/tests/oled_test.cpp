// ESPView M7-A — OledFb / OLED 命令序列 Host Tests。
//
// 覆盖：fb 尺寸/clear/fill/pixel 边界/边框/棋盘格；文本渲染确定性
// （已知字形 → 已知位模式 + 像素计数）；SSD1306 init golden bytes（0xA0 无 remap）；
    // 上传数据按页反转列序（实机修正镜像）；
// SH1106 列偏移与页起始列命令；fb→wire 分段（控制字节/分段边界/空页）；
// 控制器类型枚举。纯 C++17，零平台依赖。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "oled_cmd.h"
#include "oled_fb.h"
#include "test_util.h"

namespace {

using espview::oled::ControllerType;
using espview::oled::OledFb;
using espview::oled::WireSegment;
using espview::oled::controllerName;
using espview::oled::glyphPixelCount;
using espview::oled::initCommands;
using espview::oled::kCtrlCommand;
using espview::oled::kCtrlData;
using espview::oled::kSh1106ColumnOffset;
using espview::oled::segmentCommands;
using espview::oled::segmentFrameUpload;
using espview::oled::setContrastCommands;
using espview::oled::displayOffCommands;
using espview::oled::displayOnCommands;

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

void testFbBasics() {
    CHECK_EQ(OledFb::kWidth, 128);
    CHECK_EQ(OledFb::kHeight, 64);
    CHECK_EQ(OledFb::kPageCount, 8);
    CHECK_EQ(OledFb::kSizeBytes, size_t(1024));

    OledFb fb;  // 构造即清零
    for (size_t i = 0; i < OledFb::kSizeBytes; ++i) {
        if (fb.data()[i] != 0) {
            CHECK_MSG(false, "constructor must clear framebuffer");
            break;
        }
    }

    CHECK(fb.setPixel(0, 0, true));
    CHECK(fb.getPixel(0, 0));
    CHECK_EQ(fb.byteAt(0, 0), uint8_t(0x01));
    CHECK(fb.setPixel(127, 63, true));
    CHECK_EQ(fb.byteAt(7, 127), uint8_t(0x80));
    CHECK(fb.setPixel(64, 8, true));
    CHECK_EQ(fb.byteAt(1, 64), uint8_t(0x01));

    // 越界：忽略并返回 false（不崩溃、不写入）。
    CHECK(!fb.setPixel(-1, 0, true));
    CHECK(!fb.setPixel(128, 0, true));
    CHECK(!fb.setPixel(0, -1, true));
    CHECK(!fb.setPixel(0, 64, true));
    CHECK(!fb.getPixel(-1, 0));
    CHECK(!fb.getPixel(128, 0));
    CHECK(!fb.getPixel(0, 64));
    CHECK_EQ(fb.byteAt(8, 0), uint8_t(0));
    CHECK_EQ(fb.byteAt(0, 128), uint8_t(0));

    fb.clear();
    for (size_t i = 0; i < OledFb::kSizeBytes; ++i) {
        if (fb.data()[i] != 0) {
            CHECK_MSG(false, "clear must zero framebuffer");
            break;
        }
    }

    fb.fill(true);
    for (size_t i = 0; i < OledFb::kSizeBytes; ++i) {
        if (fb.data()[i] != 0xFF) {
            CHECK_MSG(false, "fill(true) must set all bytes");
            break;
        }
    }
    fb.fill(false);
    CHECK_EQ(countSetBits(fb), size_t(0));

    // 单像素开关。
    CHECK(fb.setPixel(3, 5, true));
    CHECK(fb.getPixel(3, 5));
    CHECK(fb.setPixel(3, 5, false));
    CHECK(!fb.getPixel(3, 5));

    // setByte / page 访问器。
    fb.setByte(2, 10, 0xAA);
    CHECK_EQ(fb.byteAt(2, 10), uint8_t(0xAA));
    CHECK(fb.page(2) != nullptr);
    CHECK(fb.page(8) == nullptr);
    CHECK_EQ(fb.page(2)[10], uint8_t(0xAA));
}

void testBorderAndCheckerboard() {
    OledFb fb;
    fb.drawBorder();
    CHECK(fb.getPixel(0, 0));
    CHECK(fb.getPixel(127, 0));
    CHECK(fb.getPixel(0, 63));
    CHECK(fb.getPixel(127, 63));
    CHECK(fb.getPixel(64, 0));
    CHECK(fb.getPixel(0, 32));
    CHECK(!fb.getPixel(5, 5));
    CHECK(!fb.getPixel(64, 63 - 1));

    OledFb cb;
    cb.drawCheckerboard(8);
    CHECK(!cb.getPixel(0, 0));   // (0/8 + 0/8) & 1 == 0
    CHECK(cb.getPixel(8, 0));    // (1 + 0) & 1 == 1
    CHECK(cb.getPixel(0, 8));
    CHECK(!cb.getPixel(8, 8));
    CHECK(!cb.getPixel(127, 63));  // (15 + 7) & 1 == 0

    OledFb c4;
    c4.drawCheckerboard(4);
    CHECK(!c4.getPixel(0, 0));
    CHECK(c4.getPixel(4, 0));
    CHECK(c4.getPixel(0, 4));
    CHECK(!c4.getPixel(4, 4));

    OledFb bad;
    bad.drawCheckerboard(0);  // 非法 cell：无操作
    CHECK_EQ(countSetBits(bad), size_t(0));
}

// 独立于实现的行优先→页式列字节转置（测试自证 drawText 的约定实现）。
uint8_t transposeGlyphColumn(const uint8_t* glyph, int col) {
    uint8_t slice = 0;
    for (int r = 0; r < OledFb::kFontHeight; ++r) {
        if ((glyph[r] >> col) & 1u) {
            slice |= static_cast<uint8_t>(1u << r);
        }
    }
    return slice;
}

void testTextRendering() {
    // 'A' = {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}（行优先，bit0=最左）。
    const uint8_t kAGlyph[8] = {0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00};
    const uint8_t* aFromFont = OledFb::fontGlyph('A');
    CHECK(std::memcmp(aFromFont, kAGlyph, 8) == 0);

    // 手算 golden（转置后页式列字节，bit0=顶行）：
    //   col0=0x7C col1=0x7E col2=0x13 col3=0x13 col4=0x7E col5=0x7C col6=0x00 col7=0x00
    const uint8_t kExpected[8] = {0x7C, 0x7E, 0x13, 0x13, 0x7E, 0x7C, 0x00, 0x00};
    for (int col = 0; col < 8; ++col) {
        CHECK_EQ(transposeGlyphColumn(aFromFont, col), kExpected[col]);
    }

    OledFb fb;
    fb.drawText(0, 0, "A");
    for (int col = 0; col < 8; ++col) {
        CHECK_MSG(fb.byteAt(0, col) == kExpected[col],
                  "drawText('A') page0 column mismatch");
    }
    CHECK_EQ(fb.byteAt(0, 8), uint8_t(0));       // 后续列为空
    CHECK_EQ(countSetBits(fb), glyphPixelCount('A'));

    // 像素级确定性：'A' 顶部行 = 列 2,3（bit2/bit3 在行 0）。
    OledFb px;
    px.drawText(0, 0, "A");
    CHECK(px.getPixel(2, 0));
    CHECK(px.getPixel(3, 0));
    CHECK(!px.getPixel(0, 0));
    CHECK(px.getPixel(1, 1));  // 行 1 有 0x1E → 列 1,2,3,4
    CHECK(px.getPixel(2, 1));
    CHECK(px.getPixel(4, 1));

    // 纵向偏移：y=8 → 整字形落在 page1。
    OledFb off;
    off.drawText(0, 8, "A");
    for (int col = 0; col < 8; ++col) {
        CHECK_EQ(off.byteAt(1, col), kExpected[col]);
        CHECK_EQ(off.byteAt(0, col), uint8_t(0));
    }

    // 跨页偏移：y=60（topPage=7, rowOffset=4）：'!' col3 = 0x5F → 0xF0 落在
    // page7，溢出下半段（slice>>4）被截断（页面外）。
    // 可见行 = 字形顶部 4 行（0x18,0x3C,0x3C,0x18）→ 2+4+4+2 = 12 像素。
    OledFb lo;
    lo.drawText(0, 60, "!");
    CHECK_EQ(lo.byteAt(7, 3), uint8_t(0x5F << 4));
    CHECK_EQ(countSetBits(lo), size_t(12));

    // 横向截断：x=120 起画 "AB" → 'A' 的 8 列全部落在 120..127 屏内
    //（col0=0x7C(5位) col1=0x7E(6) col2=0x13(3) col3=0x13(3) col4=0x7E(6)
    //  col5=0x7C(5) col6=0x00(0) col7=0x00(0) → 共 28 位），'B' 完全在屏外。
    OledFb clip;
    clip.drawText(120, 0, "AB");
    CHECK_EQ(clip.byteAt(0, 120), kExpected[0]);
    CHECK_EQ(clip.byteAt(0, 125), kExpected[5]);
    CHECK_EQ(clip.byteAt(0, 127), kExpected[7]);
    CHECK_EQ(countSetBits(clip), size_t(28));

    // 多字符确定性：像素计数 = 各字形像素数之和。
    OledFb ab;
    ab.drawText(0, 0, "AB");
    CHECK_EQ(countSetBits(ab), glyphPixelCount('A') + glyphPixelCount('B'));

    // 空格/未知字符：不产生像素。
    OledFb sp;
    sp.drawText(0, 0, " ");
    CHECK_EQ(countSetBits(sp), size_t(0));
    const uint8_t* bad = OledFb::fontGlyph('\x01');
    CHECK(bad == OledFb::fontGlyph(' '));
    (void)bad;
}

void testInitSequences() {
    // SSD1306 init golden bytes（含 charge pump、horizontal memory mode；
    // 0xA0 = segment remap OFF，实机修正：0xA1 会水平镜像整屏）。
    const std::vector<uint8_t> kSsd1306Init = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA0, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x30, 0xA4, 0xA6, 0xAF};
    const std::vector<uint8_t> got1306 = initCommands(ControllerType::kSsd1306);
    CHECK_EQ(got1306.size(), kSsd1306Init.size());
    CHECK(got1306 == kSsd1306Init);

    // SH1106：无 charge pump（内部 DC-DC）、page 寻址（0x20/0x02）。
    const std::vector<uint8_t> kSh1106Init = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x20, 0x02, 0xA0, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x30, 0xA4, 0xA6, 0xAF};
    const std::vector<uint8_t> got1106 = initCommands(ControllerType::kSh1106);
    CHECK_EQ(got1106.size(), kSh1106Init.size());
    CHECK(got1106 == kSh1106Init);

    // 单命令序列。
    CHECK(displayOnCommands() == std::vector<uint8_t>({0xAF}));
    CHECK(displayOffCommands() == std::vector<uint8_t>({0xAE}));
    CHECK(setContrastCommands(0x7F) == std::vector<uint8_t>({0x81, 0x7F}));
    CHECK(setContrastCommands(0x40) == std::vector<uint8_t>({0x81, 0x40}));

    // AUTO 语义：默认 SSD1306（探测只解决地址，无回读无法区分控制器）。
    CHECK(initCommands(ControllerType::kAuto) ==
          initCommands(ControllerType::kSsd1306));
}

void testSegmentCommands() {
    const auto cmds = initCommands(ControllerType::kSsd1306);  // 25 bytes
    const auto segs = segmentCommands(cmds, 8);  // payload 7 → 4 段 (7,7,7,4)
    CHECK_EQ(segs.size(), size_t(4));
    CHECK_EQ(segs[0].size(), size_t(8));
    CHECK_EQ(segs[1].size(), size_t(8));
    CHECK_EQ(segs[2].size(), size_t(8));
    CHECK_EQ(segs[3].size(), size_t(5));
    for (const auto& s : segs) {
        CHECK_EQ(s[0], kCtrlCommand);  // 全部命令段
        CHECK(s.size() <= size_t(8));
    }
    // 字节顺序完整保留。
    size_t idx = 1;
    for (const auto& s : segs) {
        for (size_t i = 1; i < s.size(); ++i, ++idx) {
            CHECK_EQ(s[i], cmds[idx - 1]);
        }
    }
    CHECK_EQ(idx, cmds.size() + 1);

    // 空命令序列 → 空段列表。
    CHECK_EQ(segmentCommands({}, 8).size(), size_t(0));

    // maxSegmentBytes=1 极端值：仍产出段（控制字节 + 至少 1 载荷上限）。
    const auto tiny = segmentCommands(cmds, 1);
    CHECK(!tiny.empty());
    for (const auto& s : tiny) {
        CHECK(s.size() <= size_t(2));
        CHECK(s.size() >= size_t(2));
    }
}

void testFrameUploadSegments() {
    OledFb fb;
    fb.setPixel(0, 0, true);            // page0 col0 = 0x01
    fb.setPixel(1, 63, true);           // page7 col1 = 0x80
    fb.setPixel(100, 16, true);         // page2 col100 = 0x01

    // ---- SSD1306：范围命令 + 0x40 流式分块 ----
    const auto s1306 = segmentFrameUpload(fb, ControllerType::kSsd1306, 32);
    CHECK(!s1306.empty());
    CHECK(s1306[0] == std::vector<uint8_t>({kCtrlCommand, 0x21, 0x00, 0x7F,
                                            0x22, 0x00, 0x07}));
    for (size_t i = 1; i < s1306.size(); ++i) {
        CHECK_EQ(s1306[i][0], kCtrlData);
        CHECK(s1306[i].size() <= size_t(32));
    }
    // 1024 字节、每段 31 载荷 → 33 段全满 + 1 段 1 字节。
    CHECK_EQ(s1306.size(), size_t(1 + 34));
    CHECK_EQ(s1306[1].size(), size_t(32));
    const WireSegment& last = s1306.back();
    CHECK_EQ(last.size(), size_t(2));
    // 页内列序反转（实机修正）：流末字节 = 最后页(p7) 的 fb 列 0。
    CHECK_EQ(last[1], fb.byteAt(7, 0));
    // 数据字节完整性：pos → page=pos/128, col=pos%128 ← fb 列 (127-col)。
    size_t pos = 0;
    for (size_t i = 1; i < s1306.size(); ++i) {
        for (size_t j = 1; j < s1306[i].size(); ++j, ++pos) {
            const size_t page = pos / 128;
            const size_t col = pos % 128;
            CHECK_EQ(s1306[i][j], fb.byteAt(static_cast<int>(page),
                                            static_cast<int>(127 - col)));
        }
    }
    CHECK_EQ(pos, OledFb::kSizeBytes);

    // ---- SH1106：页起始列命令含偏移 2（0x02/0x10）+ 每页 128B 数据 ----
    const auto s1106 = segmentFrameUpload(fb, ControllerType::kSh1106, 1024);
    CHECK_EQ(s1106.size(), size_t(16));  // 8 页 × (1 cmd + 1 data)
    CHECK_EQ(kSh1106ColumnOffset, 2);
    for (int p = 0; p < 8; ++p) {
        const WireSegment& cmd = s1106[static_cast<size_t>(p) * 2];
        CHECK_EQ(cmd[0], kCtrlCommand);
        CHECK_EQ(cmd[1], uint8_t(0xB0 | p));
        CHECK_EQ(cmd[2], uint8_t(kSh1106ColumnOffset & 0x0F));      // 0x02
        CHECK_EQ(cmd[3], uint8_t(0x10 | ((kSh1106ColumnOffset >> 4) & 0x0F)));
        const WireSegment& data = s1106[static_cast<size_t>(p) * 2 + 1];
        CHECK_EQ(data[0], kCtrlData);
        CHECK_EQ(data.size(), size_t(1 + 128));
        CHECK_EQ(data[1], fb.byteAt(p, OledFb::kWidth - 1));  // 反转后页首=fb 列127
    }
    CHECK_EQ(s1106[0][2], uint8_t(0x02));
    CHECK_EQ(s1106[0][3], uint8_t(0x10));

    // ---- SH1106 小段：数据按 31B 拆分，命令段仍每页一个 ----
    const auto s1106s = segmentFrameUpload(fb, ControllerType::kSh1106, 32);
    // 每页 1 cmd + ceil(128/31)=5 段数据 → 8 × 6 = 48 段。
    CHECK_EQ(s1106s.size(), size_t(48));
    for (const auto& s : s1106s) {
        CHECK(s.size() <= size_t(32));
    }
    // 页 0 数据连续性（页内反转：位置 pos2 ← fb 列 127-pos2）。
    size_t pos2 = 0;
    for (size_t i = 1; i <= 5; ++i) {
        for (size_t j = 1; j < s1106s[i].size(); ++j, ++pos2) {
            CHECK_EQ(s1106s[i][j], fb.byteAt(0, static_cast<int>(127 - pos2)));
        }
    }
    CHECK_EQ(pos2, size_t(128));

    // ---- 空页：清空 fb 后仍产出全部数据段（内容全 0）----
    OledFb emptyFb;
    const auto e1306 = segmentFrameUpload(emptyFb, ControllerType::kSsd1306, 512);
    CHECK_EQ(e1306.size(), size_t(1 + 3));  // 范围 + 512,512,0 → 3 段数据
    for (size_t i = 1; i < e1306.size(); ++i) {
        CHECK_EQ(e1306[i][0], kCtrlData);
        for (size_t j = 1; j < e1306[i].size(); ++j) {
            if (e1306[i][j] != 0) {
                CHECK_MSG(false, "empty page must produce zero data bytes");
            }
        }
    }
    const auto e1106 = segmentFrameUpload(emptyFb, ControllerType::kSh1106, 1024);
    CHECK_EQ(e1106.size(), size_t(16));
    for (size_t i = 0; i < e1106.size(); i += 2) {
        CHECK_EQ(e1106[i + 1].size(), size_t(129));
    }
}

void testControllerEnum() {
    CHECK_EQ(static_cast<int>(ControllerType::kAuto), 0);
    CHECK_EQ(static_cast<int>(ControllerType::kSsd1306), 1);
    CHECK_EQ(static_cast<int>(ControllerType::kSh1106), 2);
    CHECK(std::strcmp(controllerName(ControllerType::kAuto), "AUTO") == 0);
    CHECK(std::strcmp(controllerName(ControllerType::kSsd1306), "SSD1306") == 0);
    CHECK(std::strcmp(controllerName(ControllerType::kSh1106), "SH1106") == 0);
}

}  // namespace

void runOledTests() {
    std::printf("[oled]\n");
    testFbBasics();
    testBorderAndCheckerboard();
    testTextRendering();
    testInitSequences();
    testSegmentCommands();
    testFrameUploadSegments();
    testControllerEnum();
}
