// ESPView M7-A — OledFb：128x64 1bpp 页式 framebuffer（8 pages × 128B = 1KB）。
//
// 纯 C++17、零平台依赖（host 测试与 ESP32 侧共用同一份源码）。
// 布局约定（匹配 SSD1306/SH1106 GDDRAM page mode）：
//   fb[page * 128 + x] = 该列 8 个垂直像素，bit0 = 该页顶行（y = page*8）。
// 内置紧凑 ASCII 8x8 字体（自包含，无外部资源）：行优先存储（bit0 = 最左
// 像素、行从上到下），drawText 绘制时转置为页式列字节。
#pragma once

#include <cstddef>
#include <cstdint>

namespace espview {
namespace oled {

class OledFb {
public:
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;
    static constexpr int kPageCount = 8;          // kHeight / 8
    static constexpr size_t kSizeBytes = 1024;    // kPageCount * kWidth

    OledFb();
    void clear();
    void fill(bool on);

    // 越界像素忽略并返回 false；合法像素写入并返回 true。
    bool setPixel(int x, int y, bool on);
    bool getPixel(int x, int y) const;

    void drawBorder();                   // 1px 边框（四周最外圈）
    void drawCheckerboard(int cell = 8); // 默认 8x8 棋盘格

    // 8x8 紧凑字体；x/y 为像素坐标，超出 128x64 的部分自动截断。
    void drawText(int x, int y, const char* text);

    // 页缓冲访问器。
    const uint8_t* data() const { return fb_; }
    uint8_t* data() { return fb_; }
    const uint8_t* page(int pageIndex) const;   // 越界返回 nullptr
    uint8_t byteAt(int pageIndex, int x) const;
    void setByte(int pageIndex, int x, uint8_t v);

    // 字体访问器（host 测试/诊断用）：ASCII 0x20..0x7E → 8 bytes 行优先。
    // 未知/不可见字符回退到空格字形。
    static const uint8_t* fontGlyph(char c);
    static constexpr int kFontWidth = 8;
    static constexpr int kFontHeight = 8;

private:
    uint8_t fb_[kSizeBytes];
};

// 单字符像素计数（fontGlyph 的 popcount；host 测试用）。
size_t glyphPixelCount(char c);

}  // namespace oled
}  // namespace espview
