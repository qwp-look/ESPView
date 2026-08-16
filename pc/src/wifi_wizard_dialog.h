// ESPView M7-D4 — WifiWizardDialog：Wi-Fi 配网向导（Qt Widgets，UI 层）。
//
// 规范来源：docs/DESIGN.md AG 节（M7-D4 Wizard UX）+ M7-D 任务书 D4 步骤 1..11。
//
// 职责与约束：
//   - 只做 UI 编排：驱动纯 C++17 WifiWizardState（导航 / 校验 / 状态转换），
//     消费 ConnectionManager 的 M7-D3 信号（wifiScanResult / wifiStatus /
//     wifiScanReqAck / wifiConfigAck / capabilitiesReceived / statusChanged /
//     frameReady）；不解析任何 wire 字节；
//   - 凭据安全（AF.4 / AG.3）：密码只驻留 QLineEdit + WifiWizardState 内存；
//     Apply 发送后、对话框关闭 / 取消 / 完成时立即清零；绝不写入 QSettings /
//     日志 / 标题栏 / 状态栏；开放网络（空密码）经显式勾选；
//   - 双语：全部文案经 trText(lang, key)（key = 英文原文；i18n.cpp 词条已冻结）；
//   - 不负责 Transport 切换（UART bootstrap → TCP handoff 属 D6 集成）。
//
// 集成（main.cpp）：File/Tools 菜单动作 → 构造本对话框（传 ConnectionManager&）
// 并以模态 exec() 打开；语言以 setUiLanguage 传入（对话框自身只读语言参数）。

#pragma once

#include <QDialog>
#include <QString>
#include <vector>

#include "connection_manager.h"
#include "display_frame.h"
#include "i18n.h"
#include "wifi_wizard_state.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimer;

namespace espview {
namespace pc {

class WifiWizardDialog : public QDialog {
    Q_OBJECT
public:
    explicit WifiWizardDialog(ConnectionManager& manager, QWidget* parent = nullptr);

    // 语言切换（与 MainWindow 共用 trText 目录；切换后重刷全部文案）。
    void setUiLanguage(int lang);

protected:
    void closeEvent(QCloseEvent* event) override;  // 关闭即清零密码

private:
    void buildUi();
    void refreshUi();
    void clearSecrets();
    void onNext();
    void onBack();
    void onCancel();
    void onRetry();
    void onScanClicked();
    void onPasswordChanged(const QString& text);
    void onSsidChanged(const QString& text);
    void onOpenNetworkToggled(bool checked);
    void onTcpIpEdited();
    void onTcpPortChanged(int value);
    void onScanListSelectionChanged();

    QString tr_(const char* key) const;
    void setHint(const char* key);
    void populateScanList();
    void startScan();
    void applyConfig();

    // M7-D3 信号（queued 连接在 ctor 建立；GUI 线程消费）。
    void onCapabilitiesReceived(const espview::proto::CapabilitiesInfo& caps);
    void onStatusChanged(WorkerStatus status, const QString& text);
    void onWifiScanResult(const espview::proto::WifiScanResultInfo& result);
    void onWifiScanReqAck(bool ok, quint16 errorCode);
    void onWifiConfigAck(bool ok, quint16 errorCode);
    void onWifiStatus(const espview::proto::WifiStatusInfo& status);
    void onFrameReady(const DisplayFrame& frame);

    // M7-F：异步步看门狗（扫描 / Apply 链 / FULL resync 超时收敛，禁止挂死）。
    void startWatchdog(int ms);
    void stopWatchdog();
    void onWatchdogTimeout();

    ConnectionManager& manager_;
    WifiWizardState state_;
    UiLang lang_ = UiLang::kEnglish;
    bool scanInFlight_ = false;
    bool lastScanFirmwareUnsupported_ = false;
    bool tcpConnectedArmed_ = false;  // kTcpConnected 起等待第一帧完成 FULL resync
    QTimer* watchdogTimer_ = nullptr;  // M7-F：异步步超时（单发）
    bool scanSeqValid_ = false;        // M7-F：是否已收到至少一次扫描结果（seq 过滤基）
    uint8_t lastScanSeq_ = 0;          // M7-F：最近消费的扫描结果 seq（迟到/重复忽略）

    // M7-E：扫描页显示状态（扫描期间 OLED 显示临时暂停；完成/失败后恢复）。
    enum class ScanDisplayState {
        kIdle,       // 未在扫描（默认态）
        kScanning,   // 扫描进行中：显示临时暂停
        kSucceeded,  // 扫描完成：显示已恢复
        kFailed,     // 扫描失败：显示已恢复（可重试）
    };
    ScanDisplayState scanDisplayState_ = ScanDisplayState::kIdle;

    // 页面控件
    QLabel* title_ = nullptr;
    QLabel* hint_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    QPushButton* backBtn_ = nullptr;
    QPushButton* nextBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;

    QPushButton* scanBtn_ = nullptr;
    QLabel* scanStatusLabel_ = nullptr;  // M7-E：扫描期间显示暂停/恢复状态
    QListWidget* scanList_ = nullptr;
    QComboBox* ssidCombo_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QCheckBox* openNetCheck_ = nullptr;
    QLineEdit* serverIpEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QLabel* asyncStatusLabel_ = nullptr;
    QLabel* doneLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;

    // 最近一次扫描结果（kScan / kSelectSsid 两页共用）。
    struct ScanEntry {
        std::string ssid;
        QString display;
    };
    std::vector<ScanEntry> scanEntries_;
};

}  // namespace pc
}  // namespace espview
