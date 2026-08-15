// ESPView M7-C3 — DisplayModeWidget：Display Mode UI（Qt Widgets，GUI 线程）。
//
// 规范来源：M7-C 任务书 §二十二 + docs/DESIGN.md AA 节。
//
// 职责：
//   - 四模式下拉（Virtual Only / Physical Only / Mirror / Split）+ Apply +
//     状态标签（SWITCHING / CONNECTED / FULL RESYNC / READY / DEGRADED /
//     DISCONNECTED / Unavailable）；
//   - 持有 espview::display::DisplayUiState 模型副本（GUI 线程独占，无锁）；
//   - 用户点击 Apply → 模型 applyRequested() 校验后发出 applyRequested(int)，
//     由主控接线转发 ConnectionManager::sendDisplayMode；
//   - 主控把 SET_MODE ACK 经 onAck(bool) 投回本控件（模型收敛）；
//   - 主控可把权威状态快照经 setUiState() 注入（会话 / 路由 / FULL resync 事件）。
//
// i18n：全部文案用 tr()（可替换字符串），英文为源语言，Agent D 的 i18n
// 组件经 QTranslator 接入后自动本地化。
//
// 线程模型：本控件只在 GUI 线程创建/使用；ConnectionManager::displayModeAck
// 是 queued 信号（Worker 线程 emit），连接本槽无需额外锁。

#pragma once

#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

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
    void onComboChanged(int index);
    void onApplyClicked();

private:
    QString modeName(int mode) const;   // i18n 可替换文案
    QString t(const char* key) const;   // trText(lang_, key) 的 Qt 封装
    void refresh();                     // 同步 combo / 按钮 / 状态标签
    void updateModeCombo();             // capability 门控：不可用项禁用 + 后缀
    void updateStatusLabel();           // 状态标签映射（见文件头）
    void updateApplyButton();           // applyEnabled + Waiting for connection 标记

    display::DisplayUiState state_;
    QComboBox* modeCombo_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    bool syncingUi_ = false;  // 程序化同步 combo 时抑制信号重入
    UiLang lang_ = UiLang::kEnglish;
};

}  // namespace pc
}  // namespace espview
