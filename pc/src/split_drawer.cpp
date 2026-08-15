// ESPView M7-C4 — SplitDrawer 实现（Qt Widgets；GUI 线程独占）。
//
// 数据来源（Agent E 交付 shared/display/physical_status.h）——本文件只读
// PhysicalStatus 字段（不解析 ERROR 文本行、不实现 counters、不复制
// RuntimeStats）：
//   oled 行 → oledAddress/oledController/oledErrCount/oledOk（oledValid）
//   trx 行 → rssiDbm/channel（transportValid）
//   mem 行 → heapFree/heapMinFree（memValid）
//   disp 行 → lastFrameId/fpsHundredths（displayValid）
//   sess 行 → sessionState/helloOk/pingOk（sessionValid）
//   mod 行 → physicalScene（modeValid）
// 每个字段组以 valid 标志区分「无数据」与「真实值」（任务书 §十一
// Unavailable 判定）：valid=false 一律显示占位符 "—"，不伪造数据。
// SSID / IP / Resolution 遥测不提供 → 安全占位 "—"（绝不显示密码）。
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

#include "split_drawer.h"

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QPalette>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

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

// 状态横幅配色（RGB，浅色/深色主题均可读）。
const QColor kColorOk(0x2E, 0x7D, 0x32);
const QColor kColorWarn(0xE6, 0x51, 0x00);
const QColor kColorError(0xC6, 0x28, 0x28);
const QColor kColorMuted(0x75, 0x75, 0x75);

// 值变化才更新 label（文本 + 配色），避免 5Hz 无谓样式重算。
void setLabelText(QLabel* label, const QString& text, const QColor& color) {
    if (label->text() != text) {
        label->setText(text);
        label->setStyleSheet(QStringLiteral("color: rgb(%1,%2,%3);")
                                 .arg(color.red())
                                 .arg(color.green())
                                 .arg(color.blue()));
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

    buildUi();

    // 最小/最大宽度：Qt 层产品约束 [200,480]（SplitState 模型 [200,560]
    // 超集保持不变；minWidgetWidth_ 由布局 QFontMetrics 推导，DPI 自动缩放）。
    setMinimumWidth(minWidgetWidth_);
    setMaximumWidth(kMaxDrawerWidth);

    // 保存去抖：QSplitter 拖动期间 resizeEvent 高频触发，300ms 静默后再写
    // QSettings（避免每次拖动都写注册表/配置文件）。
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(300);
    connect(saveTimer_, &QTimer::timeout, this, &SplitDrawer::saveSettings);

    // 合并渲染节流：连续高频输入时至少间隔 200ms 才真正写 label（≤5Hz，
    // 无 50Hz 重绘；setPhysicalStatus 只存快照 + 调度，不做重量级布局）。
    renderTimer_ = new QTimer(this);
    renderTimer_->setSingleShot(true);
    renderTimer_->setInterval(kMinRenderIntervalMs);
    connect(renderTimer_, &QTimer::timeout, this, &SplitDrawer::onRenderTimer);

    // stale/时间戳检查：1Hz，只在值变化时改 label。
    staleTimer_ = new QTimer(this);
    staleTimer_->setInterval(kStaleCheckMs);
    connect(staleTimer_, &QTimer::timeout, this, &SplitDrawer::onStaleTick);
    staleTimer_->start();

    connect(collapseButton_, &QToolButton::clicked, this, &SplitDrawer::close);

    renderClock_.start();
    lastWidth_ = width();
    loadSettings();  // 恢复上次 split 状态（宽度/可见性）
    renderStatus();  // 初始即显示 No data / Unavailable，不留空白占位
}

void SplitDrawer::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // 标题栏：面板标题 + 收起按钮（✕，dock-like 最小方案）。
    auto* titleBar = new QHBoxLayout;
    titleLabel_ = new QLabel(this);
    titleLabel_->setTextFormat(Qt::RichText);
    titleLabel_->setText(
        QStringLiteral("<b>%1</b>").arg(t("ESP32 Physical / Diagnostics")));
    titleBar->addWidget(titleLabel_);
    titleBar->addStretch(1);
    collapseButton_ = new QToolButton(this);
    collapseButton_->setText(QStringLiteral("✕"));
    collapseButton_->setToolTip(t("Close drawer"));
    collapseButton_->setAutoRaise(true);
    titleBar->addWidget(collapseButton_);
    root->addLayout(titleBar);

    // 内容区：QScrollArea 纵向按需、横向滚动条永久关闭——窄宽度下
    // QFormLayout 自动换行（label minimumWidth=0 + wordWrap），不会产生
    // 横向滚动条风暴，布局不崩。
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    content->setMinimumWidth(0);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(4);
    contentLayout_ = contentLayout;  // M7-D2：addExternalWidget 插入点

    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QFont sectionFont = font();
    sectionFont.setBold(true);

    // ---- Physical Display 分区 ----
    auto* formPhysical = new QFormLayout;
    configureForm(formPhysical);
    addSection(contentLayout, "Physical Display", sectionFont);
    addRow(formPhysical, "Controller", fixed, controllerLabel_);
    addRow(formPhysical, "Resolution", fixed, resolutionLabel_);
    addRow(formPhysical, "I2C", fixed, i2cLabel_);
    addRow(formPhysical, "State", fixed, physStateLabel_);
    addRow(formPhysical, "Scene", fixed, sceneLabel_);
    addRow(formPhysical, "Last Flush", fixed, lastFlushLabel_);
    addRow(formPhysical, "Errors", fixed, errorsLabel_);
    contentLayout->addLayout(formPhysical);

    // ---- Wi-Fi / TCP 分区 ----
    auto* formWifi = new QFormLayout;
    configureForm(formWifi);
    addSection(contentLayout, "Wi-Fi / TCP", sectionFont);
    addRow(formWifi, "SSID", fixed, ssidLabel_);
    addRow(formWifi, "RSSI", fixed, rssiLabel_);
    addRow(formWifi, "Channel", fixed, channelLabel_);
    addRow(formWifi, "IP", fixed, ipLabel_);
    addRow(formWifi, "TCP status", fixed, tcpStatusLabel_);
    addRow(formWifi, "Session", fixed, sessionLabel_);
    addRow(formWifi, "Frame", fixed, frameLabel_);
    addRow(formWifi, "Heap", fixed, heapLabel_);
    contentLayout->addLayout(formWifi);

    // ---- Session / State 分区 ----
    auto* formSession = new QFormLayout;
    configureForm(formSession);
    addSection(contentLayout, "Session / State", sectionFont);
    addRow(formSession, "State", fixed, stateLabel_);
    addRow(formSession, "Last refresh", fixed, lastUpdateLabel_);
    contentLayout->addLayout(formSession);

    staleWarning_ = new QLabel(content);
    staleWarning_->setWordWrap(true);
    staleWarning_->setMinimumWidth(0);
    staleWarning_->setVisible(false);
    contentLayout->addWidget(staleWarning_);

    contentLayout->addStretch(1);
    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    // DPI-aware 最小宽度：由布局 QFontMetrics minimumSize 推导（Qt 逻辑像素
    // 随 DPI 自动缩放），不硬编码像素阈值做布局判断。
    const int layoutMin = contentLayout->minimumSize().width() + 12;
    minWidgetWidth_ = qBound(kMinDrawerWidth, layoutMin, kMaxDrawerWidth);
}

// M7-D2：外部预览 widget 插入抽屉顶部（分区标题 + widget）。
void SplitDrawer::addExternalWidget(const char* sectionKey, QWidget* widget) {
    if (sectionKey == nullptr || widget == nullptr || contentLayout_ == nullptr) {
        return;
    }
    QFont sectionFont = font();
    sectionFont.setBold(true);
    auto* header = new QLabel(t(sectionKey), this);
    header->setFont(sectionFont);
    sectionHeaderKeys_.emplace_back(header, sectionKey);
    const int insertAt = 0;  // 顶部（诊断分区之上）
    contentLayout_->insertWidget(insertAt, header);
    contentLayout_->insertWidget(insertAt + 1, widget);
}

void SplitDrawer::configureForm(QFormLayout* form) {
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(3);
    form->setContentsMargins(0, 0, 0, 0);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
}

void SplitDrawer::addSection(QVBoxLayout* layout, const char* key,
                             const QFont& sectionFont) {
    auto* header = new QLabel(t(key), this);
    header->setFont(sectionFont);
    header->setForegroundRole(QPalette::PlaceholderText);
    header->setWordWrap(true);
    header->setMinimumWidth(0);
    sectionHeaderKeys_.emplace_back(header, key);
    layout->addWidget(header);
}

void SplitDrawer::addRow(QFormLayout* form, const char* key,
                         const QFont& valueFont, QLabel*& valueLabel) {
    auto* nameLabel = new QLabel(t(key), this);
    nameLabel->setEnabled(false);  // 灰色字段名，值突出显示
    nameLabel->setWordWrap(true);
    nameLabel->setMinimumWidth(0);
    fieldNameKeys_.emplace_back(nameLabel, key);
    valueLabel = new QLabel(QString::fromUtf8(kPlaceholder), this);
    valueLabel->setWordWrap(true);
    valueLabel->setMinimumWidth(0);
    valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    valueLabel->setFont(valueFont);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(nameLabel, valueLabel);
}

void SplitDrawer::setPhysicalStatus(const espview::display::PhysicalStatus& status) {
#ifdef ESPVIEW_SPLIT_DRAWER_HAVE_PHYSICAL_STATUS
    lastStatus_ = status;
    haveStatus_ = true;
    disconnected_ = false;  // 有新鲜数据流 → 会话在线
    if (status.sessionState == 3) {
        everConnected_ = true;  // 曾 Connected → 再次 Connecting 显示 Reconnecting
    }
    lastRefreshMs_.restart();
    scheduleRender();   // 只调度（≥200ms 节流），不在此做重量级布局
    updateStaleUi();    // 立即刷新 “Just now”/stale（仅值变化时改 label）
#else
    Q_UNUSED(status);
    clearStatus();  // 头未就位（构建顺序依赖）：显示等待占位，不伪造任何数据。
#endif
}

void SplitDrawer::clearStatus() {
    haveStatus_ = false;
    disconnected_ = true;  // main.cpp 在会话失联时调用本方法 → 断线语义
    lastStatus_ = espview::display::PhysicalStatus();
    lastUpdateShown_.clear();
    staleShown_ = false;
    if (staleWarning_ != nullptr) {
        staleWarning_->setVisible(false);
    }
    renderStatus();
}

void SplitDrawer::renderStatus() {
#ifdef ESPVIEW_SPLIT_DRAWER_HAVE_PHYSICAL_STATUS
    const QString placeholder = QString::fromUtf8(kPlaceholder);
    const espview::display::PhysicalStatus& status = lastStatus_;
    const bool have = haveStatus_;

    // ---- Physical Display ----
    controllerLabel_->setText(have && status.oledValid
                                  ? QString::fromUtf8(controllerCodeName(status.oledController))
                                  : placeholder);
    resolutionLabel_->setText(placeholder);  // 遥测不提供分辨率；不伪造
    i2cLabel_->setText(have && status.oledValid
                           ? QStringLiteral("0x%1").arg(QString::number(status.oledAddress, 16)
                                                            .toUpper()
                                                            .rightJustified(2, QLatin1Char('0')))
                           : placeholder);
    // 物理显示 State：Ready / Degraded / Unavailable（oled 遥测驱动）。
    if (!have || !status.anyValid() || !status.oledValid) {
        setLabelText(physStateLabel_, t("Unavailable"), kColorMuted);
    } else if (status.oledOk) {
        setLabelText(physStateLabel_, t("Ready"), kColorOk);
    } else {
        setLabelText(physStateLabel_, t("Degraded"), kColorWarn);
    }
    sceneLabel_->setText(have && status.modeValid ? t(sceneName(status.physicalScene))
                                                  : placeholder);
    lastFlushLabel_->setText(have && status.displayValid
                                 ? QStringLiteral("#%1").arg(status.lastFrameId)
                                 : placeholder);
    errorsLabel_->setText(have && status.oledValid ? QString::number(status.oledErrCount)
                                                   : placeholder);

    // ---- Wi-Fi / TCP ----
    ssidLabel_->setText(placeholder);  // 遥测不提供 SSID；安全占位，绝不显示密码
    rssiLabel_->setText(have && status.transportValid
                            ? QStringLiteral("%1 dBm").arg(status.rssiDbm)
                            : placeholder);
    channelLabel_->setText(have && status.transportValid ? QString::number(status.channel)
                                                         : placeholder);
    ipLabel_->setText(placeholder);  // 遥测不提供 IP；安全占位
    tcpStatusLabel_->setText(have && status.sessionValid
                                 ? t(sessionStateName(status.sessionState))
                                 : placeholder);
    sessionLabel_->setText(have && status.sessionValid
                               ? QStringLiteral("HELLO %1 · PING %2")
                                     .arg(status.helloOk ? QStringLiteral("✓")
                                                         : QStringLiteral("✗"))
                                     .arg(status.pingOk ? QStringLiteral("✓")
                                                        : QStringLiteral("✗"))
                               : placeholder);
    frameLabel_->setText(have && status.displayValid
                             ? QStringLiteral("#%1 · %2 fps")
                                   .arg(status.lastFrameId)
                                   .arg(status.fpsHundredths / 100.0, 0, 'f', 2)
                             : placeholder);
    heapLabel_->setText(have && status.memValid
                            ? QStringLiteral("%1 KB free · min %2 KB")
                                  .arg(static_cast<double>(status.heapFree) / 1024.0, 0, 'f', 0)
                                  .arg(static_cast<double>(status.heapMinFree) / 1024.0, 0, 'f', 0)
                            : placeholder);

    // ---- Session / State ----
    QString stateText;
    QColor stateColor = kColorMuted;
    if (disconnected_) {
        stateText = t("Disconnected");
        stateColor = kColorError;
    } else if (!have) {
        stateText = t("No data");
    } else {
        switch (status.sessionState) {
            case 0:
                stateText = t("Disconnected");
                stateColor = kColorWarn;
                break;
            case 1:
                stateText = everConnected_ ? t("Reconnecting") : t("Connecting");
                stateColor = kColorWarn;
                break;
            case 2:
                stateText = t("Handshake");
                stateColor = kColorWarn;
                break;
            case 3:
                stateText = t("Connected");
                stateColor = kColorOk;
                break;
            default:
                stateText = t("Unknown");
                break;
        }
    }
    setLabelText(stateLabel_, stateText, stateColor);

    updateStaleUi();  // “Updated N s ago” + stale 警告（1Hz 同样调用）
#else
    // 头未就位（构建顺序依赖）：全占位，不伪造任何数据。
    controllerLabel_->setText(QString());
    resolutionLabel_->setText(QString());
    i2cLabel_->setText(QString());
    physStateLabel_->setText(QString());
    sceneLabel_->setText(QString());
    lastFlushLabel_->setText(QString());
    errorsLabel_->setText(QString());
    ssidLabel_->setText(QString());
    rssiLabel_->setText(QString());
    channelLabel_->setText(QString());
    ipLabel_->setText(QString());
    tcpStatusLabel_->setText(QString());
    sessionLabel_->setText(QString());
    frameLabel_->setText(QString());
    heapLabel_->setText(QString());
    stateLabel_->setText(QString());
    lastUpdateLabel_->setText(QString());
#endif
}

void SplitDrawer::updateStaleUi() {
    if (lastUpdateLabel_ == nullptr || staleWarning_ == nullptr) {
        return;
    }
    const QString placeholder = QString::fromUtf8(kPlaceholder);
    if (!haveStatus_) {
        if (lastUpdateShown_ != placeholder) {
            lastUpdateShown_ = placeholder;
            lastUpdateLabel_->setText(placeholder);
        }
        if (staleShown_) {
            staleShown_ = false;
            staleWarning_->setVisible(false);
        }
        return;
    }
    const qint64 elapsedMs = lastRefreshMs_.elapsed();
    const int secs = static_cast<int>(elapsedMs / 1000);
    const QString updateText =
        (secs <= 0) ? t("Just now") : t("Updated %1 s ago").arg(secs);
    if (updateText != lastUpdateShown_) {
        lastUpdateShown_ = updateText;
        lastUpdateLabel_->setText(updateText);
    }
    // stale：数据超过 kStaleThresholdMs（5s）未刷新 → 显示 stale 标记。
    const bool stale = elapsedMs > kStaleThresholdMs;
    if (stale != staleShown_) {
        staleShown_ = stale;
        staleWarning_->setVisible(stale);
        if (stale) {
            staleWarning_->setText(t("Stale — last update %1 s ago").arg(secs));
        }
    } else if (stale) {
        const QString staleText = t("Stale — last update %1 s ago").arg(secs);
        if (staleText != staleWarning_->text()) {
            staleWarning_->setText(staleText);
        }
    }
}

void SplitDrawer::scheduleRender() {
    if (renderPending_) {
        return;
    }
    renderPending_ = true;
    const qint64 elapsed = renderClock_.elapsed();
    const qint64 wait =
        std::max<qint64>(0, qint64(kMinRenderIntervalMs) - elapsed);
    renderTimer_->start(static_cast<int>(wait));
}

void SplitDrawer::onRenderTimer() {
    renderPending_ = false;
    renderClock_.restart();
    renderStatus();
}

void SplitDrawer::onStaleTick() {
    updateStaleUi();
}

void SplitDrawer::setUiLanguage(int lang) {
    lang_ = static_cast<UiLang>(lang);
    if (titleLabel_ != nullptr) {
        titleLabel_->setText(
            QStringLiteral("<b>%1</b>").arg(t("ESP32 Physical / Diagnostics")));
    }
    if (collapseButton_ != nullptr) {
        collapseButton_->setToolTip(t("Close drawer"));
    }
    for (const auto& entry : sectionHeaderKeys_) {
        if (entry.first != nullptr && entry.second != nullptr) {
            entry.first->setText(t(entry.second));
        }
    }
    for (const auto& entry : fieldNameKeys_) {
        if (entry.first != nullptr && entry.second != nullptr) {
            entry.first->setText(t(entry.second));
        }
    }
    // 值区域/状态文案（Ready/Degraded/No data/Reconnecting/stale 等）重刷；
    // 不触碰连接与帧。
    lastUpdateShown_.clear();
    staleShown_ = false;
    if (staleWarning_ != nullptr) {
        staleWarning_->setVisible(false);
    }
    renderStatus();
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
    const int w = qBound(kMinDrawerWidth, width, kMaxDrawerWidth);
    state_.setDrawerWidth(w);  // 模型 [200,560] 超集内夹取，值不变
    if (auto* splitter = qobject_cast<QSplitter*>(parentWidget())) {
        // QSplitter 模式：宽度由手柄驱动；min/max 约束范围，绝不
        // setFixedWidth（会锁死手柄）。显式 setSizes 让宽度槽/恢复生效。
        setMinimumWidth(minWidgetWidth_);
        setMaximumWidth(kMaxDrawerWidth);
        QList<int> sizes = splitter->sizes();
        if (sizes.size() == 2) {
            const int total = sizes[0] + sizes[1];
            sizes[1] = w;
            sizes[0] = qMax(0, total - w);
            splitter->setSizes(sizes);
        }
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
    state_.setDrawerWidth(w);  // 夹取（Qt 层 min/max 已保证在 [200,480]）
    emit widthChanged(w);
    scheduleSave();
}

void SplitDrawer::showEvent(QShowEvent* event) {
    QFrame::showEvent(event);
    configureSplitterHost();
}

void SplitDrawer::configureSplitterHost() {
    auto* splitter = qobject_cast<QSplitter*>(parentWidget());
    if (splitter == nullptr) {
        return;
    }
    const int idx = splitter->indexOf(this);
    if (idx >= 0) {
        // 收起只能走 ✕/open()；拖拽分隔条不会把 drawer 压到 0 以下。
        splitter->setCollapsible(idx, false);
    }
    // 恢复上次宽度：QSplitter 在隐藏/显示周期可能重置 pane 大小。
    QList<int> sizes = splitter->sizes();
    if (sizes.size() == 2) {
        const int total = sizes[0] + sizes[1];
        const int w = qBound(minWidgetWidth_, state_.drawerWidth(), kMaxDrawerWidth);
        sizes[1] = w;
        sizes[0] = qMax(0, total - w);
        splitter->setSizes(sizes);
    }
}

void SplitDrawer::scheduleSave() {
    if (settings_ != nullptr && saveTimer_ != nullptr) {
        saveTimer_->start();
    }
}

QSize SplitDrawer::sizeHint() const {
    return QSize(kDefaultDrawerWidth, 260);
}

}  // namespace pc
}  // namespace espview
