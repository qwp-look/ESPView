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
#include <QTimer>
#include <QVBoxLayout>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "host_tcp_transport.h"
#include "protocol_endpoint.h"

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

// M7-F：异步步看门狗（超时收敛，禁止向导挂死；数值为保守上界）。
constexpr int kScanWatchdogMs = 20000;   // 扫描：固件看门狗 10s + 余量
constexpr int kApplyWatchdogMs = 60000;  // Apply→Wi-Fi→TCP：DHCP 上限 + 握手余量
constexpr int kResyncWatchdogMs = 20000; // TCP 已连 → 首帧 FULL resync
// M7-G：bootstrap 步看门狗（B1/B4：Step 1/2 也须收敛，不得永久等待）。
constexpr int kBootstrapWatchdogMs = 30000;   // Step 1：UART 会话建立（含手动插线余量）
constexpr int kCapabilityWatchdogMs = 15000;  // Step 2：CAPABILITIES 超时
constexpr char kHandoffBind[] = "0.0.0.0";    // Step 10：TCP handoff 监听（全接口，AF.4 明文风险已声明）

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

uint64_t steadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

// M7-G（B7）：PC 侧 TCP handoff 观察器。
// 语义（docs/DESIGN.md AK.5 / AF.2）：ESP32 GOT_IP 后作为 TCP client 连接 PC
// 向导配置的 server 端口 → TCP HELLO 握手 → 首帧 FULL commit。本观察器只做
// “真实握手 + 首帧”检测（状态显示不得伪造 PASS）：后台线程 bind/accept/解析，
// 事件经 queued signal 送回 GUI 线程。凭据零接触（AF.4：绝不经 TCP 收发凭据）。
class TcpHandoffObserver : public QObject {
    Q_OBJECT
public:
    explicit TcpHandoffObserver(QObject* parent = nullptr) : QObject(parent) {}
    ~TcpHandoffObserver() override { stop(); }

    // bind 失败返回 false 并 emit failed()（GUI 显示可见错误）。
    bool start(uint16_t port, const std::string& bind = "0.0.0.0") {
        if (running_.load()) {
            return true;
        }
        TcpListener::Config lcfg;
        lcfg.bind = bind;
        lcfg.port = port;
        if (!listener_.bindListen(lcfg)) {
            emit failed(QStringLiteral("TCP bind %1:%2 failed — %3")
                            .arg(QString::fromStdString(bind))
                            .arg(port)
                            .arg(QString::fromStdString(listener_.lastError())));
            return false;
        }
        running_.store(true);
        thread_ = std::thread([this]() { runAccept(); });
        return true;
    }

    // 置停止标志 → cancel 唤醒 accept → join（GUI 线程调用；join 后无任何 signal）。
    void stop() {
        running_.store(false);
        listener_.cancel();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

signals:
    void handshakeConnected();   // TCP HELLO 握手完成（Step 10 依据）
    void firstFrameCommitted();  // 首帧 FULL commit（Step 11 依据）
    void failed(const QString& reason);  // bind/accept 失败
    void linkLost();             // 握手前/后连接断开

private:
    void runAccept() {
        HostTcpTransport tcp;
        if (!listener_.acceptOne(tcp)) {
            listener_.close();
            return;  // 被 stop() 取消 / 无客户端
        }
        runSession(tcp);
        listener_.close();
    }

    void runSession(HostTcpTransport& tcp) {
        proto::EndpointConfig cfg;
        cfg.protocol_version = proto::kProtocolVersion;
        cfg.device_class = 0;
        cfg.width = 320;
        cfg.height = 240;
        cfg.pixel_format = proto::PixelFormat::kRgb565;
        cfg.mode_mask = 0b1111;  // 与 SerialWorker 一致
        cfg.device_name = "espview-pc";

        auto sink = [&tcp](const uint8_t* d, size_t n) -> proto::SendStatus {
            return tcp.send(d, n);  // M8-A3：send 直接返回 canonical SendStatus
        };

        proto::ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](proto::SessionState s) {
            if (s == proto::SessionState::kConnected) {
                if (!handshakeDone_.exchange(true)) {
                    emit handshakeConnected();
                }
            } else if (s == proto::SessionState::kDisconnected) {
                emit linkLost();
            }
        };
        cb.onFrameCommit = [this](const proto::CommittedFrame&) {
            if (!frameDone_.exchange(true)) {
                emit firstFrameCommitted();
            }
        };

        proto::ProtocolEndpoint ep(cfg, sink, cb, steadyMs);

        tcp.setDataCallback([this, &ep](const uint8_t* d, size_t n) {
            std::lock_guard<std::mutex> lk(rxMutex_);
            rxBuf_.insert(rxBuf_.end(), d, d + n);
        });

        // 主动 HELLO：握手快速收敛（对端 HELLO 到达后由 endpoint 自动互发）。
        ep.onTransportConnected();

        std::vector<uint8_t> chunk;
        while (running_.load() && tcp.isConnected()) {
            {
                std::lock_guard<std::mutex> lk(rxMutex_);
                if (!rxBuf_.empty()) {
                    chunk.swap(rxBuf_);
                }
            }
            if (!chunk.empty()) {
                ep.onTransportData(chunk.data(), chunk.size());
                chunk.clear();
            }
            ep.tick();
            // 可被 stop 中断的短等待（~10ms 周期）。
            for (int i = 0; i < 2 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        tcp.close();
        // 连接在向导未停止时断开 → 通知 GUI（握手前/后统一链路丢失）。
        if (running_.load()) {
            emit linkLost();
        }
    }

    TcpListener listener_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> handshakeDone_{false};
    std::atomic<bool> frameDone_{false};
    std::mutex rxMutex_;
    std::vector<uint8_t> rxBuf_;
};

WifiWizardDialog::WifiWizardDialog(ConnectionManager& manager, QWidget* parent)
    : QDialog(parent), manager_(manager) {
    buildUi();
    setMinimumWidth(420);
    refreshUi();

    // M7-D3 信号（queued：Worker 线程 → GUI 线程；对话框模态期间持续有效）。
    connect(&manager_, &ConnectionManager::statusChanged, this,
            [this](quint64 sid, WorkerStatus s, const QString& t) { onStatusChanged(sid, s, t); });
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

    // M7-F：异步步看门狗（单发；startWatchdog 重启、stopWatchdog 停）。
    watchdogTimer_ = new QTimer(this);
    watchdogTimer_->setSingleShot(true);
    connect(watchdogTimer_, &QTimer::timeout, this, &WifiWizardDialog::onWatchdogTimeout);
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
    // M7-G（B7）：终态（Done/Error）停止 TCP handoff 观察器（幂等；join 后无事件）。
    if (step == WizardStep::kDone || step == WizardStep::kError) {
        stopHandoffListener();
    }
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
            setHint((scanInFlight_ || scanResultPending_) ? "Scanning..."
                    : "Click Scan to search for nearby Wi-Fi networks");
            showBack = true;
            showNext = true;
            nextEnabled = state_.hasSsid();
            scanBtn_->setEnabled(!scanInFlight_ && !scanResultPending_);
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
            // M7-G（B3，AF.4）：凭据只经 UART bootstrap 下发；当前传输非 UART
            // 时禁用 Apply 并明示原因（扫描等无凭据操作仍可用）。
            if (manager_.currentKind() != TransportKind::kUart) {
                setHint("Wi-Fi provisioning requires the UART bootstrap link");
                nextEnabled = false;
            }
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
            // M7-G（B7）：IP 已获 → 等待 ESP32 TCP handoff（真实握手驱动 Step 10）。
            asyncStatusLabel_->setText(tr_("IP address acquired") +
                                       QStringLiteral("\n") +
                                       tr_("Waiting for ESP32 TCP handoff..."));
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
    scanResultPending_ = false;
    lastScanFirmwareUnsupported_ = false;
    scanDisplayState_ = ScanDisplayState::kScanning;
    scanEntries_.clear();
    populateScanList();
    setHint("Scanning...");
    manager_.sendWifiScanRequest(0);  // maxEntries 0 = 默认 32
    startWatchdog(kScanWatchdogMs);   // M7-F：扫描超时收敛（固件挂死/掉线不挂 UI）
    refreshUi();
}

void WifiWizardDialog::applyConfig() {
    if (state_.step() != WizardStep::kTcpConfig) {
        return;
    }
    // M7-G（B3，AF.4 纵深防御）：凭据只经 UART bootstrap 下发；refreshUi 已
    // 禁用 Apply 按钮，此处再拦一道（TCP 等其他传输绝不发送真实凭据）。
    if (manager_.currentKind() != TransportKind::kUart) {
        setHint("Wi-Fi provisioning requires the UART bootstrap link");
        refreshUi();
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
    // M7-F：sendWifiConfig 消费后立即清除密码（AG.3/AF.4；原实现把清除放在
    // beginApply 内，导致发送时密码已为空——WPA2 实际下发空密码）。
    state_.clearPassword();
    clearSecrets();  // UI 副本（QLineEdit）同样立即清零
    startWatchdog(kApplyWatchdogMs);  // M7-F：Apply 链超时收敛
    startHandoffListener();  // M7-G（B7）：监听配置端口，等待 ESP32 TCP 连接
    refreshUi();
}

void WifiWizardDialog::startWatchdog(int ms) {
    if (watchdogTimer_ != nullptr) {
        watchdogTimer_->start(ms);
    }
}

void WifiWizardDialog::stopWatchdog() {
    if (watchdogTimer_ != nullptr) {
        watchdogTimer_->stop();
    }
}

void WifiWizardDialog::onWatchdogTimeout() {
    if (state_.step() == WizardStep::kConnectUart) {
        // M7-G（B4）：Step 1 超时 = UART bootstrap 不可用（可见错误，非密码错误）。
        state_.markError(WizardErrorCode::kUartBootstrapUnavailable);
    } else if (state_.step() == WizardStep::kReadCapabilities) {
        // M7-G（B4）：Step 2 超时 = 能力读取失败（同样收敛，不永久等待）。
        state_.markError(WizardErrorCode::kCapabilityReadFailed);
    } else if (state_.step() == WizardStep::kScan &&
               (scanInFlight_ || scanResultPending_)) {
        // 扫描无 ACK/无结果超时 → 扫描失败（显示已恢复，可重试；B1 收敛）。
        scanInFlight_ = false;
        scanResultPending_ = false;
        scanDisplayState_ = ScanDisplayState::kFailed;
        state_.markError(WizardErrorCode::kScanFailed);
    } else if (state_.isApplying()) {
        // M7-F：按当前异步步给出诚实错误——kGotIp 之后是 TCP handoff 问题
        // （Wi-Fi 已连接），之前是 Wi-Fi 连接问题。
        const WizardStep step = state_.step();
        WizardErrorCode code = WizardErrorCode::kWifiConnectFailed;
        if (step == WizardStep::kGotIp) {
            code = WizardErrorCode::kTcpHandoffFailed;
        } else if (step == WizardStep::kTcpConnected || step == WizardStep::kFullResync) {
            code = WizardErrorCode::kFullResyncFailed;
        }
        state_.markError(code);
    } else {
        return;  // 已离开异步步（防御）
    }
    clearSecrets();
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
            // M7-G（B4）：bootstrap 步进入时启动看门狗 + 已连接/能力缓存自动前进。
            if (state_.step() == WizardStep::kConnectUart ||
                state_.step() == WizardStep::kReadCapabilities) {
                enterBootstrapStep();
            } else if (state_.step() == WizardStep::kScan) {
                stopWatchdog();  // 手动进入扫描页：扫描看门狗由 startScan 管理
            }
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
    const WizardStep prev = state_.step();
    if (!state_.back()) {
        return;
    }
    // M7-G（B1）：扫描中返回 → 扫描事务取消收敛（不得停在 SCANNING）。
    if (prev == WizardStep::kScan && (scanInFlight_ || scanResultPending_)) {
        stopWatchdog();
        scanInFlight_ = false;
        scanResultPending_ = false;
        scanDisplayState_ = ScanDisplayState::kIdle;
    }
    // bootstrap 步看门狗按当前步重启（返回 Step 1/2 同样受超时保护）。
    const WizardStep step = state_.step();
    if (step == WizardStep::kConnectUart) {
        startWatchdog(kBootstrapWatchdogMs);
    } else if (step == WizardStep::kReadCapabilities) {
        startWatchdog(kCapabilityWatchdogMs);
    } else {
        stopWatchdog();
    }
    refreshUi();
}

void WifiWizardDialog::onCancel() {
    // M7-G（B2/B6）：真取消 = 清 Worker pending 队列（未发送密码副本由 Worker
    // 侧安全擦除）+ 通知 ESP32 撤销（WIFI_CLEAR）+ 状态回可编辑。
    cancelAsyncFlow();
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
        stopWatchdog();  // M7-G：能力已到 → 停 bootstrap 看门狗（扫描流程自管）
        refreshUi();
    }
}

void WifiWizardDialog::onStatusChanged(quint64 sessionId, WorkerStatus status,
                                   const QString& text) {
    Q_UNUSED(text);
    // M8-A5（HOSTUART-03）：旧会话滞留队列的 status 不改向导状态（epoch 单调）。
    if (sessionId != 0 && sessionId < manager_.sessionId()) {
        return;
    }
    // M7-G：会话已连接（含打开向导前已连接）→ Step 1 自动前进 + 能力缓存自动前进。
    if (status == WorkerStatus::Connected && state_.step() == WizardStep::kConnectUart) {
        state_.next();
        enterBootstrapStep();
        return;
    }
    // M7-F/M7-G：传输层 Error 必须收敛（禁止向导永久卡死）。
    if (status == WorkerStatus::Error) {
        stopWatchdog();
        if (state_.step() == WizardStep::kConnectUart) {
            // UART 打不开/设备丢失 → 明确“bootstrap 不可用”，不伪装成密码错误。
            state_.markError(WizardErrorCode::kUartBootstrapUnavailable);
        } else if (state_.step() == WizardStep::kReadCapabilities) {
            // M7-G（B4）：Step 2 期间链路错误 → 能力读取失败（原为死路径）。
            state_.markError(WizardErrorCode::kCapabilityReadFailed);
        } else if (state_.step() == WizardStep::kApplying) {
            // Apply 尚未 ACK：UART 掉线 = 无法确认配置送达 → 失败。
            state_.markError(WizardErrorCode::kUartConnectFailed);
        } else {
            // kConnecting..kFullResync：UART 掉线可容忍（AF.4：配网后可断开
            // 物理通道；TCP handoff 观察器接管，看门狗仍兜底）。
            return;
        }
        clearSecrets();
        refreshUi();
    } else if (status == WorkerStatus::Disconnected &&
               state_.step() == WizardStep::kApplying) {
        // Apply 尚未 ACK 时断线：不再等待（看门狗同义，立即失败）。
        stopWatchdog();
        state_.markError(WizardErrorCode::kUartConnectFailed);
        clearSecrets();
        refreshUi();
    }
}

void WifiWizardDialog::onWifiScanResult(const espview::proto::WifiScanResultInfo& result) {
    // M7-F/M7-G（B6）：仅在“等待 SCAN_RESULT”时消费；迟到/未请求/重复一律
    // 忽略——不得用过期数据覆盖已展示列表。seq 基线跨扫描保留（新扫描首结果
    // 必须与上次消费 seq 不同才接受，天然过滤上轮迟到重发）。
    if (state_.step() != WizardStep::kScan &&
        state_.step() != WizardStep::kSelectSsid) {
        return;
    }
    if (!scanResultPending_) {
        return;
    }
    if (scanSeqValid_ && result.scanSeq == lastScanSeq_) {
        return;
    }
    scanSeqValid_ = true;
    lastScanSeq_ = result.scanSeq;
    scanResultPending_ = false;
    stopWatchdog();  // M7-F：扫描完成
    scanEntries_.clear();
    for (const espview::proto::WifiScanRecordInfo& rec : result.records) {
        ScanEntry e;
        e.ssid = rec.ssid;
        e.display = QString::fromUtf8(rec.ssid.c_str(), static_cast<int>(rec.ssid.size())) +
                    tr_("[RSSI %1 dBm · ch %2]")
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
    if (ok) {
        // ACK 已接受：仍在等待 SCAN_RESULT（B1：看门狗继续，扫描未结束）。
        scanResultPending_ = true;
        refreshUi();
        return;
    }
    stopWatchdog();  // ACK 失败：收敛（显示已恢复，可重试）
    scanResultPending_ = false;
    scanDisplayState_ = ScanDisplayState::kFailed;
    // AF.3 探针语义：ACK ERR kInvalidParam = 老固件不支持 Wi-Fi provisioning。
    lastScanFirmwareUnsupported_ =
        (errorCode == static_cast<quint16>(espview::proto::ErrorCode::kInvalidParam));
    state_.markError(WizardErrorCode::kScanFailed);
    refreshUi();
}

void WifiWizardDialog::onWifiConfigAck(bool ok, quint16 errorCode) {
    Q_UNUSED(errorCode);
    if (ok) {
        state_.markConnecting();
    } else {
        state_.markError(WizardErrorCode::kApplyFailed);
        clearSecrets();
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
                startWatchdog(kResyncWatchdogMs);  // M7-F：等待首帧 FULL resync
                maybeCompleteResync();  // M7-G：TCP 首帧可能已先到（幂等）
                refreshUi();
            }
            break;
        case espview::proto::WifiStatusPhase::kError:
            if (state_.step() == WizardStep::kConnecting ||
                state_.step() == WizardStep::kGotIp ||
                state_.step() == WizardStep::kTcpConnected) {
                stopWatchdog();
                // M7-G（B5）：errorCode 必须映射为不同用户可见错误（不塌缩成
                // kWifiConnectFailed）：认证 / AP 未找到 / DHCP 超时 / server 不可达。
                const auto err = static_cast<espview::proto::ErrorCode>(status.errorCode);
                WizardErrorCode code = WizardErrorCode::kWifiConnectFailed;
                switch (err) {
                    case espview::proto::ErrorCode::kAuthFailed:
                        code = WizardErrorCode::kAuthFailed;
                        break;
                    case espview::proto::ErrorCode::kApNotFound:
                        code = WizardErrorCode::kApNotFound;
                        break;
                    case espview::proto::ErrorCode::kDhcpTimeout:
                        code = WizardErrorCode::kDhcpTimeout;
                        break;
                    case espview::proto::ErrorCode::kServerUnreachable:
                        code = WizardErrorCode::kServerUnreachable;
                        break;
                    default:
                        code = WizardErrorCode::kWifiConnectFailed;
                        break;
                }
                state_.markError(code);
                clearSecrets();
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
    // M7-G：统一经 maybeCompleteResync（TCP 观察器帧 / UART 帧均可驱动；
    // 以真实帧提交为准，不伪造 PASS）。
    if (tcpConnectedArmed_ &&
        (state_.step() == WizardStep::kTcpConnected ||
         state_.step() == WizardStep::kFullResync)) {
        tcpFramePending_ = true;
        maybeCompleteResync();
    }
}

// ---- M7-G：bootstrap 自动前进 + TCP handoff 观察器接线 ----

void WifiWizardDialog::enterBootstrapStep() {
    const WizardStep step = state_.step();
    if (step == WizardStep::kConnectUart) {
        startWatchdog(kBootstrapWatchdogMs);  // B4：Step 1 超时收敛
    } else if (step == WizardStep::kReadCapabilities) {
        startWatchdog(kCapabilityWatchdogMs);  // B4：Step 2 超时收敛
    }
    maybeAdvanceBootstrap();
    refreshUi();
}

void WifiWizardDialog::maybeAdvanceBootstrap() {
    // 会话已连接（含打开向导前已连接）→ Step 1 自动前进（statusChanged 事件可能已错过）。
    if (state_.step() == WizardStep::kConnectUart &&
        manager_.status() == WorkerStatus::Connected) {
        state_.next();
        startWatchdog(kCapabilityWatchdogMs);  // 进入 Step 2
    }
    // CAPABILITIES 已收到（缓存；能力事件可能已错过）→ Step 2 自动前进。
    espview::proto::CapabilitiesInfo caps;
    if (state_.step() == WizardStep::kReadCapabilities &&
        manager_.lastCapabilities(caps)) {
        state_.next();
        stopWatchdog();  // 进入 Step 3（扫描流程自己管理看门狗）
    }
}

void WifiWizardDialog::startHandoffListener() {
    stopHandoffListener();  // 幂等
    if (!state_.isApplying()) {
        return;
    }
    handoffObserver_ = new TcpHandoffObserver(this);
    connect(handoffObserver_, &TcpHandoffObserver::handshakeConnected, this,
            &WifiWizardDialog::onTcpHandshakeConnected);
    connect(handoffObserver_, &TcpHandoffObserver::firstFrameCommitted, this,
            &WifiWizardDialog::onTcpFirstFrame);
    connect(handoffObserver_, &TcpHandoffObserver::failed, this,
            &WifiWizardDialog::onTcpHandoffFailed);
    connect(handoffObserver_, &TcpHandoffObserver::linkLost, this,
            &WifiWizardDialog::onTcpLinkLost);
    if (!handoffObserver_->start(static_cast<uint16_t>(state_.tcpServerPort()),
                                 kHandoffBind)) {
        // PC 无法监听配置端口 → TCP handoff 无法进行（诚实报错，不伪造 PASS）。
        stopHandoffListener();
        if (state_.isApplying()) {
            state_.markError(WizardErrorCode::kTcpHandoffFailed);
            clearSecrets();
            refreshUi();
        }
    }
}

void WifiWizardDialog::stopHandoffListener() {
    if (handoffObserver_ != nullptr) {
        handoffObserver_->stop();  // join（此后无任何 signal）
        delete handoffObserver_;
        handoffObserver_ = nullptr;
    }
}

void WifiWizardDialog::cancelAsyncFlow() {
    stopWatchdog();
    stopHandoffListener();
    if (state_.isApplying()) {
        // M7-G（B2/B6）：真取消 = 清 Worker pending 队列（未发送密码副本由
        // Worker 侧安全擦除）+ 通知 ESP32 撤销（WIFI_CLEAR）+ 状态回可编辑。
        manager_.clearWifiQueue();
        manager_.sendWifiClear();
        state_.cancelApplying();
    } else if (state_.step() == WizardStep::kScan &&
               (scanInFlight_ || scanResultPending_)) {
        // M7-G（B1）：扫描中取消 → 扫描事务收敛。
        stopWatchdog();
        scanInFlight_ = false;
        scanResultPending_ = false;
        scanDisplayState_ = ScanDisplayState::kIdle;
    }
    clearSecrets();
}

void WifiWizardDialog::onTcpHandshakeConnected() {
    // TCP 连接建立 = 设备已取得 IP（无 IP 无法建连）；若 WIFI_STATUS kGotIp
    // 事件尚未到达（竞态），由真实握手推进 Step 9，不伪造。
    if (state_.step() == WizardStep::kConnecting) {
        state_.markGotIp();
    }
    if (state_.step() == WizardStep::kGotIp) {
        state_.markTcpConnected();
        tcpConnectedArmed_ = true;
        startWatchdog(kResyncWatchdogMs);  // 等待首帧 FULL（TCP 或 UART）
        maybeCompleteResync();
        refreshUi();
    }
}

void WifiWizardDialog::onTcpFirstFrame() {
    tcpFramePending_ = true;
    maybeCompleteResync();
}

void WifiWizardDialog::maybeCompleteResync() {
    if (!tcpFramePending_) {
        return;
    }
    if (state_.step() != WizardStep::kTcpConnected &&
        state_.step() != WizardStep::kFullResync) {
        return;  // 首帧早于握手事件到达：等 onTcpHandshakeConnected 再结算
    }
    tcpFramePending_ = false;
    tcpConnectedArmed_ = false;
    state_.markFullResync();
    state_.markDone();
    stopWatchdog();
    stopHandoffListener();
    clearSecrets();
    refreshUi();
}

void WifiWizardDialog::onTcpHandoffFailed(const QString& reason) {
    Q_UNUSED(reason);  // 动态诊断保留英文 raw（不落 i18n）
    if (!state_.isApplying()) {
        return;
    }
    stopWatchdog();
    state_.markError(WizardErrorCode::kTcpHandoffFailed);
    clearSecrets();
    refreshUi();
}

void WifiWizardDialog::onTcpLinkLost() {
    if (!state_.isApplying()) {
        return;
    }
    stopWatchdog();
    // 握手后断开 = resync 链路丢失；握手前断开 = handoff 失败。
    const WizardStep step = state_.step();
    const WizardErrorCode code =
        (step == WizardStep::kTcpConnected || step == WizardStep::kFullResync)
            ? WizardErrorCode::kFullResyncFailed
            : WizardErrorCode::kTcpHandoffFailed;
    state_.markError(code);
    clearSecrets();
    refreshUi();
}

void WifiWizardDialog::closeEvent(QCloseEvent* event) {
    cancelAsyncFlow();
    QDialog::closeEvent(event);
}

void WifiWizardDialog::setUiLanguage(int lang) {
    lang_ = static_cast<UiLang>(lang);
    refreshUi();
}

}  // namespace pc
}  // namespace espview

// Q_OBJECT 类（TcpHandoffObserver）定义于本文件：AUTOMOC 要求末尾包含 moc。
#include "wifi_wizard_dialog.moc"
