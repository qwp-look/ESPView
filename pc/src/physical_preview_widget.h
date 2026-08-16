// ESPView M7-D2 — PhysicalPreviewWidget：物理 OLED 1bpp 预览消费端（Qt Widgets）。
//
// 规范来源：docs/DESIGN.md AE 节（M7-D2 PHYSICAL_PREVIEW 冻结）。
// 数据来源：PhysicalPreviewState 快照（纯 C++17 模型）。本文件只消费快照，
// 不解析 wire（PHYSICAL_PREVIEW 0x13 解析由后续代理完成，经 worker → GUI
// 信号送达 setFrame）。
//
// 职责与约束：
//   - 渲染：页式 1bpp（OledFb 布局：byte[page*width + x]，bit0=该页顶行，
//     y=page*8+bit）→ QImage Format_Mono（亮像素白、背景黑，OLED 观感）→
//     手动 Format_ARGB32 2x 放大（128x64 → 256x128）→ QLabel；
//   - 性能：只在 setFrame 时重建位图并 update()，无 50Hz 重绘；stale 由
//     1Hz 检查计时器驱动，只刷新状态文本（值变化才改 label）；
//   - 状态：Controller/Resolution/Format 只读行（Controller 由外部传入或
//     占位 "—"）；状态文本 = Live（绿）/ Stale（橙，>1s）/ No Preview
//     （无数据，灰）/ Preview Disabled（ui/previewEnabled=false，灰）；
//   - 双语：全部文案经 trText(lang, key)（key=英文原文；i18n.cpp 词条由
//     另一代理统一补充，本文件只引用 key）；
//   - 持久化：loadSettings/saveSettings 仅读写 ui/previewEnabled（经模型
//     toSettingsMap/fromSettingsMap，与纯 C++17 host 测试共用同一解析路径）。
//
// 集成（主代理）：把本文件加入 pc/CMakeLists.txt 的 espview_virtual_display
// 源列表（AUTOMOC ON 会自动 moc），在 main.cpp 中构造并接线：
//   worker/preview 信号 → setFrame(PhysicalPreviewState)；
//   断线 → clear()；语言切换 → setUiLanguage(int)；
//   QSettings（main.cpp persistedSettingsKeys 白名单需追加 ui/previewEnabled）
//   → loadSettings/saveSettings；Controller 名称 → setControllerName()。
//
// 注意：本类声明了 setEnabled(bool)（preview 使能，与 QWidget::setEnabled
// 同名遮蔽；基类使能为非虚，调用一律解析到本类）。本类内部不需要基类使能，
// 如需请显式 QWidget::setEnabled。

#pragma once

#include <QElapsedTimer>
#include <QString>
#include <QWidget>

#include "i18n.h"
#include "physical_preview_state.h"

class QLabel;
class QSettings;
class QTimer;

namespace espview {
namespace pc {

class PhysicalPreviewWidget : public QWidget {
    Q_OBJECT
public:
    // stale 阈值（显示侧，镜像模型默认 kDefaultPreviewStaleMs；>1s 才 Stale）。
    static constexpr qint64 kStaleThresholdMs = 1000;
    static constexpr int kStaleCheckMs = 1000;  // 1Hz stale 检查

    explicit PhysicalPreviewWidget(QWidget* parent = nullptr);

    // 数据入口（GUI 线程专用）：只消费快照。有新帧才重建 2x 位图并 update()。
    // snapshot.sessionConnected=false 或无像素 → 按无数据渲染（No Preview）。
    void setFrame(const PhysicalPreviewState& snapshot);

    // 断线/清空：清除位图与 Controller 能力元数据 → No Preview（灰）；
    // 跨会话不残留上次会话的能力事实（不伪造）。
    void clear();

    // 语言切换：重刷全部文案（状态/字段名/标题），不触碰帧与连接。
    void setUiLanguage(int lang);

    // QSettings 持久化（仅 ui/previewEnabled 键，经模型映射）。
    void loadSettings(QSettings& settings);
    void saveSettings(QSettings& settings);

    // Preview 使能（QSettings ui/previewEnabled 镜像；遮蔽 QWidget::setEnabled）。
    void setEnabled(bool enabled);
    bool previewEnabled() const { return previewEnabled_; }

    // Controller 名称：外部快照传入（如 "SSD1306"）或占位 "—"。
    void setControllerName(const QString& name);

    // 当前是否有可显示帧（connected 且含像素）。
    bool hasFrame() const { return haveFrame_; }

    QSize sizeHint() const override;

public slots:
    // 1Hz stale 检查：只刷新状态文本（值变化才改 label），不重绘位图。
    void onStaleTick();

private:
    void buildUi();
    void renderFrame();      // 用 state_ 重建 2x 位图（仅在 setFrame 时调用）
    void refreshStatus();    // 状态文本/配色（stale tick/语言/使能/清空时调用）
    QString t(const char* key) const;  // trText(lang_, key) 的 Qt 封装

    UiLang lang_ = UiLang::kEnglish;
    PhysicalPreviewState state_;   // 最近一次快照副本
    bool haveFrame_ = false;       // 当前可显示帧（connected 且含像素）
    bool previewEnabled_ = true;   // ui/previewEnabled（默认启用，AE.5）
    QString controllerName_;       // 空 = 占位 "—"
    QTimer* staleTimer_ = nullptr;
    QElapsedTimer frameClock_;     // 距上次 setFrame 的计时（stale 判定）
    qint64 lastFrameAtMs_ = -1;    // -1 = 无帧

    QLabel* titleLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;    // Live / Stale / No Preview / Preview Disabled
    QLabel* imageLabel_ = nullptr;     // 2x 位图（无帧时隐藏）
    QLabel* controllerLabel_ = nullptr;   // 行名（i18n 重刷）
    QLabel* resolutionLabel_ = nullptr;
    QLabel* formatLabel_ = nullptr;
    QLabel* controllerValue_ = nullptr;
    QLabel* resolutionValue_ = nullptr;
    QLabel* formatValue_ = nullptr;
    QString statusKey_;
};

}  // namespace pc
}  // namespace espview
