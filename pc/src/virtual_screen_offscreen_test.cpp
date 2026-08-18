// ESPView M8-B（B1/B2）：VirtualScreenWidget Qt offscreen 布局 / 动态分辨率测试。
//
// 运行要求：QT_QPA_PLATFORM=offscreen（测试内 qputenv 设置），不依赖真实显示器。
// 覆盖（任务书 §七 B1 + §八/§九 B2）：
//   - FULL 帧 320x240 → 240x320 动态分辨率重建（logicalSize 跟随帧分辨率）；
//   - PARTIAL 帧不重建 QImage（只更新目标 RECT，性能纪律）；
//   - resize 事件不重建 QImage（resize 只影响显示缩放，逻辑分辨率不变）；
//   - 16:9 窗口 letterbox：grab 渲染检查黑边外不画内容、内容区正确（不拉伸）。
// 注意：mapToFrame 的 letterbox 坐标语义由 shared CoordinateMapper host 测试
// 覆盖（input_mapper_test），本测试不重复。
//
// 本文件零协议依赖（不接触 Packet/CRC/CHUNKED/SEQ）；只消费 DisplayFrame。

#include <QApplication>
#include <QImage>
#include <QSize>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "i18n.h"
#include "virtual_screen_widget.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// 纯色 FULL 帧（RGB565 填充 fill565；0xFFFF=白）。
espview::pc::DisplayFrame makeFull(uint16_t w, uint16_t h, uint16_t frameId,
                                   uint16_t fill565 = 0xFFFFu) {
    espview::pc::DisplayFrame f;
    f.frameId = frameId;
    f.frameType = 0;  // FULL
    f.pixelFormat = 0;
    f.width = w;
    f.height = h;
    f.rectCount = 1;
    f.byteCount = static_cast<uint32_t>(w) * h * 2u;
    espview::pc::DisplayRect r;
    r.x = 0;
    r.y = 0;
    r.w = w;
    r.h = h;
    r.pixels.assign(static_cast<size_t>(w) * h * 2u, 0);
    for (size_t i = 0; i < r.pixels.size(); i += 2) {
        r.pixels[i] = static_cast<uint8_t>(fill565 & 0xFFu);
        r.pixels[i + 1] = static_cast<uint8_t>((fill565 >> 8) & 0xFFu);
    }
    f.rects.push_back(std::move(r));
    return f;
}

espview::pc::DisplayFrame makePartial(uint16_t w, uint16_t h, uint16_t frameId,
                                      uint16_t x, uint16_t y, uint16_t rw,
                                      uint16_t rh) {
    espview::pc::DisplayFrame f;
    f.frameId = frameId;
    f.frameType = 1;  // PARTIAL
    f.pixelFormat = 0;
    f.width = w;
    f.height = h;
    f.rectCount = 1;
    f.byteCount = static_cast<uint32_t>(rw) * rh * 2u;
    espview::pc::DisplayRect r;
    r.x = x;
    r.y = y;
    r.w = rw;
    r.h = rh;
    r.pixels.assign(static_cast<size_t>(rw) * rh * 2u, 0xFFu);  // 白色目标区
    f.rects.push_back(std::move(r));
    return f;
}

// B2：FULL 动态分辨率重建 + logicalSize 跟随。
void testDynamicResolution() {
    espview::pc::VirtualScreenWidget w;
    CHECK(w.logicalSize() == QSize(320, 240));  // 无帧 → 默认几何

    w.setFrame(makeFull(320, 240, 1));
    CHECK(w.hasImage());
    CHECK(w.logicalSize() == QSize(320, 240));

    w.setFrame(makeFull(240, 320, 2));  // 动态分辨率变化
    CHECK(w.logicalSize() == QSize(240, 320));
}

// B1/B2：PARTIAL 与 resize 均不重建 QImage（逻辑分辨率不变）。
void testPartialAndResizeKeepImage() {
    espview::pc::VirtualScreenWidget w;
    w.resize(640, 480);
    w.setFrame(makeFull(320, 240, 1));
    CHECK(w.logicalSize() == QSize(320, 240));

    w.setFrame(makePartial(320, 240, 2, 10, 10, 40, 40));
    CHECK(w.logicalSize() == QSize(320, 240));  // PARTIAL 不重建

    w.resize(1280, 720);  // 16:9 resize
    w.resize(1920, 1080);
    CHECK(w.logicalSize() == QSize(320, 240));  // resize 不改变逻辑分辨率
}

// B1：16:9 窗口 letterbox —— 内容区等比缩放居中，黑边外不画内容。
void testLetterboxPaint() {
    espview::pc::VirtualScreenWidget w;
    w.resize(1280, 720);
    w.show();
    QApplication::processEvents();
    w.setFrame(makeFull(320, 240, 1, 0xFFFFu));  // 白色帧
    QApplication::processEvents();

    const QImage img = w.grab().toImage();
    CHECK(!img.isNull());
    // 内容高 = 720（16:9 窗口，4:3 内容 → 宽 960 高 720，左右黑边 160）。
    // 左黑边内像素应为背景色（0x18,0x18,0x18）。
    const QRgb bg = qRgb(0x18, 0x18, 0x18);
    const QRgb edge = img.pixel(10, 360);
    CHECK(edge == bg);
    // 内容区中心应为白色（RGB565 0xFFFF → RGB888 白）。
    const QRgb center = img.pixel(640, 360);
    CHECK(center == qRgb(0xFF, 0xFF, 0xFF));
    // 顶部/底部不应被拉伸：内容区垂直全高（4:3 内容在 16:9 窗口高度方向无黑边）。
    const QRgb top = img.pixel(640, 4);
    CHECK(top == qRgb(0xFF, 0xFF, 0xFF));
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication app(argc, argv);

    std::printf("[virtual_screen_offscreen] tests\n");
    testDynamicResolution();          // B2
    testPartialAndResizeKeepImage();  // B1/B2
    testLetterboxPaint();             // B1
    std::printf("[virtual_screen_offscreen] checks: %d, failures: %d\n", g_checks,
                g_failures);
    return g_failures == 0 ? 0 : 1;
}