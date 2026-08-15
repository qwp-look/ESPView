// ESPView M7-C4 — DisplayModeWidget：Display Mode UX（Qt Widgets，GUI 线程）。
//
// 规范来源：M7-C 任务书 §二十二 + docs/DESIGN.md AA 节。
//
// 职责：
//   - 四模式卡片（Virtual Only / Physical Only / Mirror / Split）：每张卡片
//     显示模式名称 + 描述 + Virtual 侧状态 + Physical 侧状态（内容映射见
//     docs/DESIGN.md AA.3/AA.7：Mirror/PhysicalOnly → Application、
//     Split/VirtualOnly → Diagnostics、PhysicalOnly 下 Virtual 侧清除），
//     点击卡片即改选择；
//   - Apply + 状态行（Selected / Applied / Router 三段，selected != applied
//     时高亮差异）+ 条件徽标（DISCONNECTED / PHYSICAL UNAVAILABLE / SWITCHING /
//     WAITING FOR FULL / DEGRADED / READY，颜色 + 文案，不假装修复）；
//   - 持有 espview::display::DisplayUiState 模型副本（GUI 线程独占，无锁）；
//   - 用户点击 Apply → 模型 applyRequested() 校验后发出 applyRequested(int)，
//     由主控接线转发 ConnectionManager::sendDisplayMode；
//   - 主控把 SET_MODE ACK 经 onAck(bool) 投回本控件（模型收敛）；
//   - 主控可把权威状态快照经 setUiState() 注入（会话 / 路由 / FULL resync 事件）。
//
// i18n：全部文案用 t(key)（trText(lang_, key)），英文为源语言，Agent D 的
// i18n 目录接入后自动本地化；setUiLanguage() 只重刷文案，不触碰连接/显示状态。
//
// 线程模型：本控件只在 GUI 线程创建/使用；ConnectionManager::displayModeAck
// 是 queued 信号（Worker 线程 emit），连接本槽无需额外锁。

#pragma once

#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

#include <vector>

#include "display_ui_state.h"
#include "i18n.h"

namespace espview {
namespace pc {

class DisplayModeWidget : public QWidget {
    Q_OBJECT
public:
    explicit DisplayModeWidget(QWidget* parent = nullptr);

    // 外部权威状态快照（主控接线：会话/路由/FULL resync 事件后调用）。
    void setUiState(const display::DisplayUiState& state);
    // SET_MODE ACK 结果（主控接线：ConnectionManager::displayModeAck → 本槽）。
    void onAck(bool ok);
    // 语言切换（主控接线：LanguageSelector::languageChanged → 本槽）。
    // 只改文案，不触碰连接 / 显示状态。
    void setUiLanguage(int lang);

    // 当前模型副本（只读展示 / 测试断言）。
    const display::DisplayUiState& state() const { return state_; }

signals:
    // 用户点击 Apply 且模型判定应发送（已连接 / 能力满足 / 未在切换）。
    void applyRequested(int mode);

private slots:
    void onApplyClicked();

private:
    class ModeCard;  // 模式卡片（QFrame 子类，定义见 .cpp；无 Q_OBJECT）

    QString t(const char* key) const;          // trText(lang_, key) 的 Qt 封装
    QString modeName(int mode) const;          // i18n 可替换文案
    QString modeDescription(int mode) const;   // 模式描述（i18n key）
    QString virtualSideText(int mode) const;   // Virtual 侧内容（i18n key）
    QString physicalSideText(int mode) const;  // Physical 侧内容（i18n key）
    void refresh();                            // 全量重刷（状态 / 语言切换共用）
    void rebuildCards();                       // 一次性创建 4 张模式卡片
    void updateCards();                        // 卡片名称/描述/两侧状态/样式
    void updateStatusLine();                   // Selected/Applied/Router 差异行
    void updateConditionLabel();               // 条件徽标（颜色 + 文案）
    void updateApplyButton();                  // applyEnabled + Waiting for connection 标记

    QGroupBox* groupBox_ = nullptr;  // 语言切换时重刷标题
    display::DisplayUiState state_;
    std::vector<ModeCard*> cards_;
    QPushButton* applyButton_ = nullptr;
    QLabel* statusLine_ = nullptr;
    QLabel* conditionLabel_ = nullptr;
    UiLang lang_ = UiLang::kEnglish;
};

}  // namespace pc
}  // namespace espview