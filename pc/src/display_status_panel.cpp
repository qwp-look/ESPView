// ESPView M7-C3 — DisplayStatusPanel 实现（见 display_status_panel.h）。
// 纯展示组件：只读消费 setWorkerStatus / setStats / setPhysicalStatus /
// setModeState 喂入的状态，不做协议解析（物理遥测解析在 shared/display
// physical_status.cpp，由接线方经 diagAdded 调用后喂入本面板）。

#include "display_status_panel.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QVBoxLayout>

#include <cstdio>

namespace espview {
namespace pc {

namespace {

// 任务书 §十一 状态着色（Active 绿 / Degraded 橙 / Disabled 灰 / Unavailable 红）。
const char* stateColor(PanelState s) {
    switch (s) {
        case PanelState::kActive:
            return "#1b5e20";
        case PanelState::kDegraded:
            return "#e65100";
        case PanelState::kDisabled:
            return "#757575";
        case PanelState::kUnavailable:
            return "#b71c1c";
    }
    return "#212121";
}

QString fmtU64(uint64_t v) {
    return QString::number(v);
}

}  // namespace

QString DisplayStatusPanel::t(const char* key) const {
    return QString::fromUtf8(trText(lang_, key));
}

DisplayStatusPanel::DisplayStatusPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // ---- Transport（TCP/UART + Worker 状态 + ESP32 RSSI/Ch）----
    QGroupBox* tg = addGroup(t("Transport"));
    {
        auto* grid = new QGridLayout(tg);
        int row = 0;
        transport_.kind = addRow(grid, row++, t("Kind"));
        transport_.state = addRow(grid, row++, t("State"));
        transport_.detail = addRow(grid, row++, t("Detail"));
        transport_.reconnect = addRow(grid, row++, t("Reconnects"));
        transport_.rssiCh = addRow(grid, row++, t("RSSI / Ch"));
    }
    root->addWidget(tg);

    // ---- Session（CONNECTED/DISCONNECTED/RECONNECTING + 对端 hello/ping）----
    QGroupBox* sg = addGroup(t("Session"));
    {
        auto* grid = new QGridLayout(sg);
        int row = 0;
        session_.state = addRow(grid, row++, t("State"));
        session_.hello = addRow(grid, row++, t("Peer HELLO"));
        session_.ping = addRow(grid, row++, t("Peer PING"));
        session_.rtt = addRow(grid, row++, t("RTT"));
    }
    root->addWidget(sg);

    // ---- Display Mode（MIRROR 等路由模式）----
    QGroupBox* mg = addGroup(t("Display Mode"));
    {
        auto* grid = new QGridLayout(mg);
        int row = 0;
        displayMode_.mode = addRow(grid, row++, t("Mode"));
    }
    root->addWidget(mg);

    // ---- Virtual（Application + ACTIVE）----
    QGroupBox* vg = addGroup(t("Virtual"));
    {
        auto* grid = new QGridLayout(vg);
        int row = 0;
        virtual_.app = addRow(grid, row++, t("Application"));
        virtual_.resolution = addRow(grid, row++, t("Resolution"));
        virtual_.fps = addRow(grid, row++, t("FPS"));
        virtual_.frames = addRow(grid, row++, t("Committed / discarded"));
    }
    root->addWidget(vg);

    // ---- Physical（Application/Diagnostics + READY/DEGRADED/UNAVAILABLE/DISABLED）----
    QGroupBox* pg = addGroup(t("Physical"));
    {
        auto* grid = new QGridLayout(pg);
        int row = 0;
        physical_.scene = addRow(grid, row++, t("Scene"));
        physical_.state = addRow(grid, row++, t("State"));
        physical_.oled = addRow(grid, row++, t("OLED"));
        physical_.errors = addRow(grid, row++, t("OLED errors"));
        physical_.heap = addRow(grid, row++, t("HEAP free / largest / min"));
    }
    root->addWidget(pg);

    // ---- Router（CONNECTED/DEGRADED）----
    QGroupBox* rg = addGroup(t("Router"));
    {
        auto* grid = new QGridLayout(rg);
        int row = 0;
        router_.state = addRow(grid, row++, t("State"));
    }
    root->addWidget(rg);

    // ---- Errors（OLED + 协议 + 丢帧 + 背压）----
    QGroupBox* eg = addGroup(t("Errors"));
    {
        auto* grid = new QGridLayout(eg);
        int row = 0;
        errors_.oled = addRow(grid, row++, t("OLED"));
        errors_.protocol = addRow(grid, row++, t("Protocol decode/CRC/seq"));
        errors_.dropped = addRow(grid, row++, t("Dropped frames"));
        errors_.queue = addRow(grid, row++, t("Queue full"));
    }
    root->addWidget(eg);

    // ---- FULL resync（DONE/PENDING）----
    QGroupBox* fg = addGroup(t("FULL resync"));
    {
        auto* grid = new QGridLayout(fg);
        int row = 0;
        resync_.state = addRow(grid, row++, t("State"));
    }
    root->addWidget(fg);

    refreshAll();
}

QGroupBox* DisplayStatusPanel::addGroup(const QString& title) {
    auto* box = new QGroupBox(title, this);
    box->setStyleSheet(QStringLiteral("QGroupBox { font-weight: 600; }"));
    return box;
}

DisplayStatusPanel::Row DisplayStatusPanel::addRow(QGridLayout* grid, int row,
                                                   const QString& key) {
    Row r;
    r.key = new QLabel(key, this);
    r.key->setStyleSheet(QStringLiteral("color: #546e7a;"));
    r.value = new QLabel(QStringLiteral("N/A"), this);
    r.value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(r.key, row, 0);
    grid->addWidget(r.value, row, 1);
    return r;
}

void DisplayStatusPanel::setValue(const Row& row, const QString& value,
                                  PanelState state) {
    if (row.value == nullptr) {
        return;
    }
    row.value->setText(value);
    row.value->setStyleSheet(
        QStringLiteral("color: %1; font-weight: 600;").arg(QLatin1String(stateColor(state))));
}

// ---- 状态文本映射 ----

QString DisplayStatusPanel::workerStateText(int status) const {
    switch (status) {
        case 0:
            return t("Disconnected");
        case 1:
            return t("Connecting");
        case 2:
            return t("Connected");
        case 3:
            return t("Error");
    }
    return t("Unknown");
}

QString DisplayStatusPanel::sessionStateText(uint8_t state,
                                             uint64_t reconnectCount) const {
    // proto::SessionState：0=Disconnected 1=Connecting 2=Handshake 3=Connected。
    switch (state) {
        case 0:
            return t("Disconnected");
        case 1:
            return reconnectCount > 0 ? t("Reconnecting") : t("Connecting");
        case 2:
            return t("Handshake");
        case 3:
            return t("Connected");
    }
    return t("Unknown");
}

QString DisplayStatusPanel::modeName(int mode) const {
    switch (mode) {
        case 0:
            return t("Virtual Only");
        case 1:
            return t("Physical Only");
        case 2:
            return t("Mirror");
        case 3:
            return t("Split");
    }
    return t("N/A");
}

QString DisplayStatusPanel::sceneName(uint8_t scene) const {
    switch (scene) {
        case 0:
            return t("Diagnostics");
        case 1:
            return t("Application");
        case 0xFF:
            return t("N/A");
    }
    return t("Unknown");
}

QString DisplayStatusPanel::routerStateText(uint8_t state) const {
    // RouterState：0=Idle 1=Switching 2=Connected 3=Degraded；0xFF=未知。
    switch (state) {
        case 0:
            return t("Idle");
        case 1:
            return t("Switching");
        case 2:
            return t("Connected");
        case 3:
            return t("Degraded");
        case 0xFF:
            return t("Unavailable");
    }
    return t("Unknown");
}

QString DisplayStatusPanel::stateText(PanelState s) const {
    switch (s) {
        case PanelState::kActive:
            return t("Active");
        case PanelState::kDegraded:
            return t("Degraded");
        case PanelState::kDisabled:
            return t("Disabled");
        case PanelState::kUnavailable:
            return t("Unavailable");
    }
    return t("Unknown");
}

// Virtual：Worker 会话断开 → UNAVAILABLE；PHYSICAL ONLY → DISABLED；
// 否则 ACTIVE（Split/Mirror/VirtualOnly 下 virtual 都参与路由）。
PanelState DisplayStatusPanel::virtualState() const {
    if (!hasStats_ || workerStatus_ != 2 || stats_.sessionState != 3) {
        return PanelState::kUnavailable;
    }
    if (mode_ == 1) {
        return PanelState::kDisabled;
    }
    return PanelState::kActive;
}

// Physical：无 OLED 遥测 → UNAVAILABLE；VIRTUAL ONLY → DISABLED；
// OLED 不健康 / Router DEGRADED → DEGRADED；Router CONNECTED → READY(Active)。
PanelState DisplayStatusPanel::physicalState() const {
    if (!phys_.oledValid) {
        return PanelState::kUnavailable;
    }
    if (mode_ == 0) {
        return PanelState::kDisabled;
    }
    if (routerState_ == 0xFF) {
        return PanelState::kUnavailable;  // 尚无 mod 行，无法确认路由
    }
    if (!phys_.oledOk || phys_.oledErrCount > 0) {
        return PanelState::kDegraded;
    }
    if (routerState_ == 3) {
        return PanelState::kDegraded;
    }
    if (routerState_ == 2) {
        return PanelState::kActive;  // READY
    }
    return PanelState::kUnavailable;
}

// ---- 槽 ----

void DisplayStatusPanel::setWorkerStatus(espview::pc::WorkerStatus status,
                                         const QString& text) {
    workerStatus_ = static_cast<int>(status);
    workerText_ = text;
    refreshTransport();
    refreshSession();
    refreshVirtual();
}

void DisplayStatusPanel::setStats(const espview::pc::WorkerStats& stats) {
    stats_ = stats;
    hasStats_ = true;
    refreshAll();
}

void DisplayStatusPanel::setPhysicalStatus(
    const espview::display::PhysicalStatus& ps) {
    phys_ = ps;
    refreshTransport();  // RSSI / Ch
    refreshSession();    // hello / ping
    refreshPhysical();
    refreshErrors();
}

void DisplayStatusPanel::setModeState(int mode, int routerState,
                                      bool fullResyncPending) {
    mode_ = (mode >= 0 && mode <= 3) ? mode : mode_;
    routerState_ = static_cast<int>(routerState);
    fullResyncPending_ = fullResyncPending;
    refreshMode();
    refreshVirtual();
    refreshPhysical();
    refreshRouter();
    refreshFullResync();
}

void DisplayStatusPanel::setUiLanguage(int lang) {
    lang_ = static_cast<UiLang>(lang);
    refreshAll();
}

// ---- 刷新 ----

void DisplayStatusPanel::refreshAll() {
    refreshTransport();
    refreshSession();
    refreshMode();
    refreshVirtual();
    refreshPhysical();
    refreshRouter();
    refreshErrors();
    refreshFullResync();
}

void DisplayStatusPanel::refreshTransport() {
    const bool k = hasStats_;
    setValue(transport_.kind,
             k ? (stats_.transportKind == 1 ? t("TCP") : t("UART")) : t("N/A"),
             k ? PanelState::kActive : PanelState::kUnavailable);
    PanelState st = PanelState::kUnavailable;
    switch (workerStatus_) {
        case 2:
            st = PanelState::kActive;
            break;
        case 1:
            st = PanelState::kDegraded;
            break;
        case 0:
        case 3:
            st = PanelState::kUnavailable;
            break;
    }
    setValue(transport_.state, workerStateText(workerStatus_), st);
    setValue(transport_.detail,
             workerText_.isEmpty() ? t("N/A") : t(workerText_.toUtf8().constData()),
             workerStatus_ == 2 ? PanelState::kActive : PanelState::kUnavailable);
    setValue(transport_.reconnect,
             k ? fmtU64(stats_.reconnectCount) : t("N/A"),
             k ? PanelState::kActive : PanelState::kUnavailable);
    if (phys_.transportValid) {
        const QString v =
            t("%1 dBm / ch %2")
                .arg(phys_.rssiDbm)
                .arg(phys_.channel);
        const PanelState rs =
            (phys_.rssiDbm >= -90) ? PanelState::kActive : PanelState::kDegraded;
        setValue(transport_.rssiCh, v, rs);
    } else {
        setValue(transport_.rssiCh, t("N/A"), PanelState::kUnavailable);
    }
}

void DisplayStatusPanel::refreshSession() {
    const uint8_t s = hasStats_ ? stats_.sessionState : 0;
    const uint64_t rc = hasStats_ ? stats_.reconnectCount : 0;
    PanelState st = PanelState::kUnavailable;
    if (hasStats_) {
        switch (s) {
            case 3:
                st = PanelState::kActive;
                break;
            case 1:
            case 2:
                st = PanelState::kDegraded;
                break;
            default:
                st = PanelState::kUnavailable;
                break;
        }
    }
    setValue(session_.state, sessionStateText(s, rc), st);

    if (phys_.sessionValid) {
        setValue(session_.hello, phys_.helloOk ? t("OK") : t("NO"),
                 phys_.helloOk ? PanelState::kActive : PanelState::kDegraded);
        setValue(session_.ping, phys_.pingOk ? t("OK") : t("NO"),
                 phys_.pingOk ? PanelState::kActive : PanelState::kDegraded);
    } else {
        setValue(session_.hello, t("N/A"), PanelState::kUnavailable);
        setValue(session_.ping, t("N/A"), PanelState::kUnavailable);
    }

    if (hasStats_ && stats_.rttValid) {
        setValue(session_.rtt, t("%1 ms").arg(stats_.rttMs),
                 PanelState::kActive);
    } else {
        setValue(session_.rtt, t("N/A"), PanelState::kUnavailable);
    }
}

void DisplayStatusPanel::refreshMode() {
    const bool known = (mode_ >= 0 && mode_ <= 3);
    setValue(displayMode_.mode, modeName(mode_),
             known ? PanelState::kActive : PanelState::kUnavailable);
}

void DisplayStatusPanel::refreshVirtual() {
    const PanelState st = virtualState();
    setValue(virtual_.app, stateText(st), st);

    if (hasStats_ && stats_.peerWidth > 0 && stats_.peerHeight > 0) {
        setValue(virtual_.resolution,
                 t("%1 x %2").arg(stats_.peerWidth).arg(stats_.peerHeight),
                 PanelState::kActive);
    } else {
        setValue(virtual_.resolution, t("N/A"), PanelState::kUnavailable);
    }

    if (hasStats_) {
        setValue(virtual_.fps,
                 QStringLiteral("%1").arg(stats_.effectiveFps, 0, 'f', 2),
                 stats_.effectiveFps > 0.0 ? PanelState::kActive
                                           : PanelState::kDegraded);
        setValue(virtual_.frames,
                 QStringLiteral("%1 / %2")
                     .arg(stats_.committedFrames)
                     .arg(stats_.discardedFrames),
                 stats_.discardedFrames == 0 ? PanelState::kActive
                                             : PanelState::kDegraded);
    } else {
        setValue(virtual_.fps, t("N/A"), PanelState::kUnavailable);
        setValue(virtual_.frames, t("N/A"), PanelState::kUnavailable);
    }
}

void DisplayStatusPanel::refreshPhysical() {
    setValue(physical_.scene, sceneName(phys_.physicalScene),
             phys_.physicalScene <= 1 ? PanelState::kActive
                                      : PanelState::kUnavailable);

    const PanelState st = physicalState();
    // Physical 状态文案：READY / DEGRADED / UNAVAILABLE / DISABLED。
    QString text;
    switch (st) {
        case PanelState::kActive:
            text = t("Ready");
            break;
        case PanelState::kDegraded:
            text = t("Degraded");
            break;
        case PanelState::kDisabled:
            text = t("Disabled");
            break;
        case PanelState::kUnavailable:
            text = t("Unavailable");
            break;
    }
    setValue(physical_.state, text, st);

    if (phys_.oledValid) {
        const QString ctrl = QLatin1String(controllerCodeName(phys_.oledController));
        const QString addr =
            QStringLiteral("0x%1")
                .arg(phys_.oledAddress, 2, 16, QLatin1Char('0'))
                .toUpper();
        setValue(physical_.oled,
                 t("%1 @ %2 (%3)").arg(ctrl, addr,
                                                    phys_.oledOk ? t("OK") : t("Fault")),
                 phys_.oledOk ? PanelState::kActive : PanelState::kDegraded);
    } else {
        setValue(physical_.oled, t("N/A"), PanelState::kUnavailable);
    }

    if (phys_.oledValid) {
        setValue(physical_.errors, fmtU64(phys_.oledErrCount),
                 phys_.oledErrCount == 0 ? PanelState::kActive
                                         : PanelState::kDegraded);
    } else {
        setValue(physical_.errors, t("N/A"), PanelState::kUnavailable);
    }

    if (phys_.memValid) {
        setValue(physical_.heap,
                 t("%1 / %2 / %3")
                     .arg(phys_.heapFree)
                     .arg(phys_.heapLargest)
                     .arg(phys_.heapMinFree),
                 phys_.heapMinFree > 0 ? PanelState::kActive
                                       : PanelState::kDegraded);
    } else {
        setValue(physical_.heap, t("N/A"), PanelState::kUnavailable);
    }
}

void DisplayStatusPanel::refreshRouter() {
    const uint8_t st = static_cast<uint8_t>(routerState_);
    PanelState ps = PanelState::kUnavailable;
    switch (st) {
        case 2:
            ps = PanelState::kActive;
            break;
        case 1:
        case 3:
            ps = PanelState::kDegraded;
            break;
        case 0:
            ps = PanelState::kDisabled;
            break;
        case 0xFF:
        default:
            ps = PanelState::kUnavailable;
            break;
    }
    setValue(router_.state, routerStateText(st), ps);
}

void DisplayStatusPanel::refreshErrors() {
    if (phys_.oledValid) {
        setValue(errors_.oled, fmtU64(phys_.oledErrCount),
                 phys_.oledErrCount == 0 ? PanelState::kActive
                                         : PanelState::kDegraded);
    } else {
        setValue(errors_.oled, t("N/A"), PanelState::kUnavailable);
    }

    if (hasStats_) {
        const uint64_t total = stats_.decoderErrors + stats_.crcErrors + stats_.seqGaps;
        setValue(errors_.protocol,
                 t("%1 / %2 / %3")
                     .arg(stats_.decoderErrors)
                     .arg(stats_.crcErrors)
                     .arg(stats_.seqGaps),
                 total == 0 ? PanelState::kActive : PanelState::kDegraded);
    } else {
        setValue(errors_.protocol, t("N/A"), PanelState::kUnavailable);
    }

    if (phys_.displayValid || hasStats_) {
        const uint64_t esp = phys_.displayValid ? phys_.framesDropped : 0;
        const uint64_t host = hasStats_ ? stats_.discardedFrames : 0;
        setValue(errors_.dropped,
                 t("ESP %1 / host %2").arg(esp).arg(host),
                 (esp + host) == 0 ? PanelState::kActive : PanelState::kDegraded);
    } else {
        setValue(errors_.dropped, t("N/A"), PanelState::kUnavailable);
    }

    if (phys_.displayValid) {
        setValue(errors_.queue, fmtU64(phys_.queueFullEvents),
                 phys_.queueFullEvents == 0 ? PanelState::kActive
                                            : PanelState::kDegraded);
    } else {
        setValue(errors_.queue, t("N/A"), PanelState::kUnavailable);
    }
}

void DisplayStatusPanel::refreshFullResync() {
    if (fullResyncPending_) {
        setValue(resync_.state, t("Pending"), PanelState::kDegraded);
    } else {
        setValue(resync_.state, t("Done"), PanelState::kActive);
    }
}

}  // namespace pc
}  // namespace espview
