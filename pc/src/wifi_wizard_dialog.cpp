// ESPView M7-D4 — WifiWizardDialog 实现（Qt Widgets，UI 层）。
// 规范来源：docs/DESIGN.md AG 节；状态机规则见 wifi_wizard_state.h/.cpp。

#include "wifi_wizard_dialog.h"

#include <QCheckBox>
#include <cstdio>
#include <QCloseEvent>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace espview {
namespace pc {

namespace {

constexpr int kPageInit = 0;
constexpr int kPageConnectUart = 1;
constexpr int kPageReadCapabilities = 2;
constexpr int kPageScan = 3;
constexpr int kPageSelectSsid = 4;
constexpr int kPagePassword = 5;
constexpr int kPageTcpConfig = 6;
constexpr int kPageAsync = 7;
constexpr int kPageDone = 8;
constexpr int kPageError = 9;

// IPv4 点分十进制 → 网络序 u32（AF.2 期望网络序）。输入已经过
// WifiWizardState 严格校验（恰好 4 段、每段 1..3 位数字、0..255），
// 此处解析失败仅作防御性降级（返回 false）。
bool ipv4ToNetworkOrder(const std::string& text, quint32& out) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a > 255u || b > 255u || c > 255u || d > 255u) {
        return false;
    }
    out = (static_cast<quint32>(a) << 24) | (static_cast<quint32>(b) << 16) |
          (static_cast<quint32>(c) << 8) | static_cast<quint32>(d);
    return true;
}

}  // namespace

WifiWizardDialog::WifiWizardDialog(ConnectionManager& manager, QWidget* parent)
    : QDialog(parent), manager_(manager) {
    buildUi();
    setMinimumWidth(420);
    refreshUi();

    // M7-D3 信号（queued：Worker 线程 → GUI 线程；对话框模态期间持续有效）。
    connect(&manager_, &ConnectionManager::statusChanged, this,
            [this](WorkerStatus s, const QString& t) { onStatusChanged(s, t); });
    connect(&manager_, &ConnectionManager::capabilitiesReceived, this,
            [this](const espview::proto::CapabilitiesInfo& c) { onCapabilitiesReceived(c); });
    connect(&manager_, &ConnectionManager::wifiScanResult, this,
            [this](const espview::proto::WifiScanResultInfo& r) { onWifiScanResult(r); });
    connect(&manager_, &ConnectionManager::wifiScanReqAck, this,
            [this](bool ok, quint16 code) { onWifiScanReqAck(ok, code); });
    connect(&manager_, &ConnectionManager::wifiConfigAck, this,
            [this](bool ok, quint16 code) { onWifiConfigAck(ok, code); });
    connect(&manager_, &ConnectionManager::wifiStatus, this,
            [this](const espview::proto::WifiStatusInfo& s) { onWifiStatus(s); });
    connect(&manager_, &ConnectionManager::frameReady, this,
            [this](const DisplayFrame& f) { onFrameReady(f); });
}

void WifiWizardDialog::buildUi() {
    auto* root = new QVBoxLayout(this);

    title_ = new QLabel(this);
    QFont titleFont = title_->font();
    titleFont.setBold(true);
    title_->setFont(titleFont);
    root->addWidget(title_);

    hint_ = new QLabel(this);
    hint_->setWordWrap(true);
    root->addWidget(hint_);

    pages_ = new QStackedWidget(this);
    root->addWidget(pages_, 1);

    // kInit
    pages_->addWidget(new QLabel(this));
    // kConnectUart
    pages_->addWidget(new QLabel(this));
    // kReadCapabilities
    pages_->addWidget(new QLabel(this));

    // kScan：扫描按钮 + 结果列表
    auto* scanPage = new QWidget(this);
    auto* scanLay = new QVBoxLayout(scanPage);
    scanBtn_ = new QPushButton(tr_("Scan"), scanPage);
    scanLay->addWidget(scanBtn_);
    scanStatusLabel_ = new QLabel(scanPage);
    scanStatusLabel_->setWordWrap(true);
    scanStatusLabel_->setAlignment(Qt::AlignCenter);
    scanLay->addWidget(scanStatusLabel_);
    scanList_ = new QListWidget(scanPage);
    scanLay->addWidget(scanList_, 1);
    pages_->addWidget(scanPage);

    // kSelectSsid
    auto* ssidPage = new QWidget(this);
    auto* ssidLay = new QVBoxLayout(ssidPage);
    ssidCombo_ = new QComboBox(ssidPage);
    ssidLay->addWidget(ssidCombo_);
    ssidLay->addStretch(1);
    pages_->addWidget(ssidPage);

    // kPassword
    auto* pwdPage = new QWidget(this);
    auto* pwdLay = new QGridLayout(pwdPage);
    pwdLay->addWidget(new QLabel(tr_("Password"), pwdPage), 0, 0);
    passwordEdit_ = new QLineEdit(pwdPage);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setMaxLength(static_cast<int>(kMaxWpa2PasswordBytes));
    pwdLay->addWidget(passwordEdit_, 0, 1);
    openNetCheck_ = new QCheckBox(tr_("Open network (no password)"), pwdPage);
    pwdLay->addWidget(openNetCheck_, 1, 0, 1, 2);
    pages_->addWidget(pwdPage);

    // kTcpConfig
    auto* tcpPage = new QWidget(this);
    auto* tcpLay = new QGridLayout(tcpPage);
    tcpLay->addWidget(new QLabel(tr_("Server IP"), tcpPage), 0, 0);
    serverIpEdit_ = new QLineEdit(tcpPage);
    serverIpEdit_->setPlaceholderText(QStringLiteral("192.168.1.100"));
    tcpLay->addWidget(serverIpEdit_, 0, 1);
    tcpLay->addWidget(new QLabel(tr_("TCP port"), tcpPage), 1, 0);
    portSpin_ = new QSpinBox(tcpPage);
    portSpin_->setRange(static_cast<int>(kMinTcpPort), static_cast<int>(kMaxTcpPort));
    portSpin_->setValue(8765);
    tcpLay->addWidget(portSpin_, 1, 1);
    pages_->addWidget(tcpPage);

    // kApplying..kFullResync 共用的异步状态页
    asyncStatusLabel_ = new QLabel(this);
    asyncStatusLabel_->setWordWrap(true);
    asyncStatusLabel_->setAlignment(Qt::AlignCenter);
    pages_->addWidget(asyncStatusLabel_);

    // kDone
    doneLabel_ = new QLabel(this);
    doneLabel_->setWordWrap(true);
    doneLabel_->setAlignment(Qt::AlignCenter);
    pages_->addWidget(doneLabel_);

    // kError
    errorLabel_ = new QLabel(this);
    errorLabel_->setWordWrap(true);
    errorLabel_->setAlignment(Qt::AlignCenter);
    pages_->addWidget(errorLabel_);

    // 按钮行
    auto* btnLay = new QHBoxLayout;
    btnLay->addStretch(1);
    backBtn_ = new QPushButton(tr_("Back"), this);
    btnLay->addWidget(backBtn_);
    nextBtn_ = new QPushButton(tr_("Next"), this);
    btnLay->addWidget(nextBtn_);
    cancelBtn_ = new QPushButton(tr_("Cancel"), this);
    btnLay->addWidget(cancelBtn_);
    root->addLayout(btnLay);

    connect(backBtn_, &QPushButton::clicked, this, &WifiWizardDialog::onBack);
    connect(nextBtn_, &QPushButton::clicked, this, &WifiWizardDialog::onNext);
    connect(cancelBtn_, &QPushButton::clicked, this, &WifiWizardDialog::onCancel);
    connect(scanBtn_, &QPushButton::clicked, this, &WifiWizardDialog::onScanClicked);
    connect(passwordEdit_, &QLineEdit::textChanged, this, &WifiWizardDialog::onPasswordChanged);
    connect(openNetCheck_, &QCheckBox::toggled, this, &WifiWizardDialog::onOpenNetworkToggled);
    connect(serverIpEdit_, &QLineEdit::textEdited, this, &WifiWizardDialog::onTcpIpEdited);
    connect(portSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &WifiWizardDialog::onTcpPortChanged);
    connect(ssidCombo_, &QComboBox::currentTextChanged, this,
            &WifiWizardDialog::onSsidChanged);
    connect(scanList_, &QListWidget::itemSelectionChanged, this,
            &WifiWizardDialog::onScanListSelectionChanged);
}

QString WifiWizardDialog::tr_(const char* key) const {
    return QString::fromUtf8(trText(lang_, key));
}

void WifiWizardDialog::setHint(const char* key) {
    hint_->setText(tr_(key));
}

void WifiWizardDialog::clearSecrets() {
    if (passwordEdit_ != nullptr) {
        passwordEdit_->clear();
    }
    if (openNetCheck_ != nullptr) {
        openNetCheck_->setChecked(false);
    }
    // WifiWizardState 的 password_ 仅在 beginApply() 或 cancel() 中清除；
    // 对话框关闭时强制 cancel 语义清空（未 Apply 的编辑内容也不残留）。
    if (state_.step() <= WizardStep::kTcpConfig || state_.step() == WizardStep::kError) {
        state_.cancel();
    }
}

void WifiWizardDialog::refreshUi() {
    const WizardStep step = state_.step();
    title_->setText(tr_(wizardStepTitleKey(step)));

    int page = kPageInit;
    bool showBack = false;
    bool showNext = false;
    bool nextEnabled = true;
    bool showCancel = true;
    QString nextText = tr_("Next");
    QString cancelText = tr_("Cancel");

    switch (step) {
        case WizardStep::kInit:
            page = kPageInit;
            setHint("This wizard configures Wi-Fi over the UART bootstrap link");
            showNext = true;
            nextText = tr_("Start");
            break;
        case WizardStep::kConnectUart:
            page = kPageConnectUart;
            setHint("Connect the ESP32 to this PC over UART first");
            showBack = true;
            showNext = true;
            break;
        case WizardStep::kReadCapabilities:
            page = kPageReadCapabilities;
            setHint("Waiting for the device capabilities");
            showBack = true;
            showNext = true;
            break;
        case WizardStep::kScan:
            page = kPageScan;
            setHint(scanInFlight_ ? "Scanning..."
                                  : "Click Scan to search for nearby Wi-Fi networks");
            showBack = true;
            showNext = true;
            nextEnabled = state_.hasSsid();
            scanBtn_->setEnabled(!scanInFlight_);
            // M7-E：扫描期间明确标注“显示临时暂停”，完成/失败后标注“显示已恢复”。
            // 只显示契约允许的暂停/恢复文案，不显示任何未经证明的电源结论。
            switch (scanDisplayState_) {
                case ScanDisplayState::kIdle:
                    scanStatusLabel_->setVisible(false);
                    break;
                case ScanDisplayState::kScanning:
                    scanStatusLabel_->setVisible(true);
                    scanStatusLabel_->setText(
                        tr_("displayPausedForWifiScan") + QStringLiteral("\n") +
                        tr_("displayTemporarilyPausedDuringWifiScan"));
                    break;
                case ScanDisplayState::kSucceeded:
                    scanStatusLabel_->setVisible(true);
                    scanStatusLabel_->setText(tr_("scanComplete") + QStringLiteral("\n") +
                                              tr_("restoringDisplay"));
                    break;
                case ScanDisplayState::kFailed:
                    scanStatusLabel_->setVisible(true);
                    scanStatusLabel_->setText(tr_("scanFailed") + QStringLiteral("\n") +
                                              tr_("restoringDisplay"));
                    break;
            }
            break;
        case WizardStep::kSelectSsid:
            page = kPageSelectSsid;
            setHint("Select the network to configure");
            showBack = true;
            showNext = true;
            nextEnabled = state_.hasSsid();
            break;
        case WizardStep::kPassword:
            page = kPagePassword;
            setHint("Leave empty for an open network");
            showBack = true;
            showNext = true;
            nextEnabled = state_.wifiPasswordValid();
            break;
        case WizardStep::kTcpConfig:
            page = kPageTcpConfig;
            setHint("ESP32 will connect to this TCP server after GOT_IP");
            showBack = true;
            showNext = true;
            nextText = tr_("Apply");
            nextEnabled = state_.canApply();
            break;
        case WizardStep::kApplying:
            page = kPageAsync;
            asyncStatusLabel_->setText(tr_("Applying configuration to ESP32..."));
            break;
        case WizardStep::kConnecting:
            page = kPageAsync;
            asyncStatusLabel_->setText(tr_("ESP32 is connecting to Wi-Fi..."));
            break;
        case WizardStep::kGotIp:
            page = kPageAsync;
            asyncStatusLabel_->setText(tr_("IP address acquired"));
            break;
        case WizardStep::kTcpConnected:
            page = kPageAsync;
            asyncStatusLabel_->setText(tr_("ESP32 connected to the TCP server"));
            break;
        case WizardStep::kFullResync:
            page = kPageAsync;
            asyncStatusLabel_->setText(tr_("Waiting for the first FULL frame"));
            break;
        case WizardStep::kDone:
            page = kPageDone;
            doneLabel_->setText(tr_("Wi-Fi provisioning complete"));
            showCancel = true;
            cancelText = tr_("Close");
            break;
        case WizardStep::kError:
            page = kPageError;
            {
                QString msg;
                if (state_.error().code == WizardErrorCode::kScanFailed) {
                    // M7-E：扫描失败 → 显示已恢复，可重试；不显示电源结论。
                    msg = tr_("scanFailed") + QStringLiteral("\n") +
                          tr_("restoringDisplay");
                } else {
                    msg = tr_(state_.error().i18nKey);
                }
                if (lastScanFirmwareUnsupported_) {
                    msg += QStringLiteral("\n") +
                           tr_("Firmware does not support Wi-Fi provisioning");
                }
                errorLabel_->setText(msg);
            }
            showNext = true;
            nextText = tr_("Retry");
            nextEnabled = true;
            showCancel = true;
            break;
    }

    pages_->setCurrentIndex(page);
    backBtn_->setVisible(showBack);
    backBtn_->setEnabled(showBack);
    nextBtn_->setVisible(showNext);
    nextBtn_->setEnabled(nextEnabled);
    nextBtn_->setText(nextText);
    cancelBtn_->setVisible(showCancel);
    cancelBtn_->setText(cancelText);

    // 进入选择页时同步扫描数据
    if ((step == WizardStep::kScan || step == WizardStep::kSelectSsid) &&
        scanEntries_.empty() && !scanInFlight_) {
        scanBtn_->setEnabled(step == WizardStep::kScan);
    }
}

void WifiWizardDialog::populateScanList() {
    scanList_->clear();
    ssidCombo_->blockSignals(true);
    ssidCombo_->clear();
    for (const ScanEntry& e : scanEntries_) {
        QListWidgetItem* item = new QListWidgetItem(e.display, scanList_);
        item->setData(Qt::UserRole, QString::fromUtf8(e.ssid.c_str(),
                                                      static_cast<int>(e.ssid.size())));
        ssidCombo_->addItem(e.display, QString::fromUtf8(e.ssid.c_str(),
                                                        static_cast<int>(e.ssid.size())));
    }
    ssidCombo_->blockSignals(false);
    if (scanEntries_.empty() && !scanInFlight_) {
        setHint("No Wi-Fi networks found");
    }
}

void WifiWizardDialog::startScan() {
    scanInFlight_ = true;
    lastScanFirmwareUnsupported_ = false;
    scanDisplayState_ = ScanDisplayState::kScanning;
    scanEntries_.clear();
    populateScanList();
    setHint("Scanning...");
    manager_.sendWifiScanRequest(0);  // maxEntries 0 = 默认 32
    refreshUi();
}

void WifiWizardDialog::applyConfig() {
    if (state_.step() != WizardStep::kTcpConfig) {
        return;
    }
    if (!state_.beginApply()) {
        refreshUi();
        return;
    }
    quint32 serverIpNet = 0;
    if (!ipv4ToNetworkOrder(state_.tcpServerIp(), serverIpNet)) {
        state_.markError(WizardErrorCode::kInvalidTcpServerIp);
        refreshUi();
        return;
    }
    manager_.sendWifiConfig(state_.ssid(), state_.password(), serverIpNet,
                            static_cast<uint16_t>(state_.tcpServerPort()));
    clearSecrets();  // 密码副本立即清零（AG.3；state 的 password_ 已由 beginApply 清除）
    refreshUi();
}

void WifiWizardDialog::onNext() {
    const WizardStep step = state_.step();
    switch (step) {
        case WizardStep::kInit:
        case WizardStep::kConnectUart:
        case WizardStep::kReadCapabilities:
        case WizardStep::kScan:
        case WizardStep::kSelectSsid:
        case WizardStep::kPassword:
            if (!state_.next()) {
                const WizardError err = state_.validationError();
                if (err.code != WizardErrorCode::kNone) {
                    setHint(err.i18nKey);
                }
            }
            refreshUi();
            break;
        case WizardStep::kTcpConfig:
            applyConfig();
            break;
        case WizardStep::kError:
            onRetry();
            break;
        case WizardStep::kDone:
            clearSecrets();
            accept();
            break;
        default:
            break;  // 异步步骤不可手动 Next
    }
}

void WifiWizardDialog::onBack() {
    if (state_.back()) {
        refreshUi();
    }
}

void WifiWizardDialog::onCancel() {
    clearSecrets();
    reject();
}

void WifiWizardDialog::onRetry() {
    scanDisplayState_ = ScanDisplayState::kIdle;  // M7-E：重试回扫描页，恢复默认状态
    if (state_.retry()) {
        refreshUi();
    }
}

void WifiWizardDialog::onScanClicked() {
    if (state_.step() == WizardStep::kScan) {
        startScan();
    }
}

void WifiWizardDialog::onPasswordChanged(const QString& text) {
    state_.setPassword(text.toStdString());
    nextBtn_->setEnabled(state_.wifiPasswordValid());
}

void WifiWizardDialog::onOpenNetworkToggled(bool checked) {
    if (checked) {
        passwordEdit_->clear();  // 开放网络：显式空密码
    }
    passwordEdit_->setEnabled(!checked);
    nextBtn_->setEnabled(state_.wifiPasswordValid());
}

void WifiWizardDialog::onSsidChanged(const QString& text) {
    Q_UNUSED(text);
    const QString ssid = ssidCombo_->currentData().toString();
    if (state_.setSsid(ssid.toStdString())) {
        nextBtn_->setEnabled(state_.hasSsid());
    }
}

void WifiWizardDialog::onTcpIpEdited() {
    state_.setTcpServerIp(serverIpEdit_->text().toStdString());
    nextBtn_->setEnabled(state_.canApply());
}

void WifiWizardDialog::onTcpPortChanged(int value) {
    state_.setTcpServerPort(static_cast<uint32_t>(value));
    nextBtn_->setEnabled(state_.canApply());
}

void WifiWizardDialog::onScanListSelectionChanged() {
    const QList<QListWidgetItem*> items = scanList_->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    const QString ssid = items.first()->data(Qt::UserRole).toString();
    if (state_.setSsid(ssid.toStdString())) {
        nextBtn_->setEnabled(state_.hasSsid());
    }
}

void WifiWizardDialog::onCapabilitiesReceived(const espview::proto::CapabilitiesInfo& caps) {
    Q_UNUSED(caps);
    if (state_.step() == WizardStep::kReadCapabilities) {
        state_.next();
        refreshUi();
    }
}

void WifiWizardDialog::onStatusChanged(WorkerStatus status, const QString& text) {
    Q_UNUSED(text);
    if (status == WorkerStatus::Connected && state_.step() == WizardStep::kConnectUart) {
        state_.next();
        refreshUi();
    }
}

void WifiWizardDialog::onWifiScanResult(const espview::proto::WifiScanResultInfo& result) {
    scanEntries_.clear();
    for (const espview::proto::WifiScanRecordInfo& rec : result.records) {
        ScanEntry e;
        e.ssid = rec.ssid;
        e.display = QString::fromUtf8(rec.ssid.c_str(), static_cast<int>(rec.ssid.size())) +
                    QStringLiteral("  [RSSI %1 dBm · ch %2]")
                        .arg(static_cast<int>(rec.rssi))
                        .arg(static_cast<int>(rec.channel));
        scanEntries_.push_back(std::move(e));
    }
    scanInFlight_ = false;
    scanDisplayState_ = ScanDisplayState::kSucceeded;
    populateScanList();
    refreshUi();
}

void WifiWizardDialog::onWifiScanReqAck(bool ok, quint16 errorCode) {
    scanInFlight_ = false;
    if (!ok) {
        scanDisplayState_ = ScanDisplayState::kFailed;
        // AF.3 探针语义：ACK ERR kInvalidParam = 老固件不支持 Wi-Fi provisioning。
        lastScanFirmwareUnsupported_ =
            (errorCode == static_cast<quint16>(espview::proto::ErrorCode::kInvalidParam));
        state_.markError(WizardErrorCode::kScanFailed);
    }
    refreshUi();
}

void WifiWizardDialog::onWifiConfigAck(bool ok, quint16 errorCode) {
    Q_UNUSED(errorCode);
    if (ok) {
        state_.markConnecting();
    } else {
        state_.markError(WizardErrorCode::kApplyFailed);
    }
    refreshUi();
}

void WifiWizardDialog::onWifiStatus(const espview::proto::WifiStatusInfo& status) {
    switch (static_cast<espview::proto::WifiStatusPhase>(status.phase)) {
        case espview::proto::WifiStatusPhase::kGotIp:
            if (state_.step() == WizardStep::kConnecting) {
                state_.markGotIp();
                refreshUi();
            }
            break;
        case espview::proto::WifiStatusPhase::kTcpConnected:
            if (state_.step() == WizardStep::kGotIp) {
                state_.markTcpConnected();
                tcpConnectedArmed_ = true;
                refreshUi();
            }
            break;
        case espview::proto::WifiStatusPhase::kError:
            if (state_.step() == WizardStep::kConnecting ||
                state_.step() == WizardStep::kGotIp ||
                state_.step() == WizardStep::kTcpConnected) {
                state_.markError(WizardErrorCode::kWifiConnectFailed);
                refreshUi();
            }
            break;
        default:
            break;  // kIdle/kScanning/kConfigApplying/kWifiConnecting/kWifiConnected/kTcpConnecting/kCleared
    }
}

void WifiWizardDialog::onFrameReady(const DisplayFrame& frame) {
    Q_UNUSED(frame);
    // Step 11：TCP 已连后的第一帧（FULL 提交）→ 全帧重同步完成 → Done。
    if (tcpConnectedArmed_ &&
        (state_.step() == WizardStep::kTcpConnected ||
         state_.step() == WizardStep::kFullResync)) {
        tcpConnectedArmed_ = false;
        state_.markFullResync();
        state_.markDone();
        clearSecrets();
        refreshUi();
    }
}

void WifiWizardDialog::closeEvent(QCloseEvent* event) {
    clearSecrets();
    QDialog::closeEvent(event);
}

void WifiWizardDialog::setUiLanguage(int lang) {
    lang_ = static_cast<UiLang>(lang);
    refreshUi();
}

}  // namespace pc
}  // namespace espview
