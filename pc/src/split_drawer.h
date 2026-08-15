// ESPView M7-C4 — SplitDrawer：Split 模式右侧物理诊断抽屉（Qt Widgets）。
//
// 职责（任务书 §二十二 15–18 + M7-C4 产品化）：
//   - 面板标题 "ESP32 Physical / Diagnostics"，内容仅 Diagnostics/Status
//     文本，绝不绘制/伪造 OLED framebuffer 像素（当前 wire 格式没有 Physical
//     framebuffer uplink，见报告）；
//   - 数据来源：只读 PhysicalStatus 快照（Agent E）+ SplitState 模型；
//     不复制 RuntimeStats、不解析 ERROR 文本行、不做 counters 统计；
//   - 三分区：Physical Display / Wi-Fi / TCP / Session / State，窄宽度时
//     QFormLayout 自动换行（QScrollArea 横向滚动条永久关闭，无滚动条风暴）；
//   - open/close（✕ 按钮收起；主窗口 Split 模式选中时 open()）+ QSplitter
//     手柄拖拽调整宽度，范围 [kMinDrawerWidth, kMaxDrawerWidth]；
//   - 宽度/可见性持久化：QSettings split/drawerWidth + split/drawerVisible
//     （键名与 SplitState 一致；绝不保存任何凭据/密码）；
//   - DPI aware：Qt 逻辑像素随 DPI 自动缩放，最小宽度由布局 QFontMetrics
//     minimumSize 推导，不硬编码像素阈值做布局判断；
//   - 性能：setPhysicalStatus 只存快照并调度合并渲染（≥200ms 节流 →
//     刷新 ≤5Hz），stale 判定由 1Hz 检查计时器驱动，无 50Hz 重绘；
//   - 语言：setUiLanguage(int) 重刷全部文案（标题/分区/字段/状态值/stale），
//     不触碰连接与帧。

#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QSize>
#include <QString>
#include <utility>
#include <vector>

#include "i18n.h"
#include "physical_status.h"  // PhysicalStatus（按值快照，纯 C++17 零依赖）
#include "split_state.h"

class QColor;
class QFont;
class QFormLayout;
class QLabel;
class QSettings;
class QShowEvent;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace espview {
namespace pc {

using espview::display::SplitState;

class SplitDrawer : public QFrame {
    Q_OBJECT
public:
    // 宽度产品约束（Qt 层；SplitState 模型仍是 [200,560] 超集，共享模型不改）。
    static constexpr int kMinDrawerWidth = 200;
    static constexpr int kMaxDrawerWidth = 480;
    static constexpr int kDefaultDrawerWidth = 320;
    // 诊断刷新 ≤5Hz（200ms 节流）；stale 阈值 5s（遥测典型刷新 ≥1Hz）。
    static constexpr int kMinRenderIntervalMs = 200;
    static constexpr int kStaleThresholdMs = 5000;
    static constexpr int kStaleCheckMs = 1000;

    // settings 为可选（非拥有）持久化后端；nullptr = 不持久化。
    explicit SplitDrawer(QSettings* settings = nullptr, QWidget* parent = nullptr);

    // 数据入口：把 Agent E 解析出的 PhysicalStatus 渲染到 label 组（GUI 线程
    // 专用）。只存快照 + 调度合并渲染（≤5Hz），不做重量级布局。
    void setPhysicalStatus(const espview::display::PhysicalStatus& status);

    // M7-D2：在抽屉顶部插入外部预览 widget（如 PhysicalPreviewWidget）与
    // 分区标题；分区标题随 setUiLanguage 重刷。仅 buildUi 后调用一次。
    void addExternalWidget(const char* sectionKey, QWidget* widget);

    // 清空所有字段为占位符（断线 / 未解析 / 头未就位时；会话失联语义）。
    void clearStatus();

    // 模型访问（主窗口同步模式切换 / 菜单勾选时使用）。
    const SplitState& state() const { return state_; }
    void applyState(const SplitState& state);

    // QSettings 持久化（键名与 SplitState 一致；不含凭据）。
    void loadSettings();
    void saveSettings();
    // 语言切换：重刷全部文案（标题/分区/字段/状态值/stale），不触碰数据/连接。
    void setUiLanguage(int lang);

    QSize sizeHint() const override;

public slots:
    // 开合（QWidget::setVisible 的虚槽覆写：发出 visibilityChanged）。
    void setVisible(bool visible) override;
    void open() { setVisible(true); }
    void close() { setVisible(false); }

    // 宽度槽：夹取 [kMinDrawerWidth, kMaxDrawerWidth]；父级为 QSplitter 时
    // 通过 splitter->setSizes 移动手柄（不锁 setFixedWidth）。
    void setDrawerWidth(int width);

signals:
    void visibilityChanged(bool visible);
    void widthChanged(int width);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private slots:
    void onRenderTimer();   // 合并渲染（节流 ≤5Hz）
    void onStaleTick();     // 1Hz stale/时间戳检查

private:
    void buildUi();
    void configureForm(QFormLayout* form);
    void addSection(QVBoxLayout* layout, const char* key, const QFont& sectionFont);
    void addRow(QFormLayout* form, const char* key, const QFont& valueFont,
                QLabel*& valueLabel);
    void renderStatus();    // 用 lastStatus_ / haveStatus_ 重渲染值区域
    void updateStaleUi();   // stale 标记 + “Updated N s ago”（值变化才改 label）
    void scheduleRender();  // 200ms 节流调度
    void applyWidth(int width);
    void scheduleSave();
    void configureSplitterHost();  // 收起只能走 ✕/open()，拖拽不压到 0
    QString t(const char* key) const;  // trText(lang_, key) 的 Qt 封装

    SplitState state_;
    QSettings* settings_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    QTimer* renderTimer_ = nullptr;
    QTimer* staleTimer_ = nullptr;
    QToolButton* collapseButton_ = nullptr;
    QElapsedTimer renderClock_;    // 距上次真实渲染的节流计时
    QElapsedTimer lastRefreshMs_;  // 距上次 setPhysicalStatus 的计时
    bool renderPending_ = false;
    int lastWidth_ = -1;
    int minWidgetWidth_ = kMinDrawerWidth;  // DPI/字宽推导的最小宽度
    UiLang lang_ = UiLang::kEnglish;

    // 文案重刷表（语言切换时统一重刷）。
    std::vector<std::pair<QLabel*, const char*>> sectionHeaderKeys_;
    std::vector<std::pair<QLabel*, const char*>> fieldNameKeys_;
    QLabel* titleLabel_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;  // buildUi 填充；addExternalWidget 插入

    espview::display::PhysicalStatus lastStatus_;  // 最近一次快照
    bool haveStatus_ = false;
    bool disconnected_ = false;   // clearStatus() = 会话失联（断线语义）
    bool everConnected_ = false;  // 曾 Connected → 再次 Connecting 显示 Reconnecting

    // ---- Physical Display 分区 ----
    QLabel* controllerLabel_ = nullptr;
    QLabel* resolutionLabel_ = nullptr;
    QLabel* i2cLabel_ = nullptr;
    QLabel* physStateLabel_ = nullptr;  // Ready / Degraded / Unavailable
    QLabel* sceneLabel_ = nullptr;
    QLabel* lastFlushLabel_ = nullptr;
    QLabel* errorsLabel_ = nullptr;

    // ---- Wi-Fi / TCP 分区 ----
    QLabel* ssidLabel_ = nullptr;      // 安全占位：遥测不提供，绝不显示密码
    QLabel* rssiLabel_ = nullptr;
    QLabel* channelLabel_ = nullptr;
    QLabel* ipLabel_ = nullptr;        // 安全占位：遥测不提供
    QLabel* tcpStatusLabel_ = nullptr;
    QLabel* sessionLabel_ = nullptr;   // HELLO / PING 存活
    QLabel* frameLabel_ = nullptr;
    QLabel* heapLabel_ = nullptr;

    // ---- Session / State 分区 ----
    QLabel* stateLabel_ = nullptr;         // 会话横幅（彩色）
    QLabel* lastUpdateLabel_ = nullptr;    // “Updated N s ago”
    QLabel* staleWarning_ = nullptr;       // 超过阈值 → 显示 stale 警告行
    QString lastUpdateShown_;
    bool staleShown_ = false;
};

}  // namespace pc
}  // namespace espview
