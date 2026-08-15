// ESPView M7-C3 — SplitDrawer：Split 模式右侧诊断抽屉（Qt Widgets）。
//
// 职责（任务书 §二十二 15–18）：
//   - 面板标题 "ESP32 Physical / Diagnostics"；
//   - 内容区：label 组展示 Physical Diagnostics/Status（RSSI / CH / Heap /
//     Frame / Errors / Transport / OLED addr / Scene / Availability），
//     字段由外部数据（Agent E 的 PhysicalStatus）更新，本类不做任何
//     ERROR 文本解析 / counters 统计；
//   - open/close（setVisible 槽）+ resize（固定宽度槽 setDrawerWidth /
//     QSplitter 手柄，范围 [200, 560] 与 SplitState 常量一致）；
//   - 信号 visibilityChanged(bool) / widthChanged(int)；
//   - QSettings 持久化（split/drawerVisible + split/drawerWidth；
//     键名与 SplitState::toSettingsMap 一致；绝不保存任何凭据）。
//
// 与 VirtualScreenWidget 的关系：本类只是 Qt 布局层 —— VirtualScreenWidget
// 保持 320x240 逻辑分辨率不变（实际 widget 尺寸由布局/QSplitter 缩放）。
// 本类不持有、不伪造 OLED framebuffer：当前 wire format 没有 Physical
// framebuffer uplink（协议 gap，见报告），因此第一版只显示状态文本
// （ERROR 文本行 -> PhysicalStatus -> label）。
//
// 布局最小方案（dock-like）：QSplitter(screen | drawer) 或独立固定宽度嵌入。
//   - QSplitter 模式：构造时 setMinimumWidth/setMaximumWidth，手柄自动在
//     [200, 560] 内拖动（不侵入 Virtual 侧）；宽度经 resizeEvent ->
//     widthChanged 上报；
//   - 固定宽度槽模式：setDrawerWidth() 夹取并 setFixedWidth。

#pragma once

#include <QFrame>
#include <QSize>
#include <utility>
#include <vector>

#include "i18n.h"
#include "physical_status.h"  // PhysicalStatus（按值快照，纯 C++17 零依赖）
#include "split_state.h"

class QFormLayout;
class QLabel;
class QSettings;
class QTimer;
class QToolButton;

namespace espview {
namespace display {
}  // namespace display

namespace pc {

using espview::display::SplitState;

class SplitDrawer : public QFrame {
    Q_OBJECT
public:
    // settings 为可选（非拥有）持久化后端；nullptr = 不持久化。
    explicit SplitDrawer(QSettings* settings = nullptr, QWidget* parent = nullptr);

    // 数据入口：把 Agent E 解析出的 PhysicalStatus 渲染到 label 组
    // （GUI 线程专用，与 VirtualScreenWidget 同线程约束）。
    void setPhysicalStatus(const espview::display::PhysicalStatus& status);

    // 清空所有字段为占位符（断线 / 未解析 / 头未就位时）。
    void clearStatus();

    // 模型访问（主窗口同步模式切换 / 菜单勾选时使用）。
    const SplitState& state() const { return state_; }
    void applyState(const SplitState& state);

    // QSettings 持久化（键名与 SplitState 一致；不含凭据）。
    void loadSettings();
    void saveSettings();
    // 语言切换：只重刷字段名/标题文案，不触碰数据 / 连接。
    void setUiLanguage(int lang);

    QSize sizeHint() const override;

public slots:
    // 开合（QWidget::setVisible 的虚槽覆写：发出 visibilityChanged）。
    void setVisible(bool visible) override;
    void open() { setVisible(true); }
    void close() { setVisible(false); }

    // 固定宽度槽：夹取 [200, 560] 并 setFixedWidth；若父级为 QSplitter
    // 则不锁宽度（由手柄驱动，min/max 已限定范围）。
    void setDrawerWidth(int width);

signals:
    void visibilityChanged(bool visible);
    void widthChanged(int width);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildUi();
    void renderStatus();  // 用 lastStatus_ / haveStatus_ 重渲染值区域
    void applyWidth(int width);
    void scheduleSave();
    QString t(const char* key) const;  // trText(lang_, key) 的 Qt 封装

    SplitState state_;
    QSettings* settings_ = nullptr;
    QTimer* saveTimer_ = nullptr;
    QToolButton* closeButton_ = nullptr;
    int lastWidth_ = -1;
    UiLang lang_ = UiLang::kEnglish;
    // 字段名标签 + i18n key（语言切换时重刷文案）。
    std::vector<std::pair<QLabel*, const char*>> fieldNameKeys_;
    QLabel* titleLabel_ = nullptr;
    espview::display::PhysicalStatus lastStatus_;  // 最近一次快照（语言切换重刷）
    bool haveStatus_ = false;

    // 内容 label 组（setPhysicalStatus / clearStatus 更新）。
    QLabel* rssiLabel_ = nullptr;
    QLabel* channelLabel_ = nullptr;
    QLabel* heapLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* errorsLabel_ = nullptr;
    QLabel* transportLabel_ = nullptr;
    QLabel* oledAddrLabel_ = nullptr;
    QLabel* sceneLabel_ = nullptr;
    QLabel* availabilityLabel_ = nullptr;
};

}  // namespace pc
}  // namespace espview
