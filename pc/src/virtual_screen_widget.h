// ESPView M2 — VirtualScreenWidget：ESP32 虚拟屏幕（Qt Widget）。
//
// 规范来源：docs/DESIGN.md M2 节 + spec §7/§9/§10/§11/§17/§18。
//
// 职责：只接收已经提交的 DisplayFrame；保存 PC 端「最后一次 committed 镜像」；
// paintEvent 等比例（letterbox）绘制 QImage；resize 不改变逻辑分辨率。
//
// 显示规则（与协议语义一致，Qt 层不重新判断协议合法性）：
//   - FULL   → 用新帧重建整张 QImage（FrameAssembler 已保证帧内完整性）；
//   - PARTIAL→ 只把目标 RECT 写入当前 QImage（不重建整张）；
//   - 断线   → clearDisplay() 清空/隐藏旧画面；重连后必须等新的 FULL 才恢复。
//
// Qt 只知道 DisplayFrame；不接触 Packet / Message / CRC / CHUNKED / SEQ。
// GUI 线程独占本类（Worker 经 queued signal 调 setFrame）。
//
// 像素转换：协议 RGB565 little-endian（低字节在前）→ QImage::Format_RGB888，
// 显式按 R5/G6/B5 位域展开，不用 reinterpret_cast / 依赖 x86 endian。

#pragma once

#include <cstdint>
#include <QImage>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "display_frame.h"
#include "display_geometry.h"  // M8-B（B2）：默认几何单一来源（无帧时 logicalSize 回退）
#include "i18n.h"

namespace espview {
namespace pc {

class InputController;

class VirtualScreenWidget : public QWidget {
    Q_OBJECT
public:
    explicit VirtualScreenWidget(QWidget* parent = nullptr);

    // GUI 线程专用：接收已提交帧并更新显示。
    void setFrame(const DisplayFrame& frame);
    // 断线/停止：清空并隐藏旧画面（不继续显示旧镜像作为“当前真实状态”）。
    void clearDisplay();
    bool hasImage() const { return hasImage_; }

    // 保存当前画面为 PNG（调试/像素校验用，非核心依赖）。
    bool savePng(const QString& path) const;
    // --dump-png 调试：设置后每个新 FULL commit 保存一张 PNG 到该目录。
    // 文件名 full_<frameId>.png；同 frameId 只存一次；目录需已存在。
    void setPngDumpDir(const QString& dir);

    // M8-B（B2）：逻辑分辨率跟随当前帧（rendered）；无帧时回退默认几何
    // （kVirtualDisplayGeometry）。resize 窗口不改变本值（只影响显示缩放）。
    QSize logicalSize() const {
        if (hasImage_ && !image_.isNull()) {
            return image_.size();
        }
        return QSize(espview::display::kVirtualDisplayGeometry.width,
                     espview::display::kVirtualDisplayGeometry.height);
    }

    // M3：接 InputController（GUI 线程同线程调用；传入已映射的逻辑坐标事件）。
    // letterbox 黑边外的鼠标事件不会上送（spec §12 推荐行为）。
    void setInputController(InputController* c) { inputController_ = c; }

    // M7-G7：语言切换（只改无信号占位文案，不触碰帧/输入链路）。
    void setUiLanguage(int lang);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    void blitRect(const DisplayRect& rect);
    QPoint mapToFrame(const QPoint& pos) const;
    void maybeDumpPng();

    QImage image_;  // Format_RGB888，逻辑分辨率（320x240），GUI 线程独占
    bool hasImage_ = false;
    uint16_t frameId_ = 0;
    uint8_t frameType_ = 0;

    QString pngDumpDir_;
    QStringList dumpedPngFrameIds_;  // 已保存的 frameId，避免重复写盘
    InputController* inputController_ = nullptr;
    UiLang lang_ = UiLang::kEnglish;  // M7-G7：无信号占位文案语言
};

}  // namespace pc
}  // namespace espview
