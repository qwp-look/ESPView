// ESPView M7-C3 — SplitDrawer 实现（Qt Widgets；GUI 线程独占）。

#include "split_drawer.h"

#include <QFormLayout>
#include <QFontDatabase>
#include <QLabel>
#include <QResizeEvent>
#include <QSettings>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

// ── 数据来源（Agent E 交付 shared/display/physical_status.h）────────
// 本文件只读 PhysicalStatus 字段（不解析 ERROR 文本行、不实现 counters）：
//   oled 行 → oledAddress/oledController/oledErrCount/oledOk（oledValid）
//   trx 行 → rssiDbm/channel（transportValid）
//   mem 行 → heapFree（memValid）
//   disp 行 → lastFrameId（displayValid）
//   sess 行 → sessionState/helloOk/pingOk（sessionValid）
//   mod 行 → physicalScene（modeValid）
// 每个字段组以 valid 标志区分「无数据」与「真实值」（任务书 §十一
// Unavailable 判定）：valid=false 一律显示占位符 "—"，不伪造数据。
#if defined(__has_include)
#  if __has_include("../shared/display/physical_status.h")
#    include "../shared/display/physical_status.h"
#    define ESPVIEW_SPLIT_DRAWER_HAVE_PHYSICAL_STATUS 1
#  elif __has_include("shared/display/physical_status.h")
#    include "shared/display/physical_status.h"
#    define ESPVIEW_SPLIT_DRAWER_HAVE_PHYSICAL_STATUS 1
#  elif __has_include("physical_status.h")
#    include "physical_status.h"
#    define ESPVIEW_SPLIT_DRAWER_HAVE_PHYSICAL_STATUS 1
#  endif
#endif

namespace espview {
namespace pc {

namespace {

const char* kPlaceholder = "—";

// 会话态数值（proto::SessionState）：0=Disconnected 1=Connecting
// 2=Handshake 3=Connected（见 physical_status.h 字段注释）。
const char* sessionStateName(uint8_t st) {
    switch (st) {
        case 0:
            return "Disconnected";
        case 1:
            return "Connecting";
        case 2:
            return "Handshake";
        case 3:
            return "Connected";
        default:
            return "Unknown";
    }
}

// 物理场景（PhysicalScene）：0=Diagnostics 1=Application；0xFF=未知。
const char* sceneName(uint8_t scene) {
    switch (scene) {
        case 0:
            return "Diagnostics";
        case 1:
            return "Application";
        default:
            return "Unknown";
    }
}

}  // namespace

QString SplitDrawer::t(const char* key) const {
    return QString::fromUtf8(trText(lang_, key));
}

SplitDrawer::SplitDrawer(QSettings* settings, QWidget* parent)
    : QFrame(parent), settings_(settings) {
    setObjectName(QStringLiteral("splitDrawer"));
    setFrameShape(QFrame::StyledPanel);
    // 宽度范围与 SplitState 常量一致：QSplitter 手柄只能在 [200, 560] 拖动，
    // 不侵入 Virtual 侧最小宽度。
    setMinimumWidth(SplitState::kMinDrawerWidth);
    setMaximumWidth(SplitState::kMaxDrawerWidth);

    buildUi();

    // 保存去抖：QSplitter 拖动期间 resizeEvent 高频触发，300ms 静默后再写
    // QSettings（避免每次拖动都写注册表/配置文件）。
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(300);
    connect(saveTimer_, &QTimer::timeout, this, &SplitDrawer::saveSettings);

    connect(closeButton_, &QToolButton::clicked, this, &SplitDrawer::close);

    lastWidth_ = width();
    loadSettings();  // 恢复上次 split 状态（宽度/可见性）
}

void SplitDrawer::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // 标题栏：面板标题 + 关闭按钮（dock-like 最小方案）。
    auto* titleBar = new QHBoxLayout;
    titleLabel_ = new QLabel(this);
    titleLabel_->setTextFormat(Qt::RichText);
    titleLabel_->setText(QStringLiteral("<b>%1</b>").arg(t("ESP32 Physical / Diagnostics")));
    titleBar->addWidget(titleLabel_);
    titleBar->addStretch(1);
    closeButton_ = new QToolButton(this);
    closeButton_->setText(QStringLiteral("✕"));
    closeButton_->setToolTip(t("Close drawer"));
    closeButton_->setAutoRaise(true);
    titleBar->addWidget(closeButton_);
    root->addLayout(titleBar);

    // 内容区：label 组，字段由外部数据更新（setPhysicalStatus）。
    auto* form = new QFormLayout;
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(3);
    form->setContentsMargins(0, 0, 0, 0);

    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    auto addRow = [this, form, fixed](const QString& name, QLabel*& valueLabel) {
        auto* nameLabel = new QLabel(name, this);
        fieldNameKeys_.emplace_back(nameLabel, nullptr);  // key 由调用方后续设置
        nameLabel->setEnabled(false);  // 灰色字段名，值突出显示
        valueLabel = new QLabel(QString::fromUtf8(kPlaceholder), this);
        valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        valueLabel->setFont(fixed);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(nameLabel, valueLabel);
    };

    addRow(t("RSSI"), rssiLabel_);
    fieldNameKeys_.back().second = "RSSI";
    addRow(t("Channel"), channelLabel_);
    fieldNameKeys_.back().second = "Channel";
    addRow(t("Heap"), heapLabel_);
    fieldNameKeys_.back().second = "Heap";
    addRow(t("Frame"), frameLabel_);
    fieldNameKeys_.back().second = "Frame";
    addRow(t("Errors"), errorsLabel_);
    fieldNameKeys_.back().second = "Errors";
    addRow(t("Transport"), transportLabel_);
    fieldNameKeys_.back().second = "Transport";
    addRow(t("OLED addr"), oledAddrLabel_);
    fieldNameKeys_.back().second = "OLED addr";
    addRow(t("Scene"), sceneLabel_);
    fieldNameKeys_.back().second = "Scene";
    addRow(t("Availability"), availabilityLabel_);
    fieldNameKeys_.back().second = "Availability";

    root->addLayout(form);
    root->addStretch(1);
}

void SplitDrawer::setPhysicalStatus(const espview::display::PhysicalStatus& status) {
#ifdef ESPVIEW_SPLIT_DRAWER_HAVE_PHYSICAL_STATUS
    lastStatus_ = status;
    haveStatus_ = true;
    renderStatus();
#else
    Q_UNUSED(status);
    // 头未就位（构建顺序依赖）：显示等待占位，不伪造任何数据。
    clearStatus();
    availabilityLabel_->setText(QStringLiteral("n/a (status link pending)"));
#endif
}

void SplitDrawer::clearStatus() {
    haveStatus_ = false;
    renderStatus();
}

void SplitDrawer::renderStatus() {
#ifdef ESPVIEW_SPLIT_DRAWER_HAVE_PHYSICAL_STATUS
    const QString placeholder = QString::fromUtf8(kPlaceholder);
    if (!haveStatus_) {
        rssiLabel_->setText(placeholder);
        channelLabel_->setText(placeholder);
        heapLabel_->setText(placeholder);
        frameLabel_->setText(placeholder);
        errorsLabel_->setText(placeholder);
        transportLabel_->setText(placeholder);
        oledAddrLabel_->setText(placeholder);
        sceneLabel_->setText(placeholder);
        availabilityLabel_->setText(placeholder);
        return;
    }
    const espview::display::PhysicalStatus& status = lastStatus_;

    // RSSI / CH（trx 行）
    rssiLabel_->setText(status.transportValid
                            ? QStringLiteral("%1 dBm").arg(status.rssiDbm)
                            : placeholder);
    channelLabel_->setText(status.transportValid ? QString::number(status.channel)
                                                 : placeholder);

    // Heap（mem 行）
    heapLabel_->setText(
        status.memValid
            ? QStringLiteral("%1 KB")
                  .arg(static_cast<double>(status.heapFree) / 1024.0, 0, 'f', 1)
            : placeholder);

    // Frame（disp 行）
    frameLabel_->setText(status.displayValid ? QString::number(status.lastFrameId)
                                             : placeholder);

    // Errors（oled 行 err=：OLED 错误计数）
    errorsLabel_->setText(status.oledValid ? QString::number(status.oledErrCount)
                                           : placeholder);

    // Transport（sess 行：会话态；对端 PING 存活见 Availability）
    transportLabel_->setText(
        status.sessionValid ? t(sessionStateName(status.sessionState))
                            : placeholder);

    // OLED address（oled 行：0x3C + 控制器名）
    const QString addr = QStringLiteral("0x%1").arg(
        QString::number(status.oledAddress, 16).toUpper().rightJustified(2, QLatin1Char('0')));
    oledAddrLabel_->setText(
        status.oledValid
            ? addr + QStringLiteral(" ") +
                  QString::fromUtf8(controllerCodeName(status.oledController))
            : placeholder);

    // Scene（mod 行）
    sceneLabel_->setText(status.modeValid ? t(sceneName(status.physicalScene))
                                          : placeholder);

    // Availability：OLED 健康（oledOk）+ 会话连接（sessionState==3）。
    // 无任何有效数据 → "—"；两者皆好 → OK；其一 → PARTIAL；否则 DEGRADED。
    if (!status.anyValid()) {
        availabilityLabel_->setText(placeholder);
    } else if (status.oledOk && status.sessionState == 3) {
        availabilityLabel_->setText(t("OK"));
    } else if (status.oledOk || status.sessionState == 3) {
        availabilityLabel_->setText(t("Partial"));
    } else {
        availabilityLabel_->setText(t("Degraded"));
    }
#else
    rssiLabel_->setText(QString());
    channelLabel_->setText(QString());
    heapLabel_->setText(QString());
    frameLabel_->setText(QString());
    errorsLabel_->setText(QString());
    transportLabel_->setText(QString());
    oledAddrLabel_->setText(QString());
    sceneLabel_->setText(QString());
    availabilityLabel_->setText(QStringLiteral("n/a (status link pending)"));
#endif
}

void SplitDrawer::setUiLanguage(int lang) {
    lang_ = static_cast<UiLang>(lang);
    if (titleLabel_ != nullptr) {
        titleLabel_->setText(
            QStringLiteral("<b>%1</b>").arg(t("ESP32 Physical / Diagnostics")));
    }
    if (closeButton_ != nullptr) {
        closeButton_->setToolTip(t("Close drawer"));
    }
    for (const auto& entry : fieldNameKeys_) {
        if (entry.first != nullptr && entry.second != nullptr) {
            entry.first->setText(t(entry.second));
        }
    }
    renderStatus();  // 值区域状态文案（OK/PARTIAL/DEGRADED）同步重刷
}

void SplitDrawer::setVisible(bool visible) {
    QFrame::setVisible(visible);
    state_.setDrawerVisible(visible);
    emit visibilityChanged(visible);
    scheduleSave();
}

void SplitDrawer::setDrawerWidth(int width) {
    applyWidth(width);
}

void SplitDrawer::applyWidth(int width) {
    state_.setDrawerWidth(width);  // 夹取 [200, 560]
    const int w = state_.drawerWidth();
    if (qobject_cast<QSplitter*>(parentWidget()) != nullptr) {
        // QSplitter 模式：宽度由手柄驱动，min/max 已在构造时设置；绝不
        // setFixedWidth（会锁死手柄）。
        setMinimumWidth(SplitState::kMinDrawerWidth);
        setMaximumWidth(SplitState::kMaxDrawerWidth);
    } else {
        // 固定宽度槽模式：独立嵌入时直接锁宽。
        setFixedWidth(w);
    }
    if (w != lastWidth_) {
        lastWidth_ = w;
        emit widthChanged(w);
    }
    scheduleSave();
}

void SplitDrawer::applyState(const SplitState& state) {
    state_ = state;
    applyWidth(state_.drawerWidth());
    QFrame::setVisible(state_.drawerVisible());
    emit visibilityChanged(state_.drawerVisible());
    scheduleSave();
}

void SplitDrawer::loadSettings() {
    if (settings_ == nullptr) {
        return;
    }
    SplitState::SettingsMap map;
    map.emplace_back(
        SplitState::kKeyDrawerVisible,
        settings_
            ->value(QString::fromUtf8(SplitState::kKeyDrawerVisible), QStringLiteral("0"))
            .toString()
            .toStdString());
    map.emplace_back(
        SplitState::kKeyDrawerWidth,
        settings_
            ->value(QString::fromUtf8(SplitState::kKeyDrawerWidth),
                    QString::number(SplitState::kDefaultDrawerWidth))
            .toString()
            .toStdString());
    state_.fromSettingsMap(map);
    applyWidth(state_.drawerWidth());
    setVisible(state_.drawerVisible());
}

void SplitDrawer::saveSettings() {
    if (settings_ == nullptr) {
        return;
    }
    for (const auto& kv : state_.toSettingsMap()) {
        settings_->setValue(QString::fromUtf8(kv.first.c_str()),
                            QString::fromUtf8(kv.second.c_str()));
    }
}

void SplitDrawer::resizeEvent(QResizeEvent* event) {
    QFrame::resizeEvent(event);
    const int w = width();
    if (w == lastWidth_) {
        return;
    }
    lastWidth_ = w;
    state_.setDrawerWidth(w);  // 夹取（正常情况 min/max 已保证范围）
    emit widthChanged(w);
    scheduleSave();
}

void SplitDrawer::scheduleSave() {
    if (settings_ != nullptr && saveTimer_ != nullptr) {
        saveTimer_->start();
    }
}

QSize SplitDrawer::sizeHint() const {
    return QSize(SplitState::kDefaultDrawerWidth, 260);
}

}  // namespace pc
}  // namespace espview
