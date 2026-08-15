// ESPView M7-C3 — DisplayModeWidget 实现（见 display_mode_widget.h）。

#include "display_mode_widget.h"

#include <QVBoxLayout>

#include <QGroupBox>

namespace espview {
namespace pc {

using display::DisplayRouteMode;
using display::DisplayUiState;
using display::UiRouterState;

namespace {

// QStandardItemModel 的 enabled 角色（QComboBox 默认模型）。
constexpr int kItemEnabledRole = static_cast<int>(Qt::UserRole) - 1;

}  // namespace

QString DisplayModeWidget::t(const char* key) const {
    return QString::fromUtf8(trText(lang_, key));
}

DisplayModeWidget::DisplayModeWidget(QWidget* parent) : QWidget(parent) {
    modeCombo_ = new QComboBox(this);
    modeCombo_->addItem(t("Virtual Only"), 0);
    modeCombo_->addItem(t("Physical Only"), 1);
    modeCombo_->addItem(t("Mirror"), 2);
    modeCombo_->addItem(t("Split"), 3);
    modeCombo_->setCurrentIndex(0);

    applyButton_ = new QPushButton(t("Apply"), this);
    applyButton_->setEnabled(true);

    statusLabel_ = new QLabel(t("Disconnected"), this);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* group = new QGroupBox(t("Display Mode"), this);
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setSpacing(6);
    groupLayout->addWidget(modeCombo_);
    groupLayout->addWidget(applyButton_);
    groupLayout->addWidget(statusLabel_);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(group);
    outer->addStretch(1);

    connect(modeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &DisplayModeWidget::onComboChanged);
    connect(applyButton_, &QPushButton::clicked, this, &DisplayModeWidget::onApplyClicked);

    refresh();
}

QString DisplayModeWidget::modeName(int mode) const {
    switch (mode) {
        case 0:
            return t("Virtual Only");
        case 1:
            return t("Physical Only");
        case 2:
            return t("Mirror");
        case 3:
            return t("Split");
        default:
            return t("Unknown");
    }
}

void DisplayModeWidget::setUiState(const DisplayUiState& state) {
    state_ = state;
    refresh();
}

void DisplayModeWidget::onAck(bool ok) {
    state_.onAck(ok);
    refresh();
}

void DisplayModeWidget::setUiLanguage(int lang) {
    lang_ = static_cast<UiLang>(lang);
    refresh();
}

void DisplayModeWidget::onComboChanged(int index) {
    if (syncingUi_) {
        return;
    }
    if (index < 0 || index > 3) {
        return;
    }
    state_.setSelectedMode(static_cast<DisplayRouteMode>(index));
    refresh();
}

void DisplayModeWidget::onApplyClicked() {
    const bool shouldSend = state_.applyRequested();
    if (shouldSend) {
        emit applyRequested(static_cast<int>(state_.selectedMode));
    }
    refresh();
}

void DisplayModeWidget::refresh() {
    syncingUi_ = true;
    const int idx = static_cast<int>(state_.selectedMode);
    if (modeCombo_->currentIndex() != idx) {
        modeCombo_->setCurrentIndex(idx);
    }
    syncingUi_ = false;

    updateModeCombo();
    updateStatusLabel();
    updateApplyButton();
}

void DisplayModeWidget::updateModeCombo() {
    modeCombo_->setItemText(0, modeName(0));  // 语言切换时重刷 item 0（Virtual Only）
    for (int i = 1; i <= 3; ++i) {
        const bool usable = state_.physicalAvailable;
        // capability 门控（M7-C 冻结语义）：PhysicalOnly/Mirror/Split 不可选。
        modeCombo_->setItemData(i, QVariant(usable), kItemEnabledRole);
        QString text = modeName(i);
        if (!usable) {
            text += QStringLiteral(" (") + t("Unavailable") + QStringLiteral(")");
        }
        modeCombo_->setItemText(i, text);
    }
}

void DisplayModeWidget::updateStatusLabel() {
    QString text;
    QString color;
    if (!state_.sessionConnected) {
        text = t("Disconnected");
        color = QStringLiteral("#b71c1c");
    } else if (state_.routerState == UiRouterState::kUnavailable) {
        text = t("Unavailable");
        color = QStringLiteral("#b71c1c");
    } else if (state_.switchingInProgress ||
               state_.routerState == UiRouterState::kSwitching) {
        text = t("Switching");
        color = QStringLiteral("#e65100");
    } else if (state_.fullResyncPending) {
        text = t("FULL resync");
        color = QStringLiteral("#e65100");
    } else if (state_.routerState == UiRouterState::kDegraded) {
        text = t("Degraded");
        color = QStringLiteral("#e65100");
    } else if (state_.routerState == UiRouterState::kConnected) {
        text = t("Ready");  // 已连接 + 已同步 + 未降级
        color = QStringLiteral("#1b5e20");
    } else {
        text = t("Connected");  // 已连接但模式尚未收敛（kIdle）
        color = QStringLiteral("#1b5e20");
    }
    statusLabel_->setText(text);
    statusLabel_->setStyleSheet(
        QStringLiteral("color:%1;font-weight:bold;").arg(color));
    if (!state_.lastError.empty()) {
        statusLabel_->setToolTip(QString::fromStdString(state_.lastError));
    } else {
        statusLabel_->setToolTip(QString());
    }
}

void DisplayModeWidget::updateApplyButton() {
    applyButton_->setEnabled(state_.applyEnabled);
    if (state_.waitingForConnection) {
        applyButton_->setText(t("Apply") + QStringLiteral(" — ") +
                              t("Waiting for connection"));
        applyButton_->setToolTip(
            t("Session not connected; the selected mode will be sent once connected."));
    } else {
        applyButton_->setText(t("Apply"));
        applyButton_->setToolTip(
            t("Send the selected display mode to the device (SET_MODE)."));
    }
}

}  // namespace pc
}  // namespace espview
