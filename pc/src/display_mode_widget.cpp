// ESPView M7-C4 — DisplayModeWidget 实现（见 display_mode_widget.h）。

#include "display_mode_widget.h"

#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QVBoxLayout>

#include <functional>

namespace espview {
namespace pc {

using display::DisplayRouteMode;
using display::DisplayUiState;
using display::UiRouterState;

namespace {

// 状态语义色（与主窗口状态面板一致的红/橙/绿体系）。
constexpr const char* kColorGreen = "#1b5e20";
constexpr const char* kColorOrange = "#e65100";
constexpr const char* kColorRed = "#b71c1c";
constexpr const char* kColorNeutral = "#455a64";
constexpr const char* kColorGray = "#9e9e9e";
constexpr const char* kColorAccent = "#1565c0";

// 模式 → 各侧内容（docs/DESIGN.md AA.3/AA.7 定稿：Mirror/PhysicalOnly →
// Application；Split/VirtualOnly → Diagnostics；PhysicalOnly 下 Virtual 侧
// 无内容（清除）。值为 i18n key（英文原文）。
constexpr const char* kVirtualContent[4] = {
    "Application",  // VirtualOnly
    "(cleared)",    // PhysicalOnly
    "Application",  // Mirror
    "Application",  // Split
};
constexpr const char* kPhysicalContent[4] = {
    "Diagnostics",  // VirtualOnly（诊断页继续，非路由输出）
    "Application",  // PhysicalOnly
    "Application",  // Mirror
    "Diagnostics",  // Split
};

}  // namespace

// ---- ModeCard：单个模式卡片（点击选择；纯 QFrame 子类，无 Q_OBJECT）----
class DisplayModeWidget::ModeCard : public QFrame {
public:
    explicit ModeCard(int mode, QWidget* parent = nullptr)
        : QFrame(parent), mode_(mode) {
        setCursor(Qt::PointingHandCursor);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(10, 8, 10, 8);
        lay->setSpacing(3);

        name_ = new QLabel(this);
        QFont nameFont = name_->font();
        nameFont.setBold(true);
        name_->setFont(nameFont);

        desc_ = new QLabel(this);
        desc_->setWordWrap(true);

        virtualSide_ = new QLabel(this);
        physicalSide_ = new QLabel(this);

        lay->addWidget(name_);
        lay->addWidget(desc_);
        lay->addWidget(virtualSide_);
        lay->addWidget(physicalSide_);
        lay->addStretch(1);  // 等高卡片：内容顶部对齐，底部留白吸收行高差
    }

    int mode() const { return mode_; }

    void setTexts(const QString& name, const QString& desc, const QString& virtualSide,
                  const QString& physicalSide) {
        name_->setText(name);
        desc_->setText(desc);
        virtualSide_->setText(virtualSide);
        physicalSide_->setText(physicalSide);
    }

    void setNameColor(const QString& color) {
        name_->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    }

    void setDescriptionColor(const QString& color) {
        desc_->setStyleSheet(QStringLiteral("color:%1;").arg(color));
    }

    void setSideColors(const QString& virtualColor, const QString& physicalColor) {
        virtualSide_->setStyleSheet(QStringLiteral("color:%1;").arg(virtualColor));
        physicalSide_->setStyleSheet(QStringLiteral("color:%1;").arg(physicalColor));
    }

    // 选中/不可用外观（整卡边框 + 底色；只在此控件内生效）。
    void applyCardStyle(bool selected, bool unavailable) {
        QString qss;
        if (unavailable) {
            qss = QStringLiteral(
                "QFrame { border:1px dashed #bdbdbd; border-radius:4px; "
                "background:#fafafa; }");
        } else if (selected) {
            qss = QStringLiteral(
                "QFrame { border:2px solid #1565c0; border-radius:4px; "
                "background:#e8f0fe; }");
        } else {
            qss = QStringLiteral(
                "QFrame { border:1px solid #cfd8dc; border-radius:4px; "
                "background:#ffffff; }");
        }
        setStyleSheet(qss);
    }

    // 点击激活回调（由 DisplayModeWidget 注入；非信号，避免 Q_OBJECT/moc 依赖）。
    std::function<void(int)> onActivated;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos()) &&
            onActivated) {
            onActivated(mode_);
        }
        QFrame::mouseReleaseEvent(event);
    }

private:
    int mode_ = 0;
    QLabel* name_ = nullptr;
    QLabel* desc_ = nullptr;
    QLabel* virtualSide_ = nullptr;
    QLabel* physicalSide_ = nullptr;
};

QString DisplayModeWidget::t(const char* key) const {
    return QString::fromUtf8(trText(lang_, key));
}

DisplayModeWidget::DisplayModeWidget(QWidget* parent) : QWidget(parent) {
    groupBox_ = new QGroupBox(t("Display Mode"), this);
    QGroupBox* group = groupBox_;
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setSpacing(6);

    rebuildCards();
    auto* cardsRow = new QHBoxLayout;
    cardsRow->setSpacing(8);
    for (ModeCard* card : cards_) {
        cardsRow->addWidget(card, 1);
    }
    groupLayout->addLayout(cardsRow);

    statusLine_ = new QLabel(group);
    statusLine_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    groupLayout->addWidget(statusLine_);

    conditionLabel_ = new QLabel(group);
    conditionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    groupLayout->addWidget(conditionLabel_);

    auto* applyRow = new QHBoxLayout;
    applyButton_ = new QPushButton(t("Apply"), group);
    applyRow->addWidget(applyButton_);
    applyRow->addStretch(1);
    groupLayout->addLayout(applyRow);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(group);
    outer->addStretch(1);

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

QString DisplayModeWidget::modeDescription(int mode) const {
    switch (mode) {
        case 0:
            return t("Only the PC display is active; the physical side keeps showing diagnostics.");
        case 1:
            return t("Only the physical display is active; the PC side is cleared.");
        case 2:
            return t("Both displays show the same application view.");
        case 3:
            return t("PC shows the application; physical shows diagnostics.");
        default:
            return QString();
    }
}

QString DisplayModeWidget::virtualSideText(int mode) const {
    if (mode < 0 || mode > 3) {
        return t("Unknown");
    }
    return t(kVirtualContent[mode]);
}

QString DisplayModeWidget::physicalSideText(int mode) const {
    if (mode < 0 || mode > 3) {
        return t("Unknown");
    }
    return t(kPhysicalContent[mode]);
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

void DisplayModeWidget::onApplyClicked() {
    const bool shouldSend = state_.applyRequested();
    if (shouldSend) {
        emit applyRequested(static_cast<int>(state_.selectedMode));
    }
    refresh();
}

void DisplayModeWidget::rebuildCards() {
    cards_.clear();
    for (int mode = 0; mode <= 3; ++mode) {
        auto* card = new ModeCard(mode, this);
        card->onActivated = [this](int m) {
            state_.setSelectedMode(static_cast<DisplayRouteMode>(m));
            refresh();
        };
        cards_.push_back(card);
    }
}

void DisplayModeWidget::updateCards() {
    for (ModeCard* card : cards_) {
        const int mode = card->mode();
        const auto routeMode = static_cast<DisplayRouteMode>(mode);
        const bool selected = (routeMode == state_.selectedMode);
        const bool unavailable =
            !state_.physicalAvailable && display::modeRequiresPhysical(routeMode);

        QString name = modeName(mode);
        QString vSide = t("Virtual") + QStringLiteral(": ") + virtualSideText(mode);
        QString pSide = t("Physical") + QStringLiteral(": ") + physicalSideText(mode);

        // 侧状态颜色：路由输出 → 绿；未启用 / 已清除 → 灰；物理降级 → 橙。
        QString vColor = QString::fromUtf8(kColorGray);
        QString pColor = QString::fromUtf8(kColorGray);
        if (routeMode != DisplayRouteMode::kPhysicalOnly) {
            vColor = QString::fromUtf8(kColorGreen);
        }
        if (display::modeRequiresPhysical(routeMode)) {
            pColor = QString::fromUtf8(kColorGreen);
        }
        if (routeMode == state_.appliedMode &&
            state_.routerState == UiRouterState::kDegraded &&
            display::modeRequiresPhysical(routeMode)) {
            pColor = QString::fromUtf8(kColorOrange);  // 物理侧降级：不假装修复
        }

        if (unavailable) {
            name += QStringLiteral(" (") + t("Unavailable") + QStringLiteral(")");
            vColor = QString::fromUtf8(kColorGray);
            pColor = QString::fromUtf8(kColorGray);
            card->setToolTip(t("Physical unavailable"));
        } else {
            card->setToolTip(QString());
        }

        card->setTexts(name, modeDescription(mode), vSide, pSide);
        card->setNameColor(selected ? QString::fromUtf8(kColorAccent)
                                    : (unavailable ? QString::fromUtf8(kColorGray)
                                                   : QStringLiteral("#212121")));
        card->setDescriptionColor(unavailable ? QString::fromUtf8(kColorGray)
                                              : QString::fromUtf8(kColorNeutral));
        card->setSideColors(vColor, pColor);
        card->applyCardStyle(selected, unavailable);
    }
}

void DisplayModeWidget::updateStatusLine() {
    const QString selName = modeName(static_cast<int>(state_.selectedMode));
    const QString appName = modeName(static_cast<int>(state_.appliedMode));
    const bool diff = state_.selectedMode != state_.appliedMode;
    const QString diffColor = QString::fromUtf8(kColorOrange);
    const QString normalColor = QStringLiteral("#212121");

    QString routerName;
    QString routerColor;
    switch (state_.routerState) {
        case UiRouterState::kIdle:
            routerName = t("Idle");
            routerColor = QString::fromUtf8(kColorNeutral);
            break;
        case UiRouterState::kSwitching:
            routerName = t("Switching");
            routerColor = QString::fromUtf8(kColorOrange);
            break;
        case UiRouterState::kConnected:
            routerName = t("Connected");
            routerColor = QString::fromUtf8(kColorGreen);
            break;
        case UiRouterState::kDegraded:
            routerName = t("Degraded");
            routerColor = QString::fromUtf8(kColorOrange);
            break;
        case UiRouterState::kUnavailable:
            routerName = t("Unavailable");
            routerColor = QString::fromUtf8(kColorRed);
            break;
    }

    const QString diffStyle = diff ? QStringLiteral("font-weight:bold;") : QString();
    QString html;
    html += QStringLiteral("<span style='color:%1;%2'>%3</span>")
                .arg(diff ? diffColor : normalColor, diffStyle,
                     t("Selected") + QStringLiteral(": ") + selName);
    html += QStringLiteral(" / ");
    html += QStringLiteral("<span style='color:%1;%2'>%3</span>")
                .arg(diff ? diffColor : normalColor, diffStyle,
                     t("Applied") + QStringLiteral(": ") + appName);
    html += QStringLiteral(" / ");
    html += QStringLiteral("<span style='color:%1;'>%2</span>")
                .arg(routerColor, t("Router") + QStringLiteral(": ") + routerName);
    statusLine_->setText(html);

    if (!state_.lastError.empty()) {
        statusLine_->setToolTip(QString::fromStdString(state_.lastError));
    } else {
        statusLine_->setToolTip(QString());
    }
}

void DisplayModeWidget::updateConditionLabel() {
    QString text;
    QString color;
    if (!state_.sessionConnected) {
        text = t("Disconnected");
        color = QString::fromUtf8(kColorRed);
    } else if (state_.routerState == UiRouterState::kUnavailable) {
        text = t("Physical unavailable");
        color = QString::fromUtf8(kColorRed);
    } else if (state_.switchingInProgress ||
               state_.routerState == UiRouterState::kSwitching) {
        text = t("Switching");
        color = QString::fromUtf8(kColorOrange);
    } else if (state_.fullResyncPending) {
        text = t("Waiting for FULL");
        color = QString::fromUtf8(kColorOrange);
    } else if (state_.routerState == UiRouterState::kDegraded) {
        text = t("Degraded");
        color = QString::fromUtf8(kColorOrange);
    } else if (state_.routerState == UiRouterState::kConnected) {
        text = t("Ready");
        color = QString::fromUtf8(kColorGreen);
    } else {
        text = t("Connected");
        color = QString::fromUtf8(kColorGreen);
    }
    // 断开时已点过 Apply → 追加等待提示（与 Apply 按钮一致，不假装已发送）。
    if (!state_.sessionConnected && state_.waitingForConnection) {
        text += QStringLiteral(" — ") + t("Waiting for connection");
    }
    // M7-G3: model errors must have visible feedback (not hidden in tooltip only).
    // States already carrying the error text stay as-is; any other pending error
    // (ACK fail / timeout / rejected selection) becomes an explicit red Error.
    const bool errorTextShown =
        text == t("Disconnected") ||
        text.startsWith(t("Disconnected") + QStringLiteral(" — ")) ||
        text == t("Physical unavailable") || text == t("Switching") ||
        text == t("Waiting for connection");
    if (!state_.lastError.empty() && !errorTextShown) {
        if (state_.lastError == "physical display unavailable") {
            text = t("Physical unavailable");
        } else {
            text = t("Error");
        }
        color = QString::fromUtf8(kColorRed);
    }
    conditionLabel_->setText(QStringLiteral("● ") + text);
    conditionLabel_->setStyleSheet(
        QStringLiteral("color:%1;font-weight:bold;").arg(color));
    if (!state_.lastError.empty()) {
        conditionLabel_->setToolTip(QString::fromStdString(state_.lastError));
    } else {
        conditionLabel_->setToolTip(QString());
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

void DisplayModeWidget::refresh() {
    groupBox_->setTitle(t("Display Mode"));  // 语言切换重刷标题
    updateCards();
    updateStatusLine();
    updateConditionLabel();
    updateApplyButton();
}

}  // namespace pc
}  // namespace espview