// ESPView M2 — VirtualScreenWidget 实现（见 virtual_screen_widget.h）。

#include "virtual_screen_widget.h"

#include <QDebug>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWheelEvent>

#include "coordinate_mapper.h"
#include "input_controller.h"
#include "qt_key_adapter.h"

namespace espview {
namespace pc {

namespace {

// RGB565（LE 两字节）→ RGB888。位域复制展开（标准做法，非线性缩放）。
//   R5: 0..31 → 0..255；G6: 0..63 → 0..255；B5: 0..31 → 0..255
inline void rgb565ToRgb888(uint16_t v, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint8_t r5 = static_cast<uint8_t>((v >> 11) & 0x1Fu);
    const uint8_t g6 = static_cast<uint8_t>((v >> 5) & 0x3Fu);
    const uint8_t b5 = static_cast<uint8_t>(v & 0x1Fu);
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

}  // namespace

VirtualScreenWidget::VirtualScreenWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(320, 240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);  // M3：无按键按住也要收到 mouseMoveEvent
}

void VirtualScreenWidget::setFrame(const DisplayFrame& frame) {
    // v0.1 仅 RGB565；其它格式（未来）不绘制。
    if (frame.pixelFormat != 0 || frame.width == 0 || frame.height == 0 ||
        frame.width > 4096 || frame.height > 4096) {
        return;
    }
    if (frame.frameType == 0) {  // FULL：重建整张 QImage
        image_ = QImage(frame.width, frame.height, QImage::Format_RGB888);
        image_.fill(Qt::black);
        hasImage_ = true;
        for (const DisplayRect& rect : frame.rects) {
            blitRect(rect);
        }
    } else if (frame.frameType == 1) {  // PARTIAL：只修改目标 RECT
        if (!hasImage_ || image_.isNull() || image_.width() != frame.width ||
            image_.height() != frame.height) {
            return;  // 无已提交基准（防御；FrameAssembler 已保证 PARTIAL 有 base）
        }
        for (const DisplayRect& rect : frame.rects) {
            blitRect(rect);
        }
    } else {
        return;
    }

    frameId_ = frame.frameId;
    frameType_ = frame.frameType;
    maybeDumpPng();
    update();
}

void VirtualScreenWidget::clearDisplay() {
    image_ = QImage();
    hasImage_ = false;
    frameId_ = 0;
    frameType_ = 0;
    update();
}

bool VirtualScreenWidget::savePng(const QString& path) const {
    if (!hasImage_ || image_.isNull()) {
        return false;
    }
    return image_.save(path, "PNG");
}

void VirtualScreenWidget::setPngDumpDir(const QString& dir) {
    pngDumpDir_ = dir;
}

void VirtualScreenWidget::blitRect(const DisplayRect& rect) {
    if (image_.isNull() || rect.pixels.size() != static_cast<size_t>(rect.w) * rect.h * 2u) {
        return;
    }
    const int iw = image_.width();
    const int ih = image_.height();
    const int x0 = static_cast<int>(rect.x);
    const int y0 = static_cast<int>(rect.y);
    const int w = static_cast<int>(rect.w);
    const int h = static_cast<int>(rect.h);
    if (x0 < 0 || y0 < 0 || w <= 0 || h <= 0 || x0 + w > iw || y0 + h > ih) {
        return;  // 越界：不绘制（FrameAssembler 已校验，防御性检查）
    }
    const uint8_t* src = rect.pixels.data();
    uchar* dstBase = image_.bits() + y0 * image_.bytesPerLine() + x0 * 3;
    for (int y = 0; y < h; ++y) {
        uchar* dst = dstBase + y * image_.bytesPerLine();
        for (int x = 0; x < w; ++x) {
            const size_t off = (static_cast<size_t>(y) * w + x) * 2u;
            const uint16_t v = static_cast<uint16_t>(src[off]) |
                               (static_cast<uint16_t>(src[off + 1]) << 8);
            uint8_t r = 0, g = 0, b = 0;
            rgb565ToRgb888(v, r, g, b);
            dst[x * 3 + 0] = r;
            dst[x * 3 + 1] = g;
            dst[x * 3 + 2] = b;
        }
    }
}

void VirtualScreenWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x18, 0x18, 0x18));

    if (!hasImage_ || image_.isNull()) {
        painter.setPen(QColor(0x88, 0x88, 0x88));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No signal — waiting for FULL frame"));
        return;
    }

    // 等比例缩放（KeepAspectRatio = letterbox，禁止拉伸成非等比例）。
    QSize target = image_.size();
    target.scale(size(), Qt::KeepAspectRatio);
    QRect dst(QPoint(0, 0), target);
    dst.moveCenter(rect().center());

    // 关闭平滑缩放：整数倍/最近邻采样，保证像素校验颜色不被插值改变。
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(dst, image_);
}

QPoint VirtualScreenWidget::mapToFrame(const QPoint& pos) const {
    if (!hasImage_ || image_.isNull()) {
        return QPoint(-1, -1);
    }
    // M3：统一走共享 CoordinateMapper（与主机测试同一实现；letterbox 外返回 false）。
    int ox = -1;
    int oy = -1;
    if (!espview::input::CoordinateMapper::mapPoint(pos.x(), pos.y(), width(), height(),
                                                    image_.width(), image_.height(), ox, oy)) {
        return QPoint(-1, -1);
    }
    return QPoint(ox, oy);
}

void VirtualScreenWidget::mousePressEvent(QMouseEvent* event) {
    const QPoint f = mapToFrame(event->pos());
    qDebug() << "[input] mousePress frame" << f << "window" << event->pos();
    if (inputController_ != nullptr && f.x() >= 0 && f.y() >= 0) {
        // letterbox 外不发送（spec §12 推荐行为）。
        inputController_->onMousePress(espview::input::buttonBitFromQt(event->button()), f.x(),
                                       f.y(),
                                       espview::input::modifiersFromQt(event->modifiers()));
    }
    QWidget::mousePressEvent(event);
}

void VirtualScreenWidget::mouseMoveEvent(QMouseEvent* event) {
    const QPoint f = mapToFrame(event->pos());
    qDebug() << "[input] mouseMove frame" << f << "window" << event->pos();
    if (inputController_ != nullptr && f.x() >= 0 && f.y() >= 0) {
        inputController_->onMouseMove(f.x(), f.y(),
                                      espview::input::buttonsFromQt(event->buttons()));
    }
    QWidget::mouseMoveEvent(event);
}

void VirtualScreenWidget::mouseReleaseEvent(QMouseEvent* event) {
    const QPoint f = mapToFrame(event->pos());
    qDebug() << "[input] mouseRelease frame" << f << "window" << event->pos();
    if (inputController_ != nullptr && f.x() >= 0 && f.y() >= 0) {
        inputController_->onMouseRelease(espview::input::buttonBitFromQt(event->button()), f.x(),
                                         f.y(),
                                         espview::input::modifiersFromQt(event->modifiers()));
    }
    QWidget::mouseReleaseEvent(event);
}

void VirtualScreenWidget::wheelEvent(QWheelEvent* event) {
    const QPoint f = mapToFrame(event->position().toPoint());
    qDebug() << "[input] wheel delta" << event->angleDelta().y() << "frame" << f;
    if (inputController_ != nullptr && f.x() >= 0 && f.y() >= 0) {
        inputController_->onWheel(event->angleDelta().y(), f.x(), f.y(),
                                  espview::input::buttonsFromQt(event->buttons()),
                                  espview::input::modifiersFromQt(event->modifiers()));
    }
    QWidget::wheelEvent(event);
}

void VirtualScreenWidget::keyPressEvent(QKeyEvent* event) {
    qDebug() << "[input] keyPress" << event->key() << "text" << event->text()
             << "autoRepeat" << event->isAutoRepeat();
    if (inputController_ != nullptr) {
        inputController_->onKeyPress(event->key(),
                                     espview::input::modifiersFromQt(event->modifiers()),
                                     event->isAutoRepeat());
    }
    QWidget::keyPressEvent(event);
}

void VirtualScreenWidget::keyReleaseEvent(QKeyEvent* event) {
    qDebug() << "[input] keyRelease" << event->key() << "autoRepeat"
             << event->isAutoRepeat();
    if (inputController_ != nullptr) {
        inputController_->onKeyRelease(event->key(),
                                       espview::input::modifiersFromQt(event->modifiers()));
    }
    QWidget::keyReleaseEvent(event);
}

void VirtualScreenWidget::maybeDumpPng() {
    if (pngDumpDir_.isEmpty() || frameType_ != 0) {
        return;
    }
    const QString id = QString::number(frameId_);
    if (dumpedPngFrameIds_.contains(id)) {
        return;
    }
    const QString path = pngDumpDir_ + QStringLiteral("/full_") + id + QStringLiteral(".png");
    if (image_.save(path, "PNG")) {
        dumpedPngFrameIds_.append(id);
        if (dumpedPngFrameIds_.size() > 500) {
            dumpedPngFrameIds_.removeFirst();
        }
        qDebug() << "[dump] saved" << path;
    }
}

}  // namespace pc
}  // namespace espview
