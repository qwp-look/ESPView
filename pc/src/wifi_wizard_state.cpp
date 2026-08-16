// ESPView M7-D — WifiWizardState 实现（纯 C++17，零 Qt / 零协议 wire 依赖）。
// 规范来源见 wifi_wizard_state.h；本文件只做状态转换与本地输入校验，
// 不接触协议消息（WIFI_SCAN / WIFI_CONFIG / WIFI_STATUS 尚未冻结）。

#include "wifi_wizard_state.h"

#include <algorithm>

namespace espview {
namespace pc {

namespace {

// 802.11 SSID 可见字节：0x20..0x7E（可打印 ASCII）。
bool isVisibleSsidByte(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x20 && u <= 0x7E;
}

// IPv4 点分十进制严格校验：恰好 4 段，每段 1..3 位十进制数字，值 0..255。
bool isIpv4DottedDecimal(const std::string& s) {
    if (s.empty()) {
        return false;
    }
    int octets = 0;
    int value = 0;
    int digits = 0;
    for (char c : s) {
        if (c == '.') {
            if (digits == 0) {
                return false;  // 空段（".." 或首/尾点）
            }
            ++octets;
            value = 0;
            digits = 0;
            continue;
        }
        if (c < '0' || c > '9') {
            return false;  // 非数字字符（含符号/空格/字母）
        }
        value = value * 10 + (c - '0');
        if (value > 255) {
            return false;
        }
        ++digits;
        if (digits > 3) {
            return false;
        }
    }
    if (digits == 0) {
        return false;
    }
    return octets == 3;  // 4 段（最后一个点在循环内计数）
}

}  // namespace

// ---- key 目录（英文原文，i18n key 即 msgid；i18n.cpp 词条由另一代理补充）----

const char* wizardStepTitleKey(WizardStep step) {
    switch (step) {
        case WizardStep::kInit:
            return "Wi-Fi Wizard";
        case WizardStep::kConnectUart:
            return "Connect ESP32";
        case WizardStep::kReadCapabilities:
            return "Read Capabilities";
        case WizardStep::kScan:
            return "Scan Wi-Fi";
        case WizardStep::kSelectSsid:
            return "Select SSID";
        case WizardStep::kPassword:
            return "Enter Password";
        case WizardStep::kTcpConfig:
            return "TCP Server Config";
        case WizardStep::kApplying:
            return "Applying";
        case WizardStep::kConnecting:
            return "Connecting to Wi-Fi";
        case WizardStep::kGotIp:
            return "Got IP";
        case WizardStep::kTcpConnected:
            return "TCP Connected";
        case WizardStep::kFullResync:
            return "Full Resync";
        case WizardStep::kDone:
            return "Done";
        case WizardStep::kError:
            return "Error";
    }
    return "Error";  // 防御：非法枚举值
}

const char* wizardErrorKey(WizardErrorCode code) {
    switch (code) {
        case WizardErrorCode::kSsidMissing:
            return "SSID is required";
        case WizardErrorCode::kSsidTooLong:
            return "SSID too long (max 32 characters)";
        case WizardErrorCode::kSsidInvalidChars:
            return "SSID contains invalid characters";
        case WizardErrorCode::kPasswordTooShort:
            return "Password too short (min 8 characters)";
        case WizardErrorCode::kPasswordTooLong:
            return "Password too long (max 63 characters)";
        case WizardErrorCode::kInvalidTcpServerIp:
            return "Invalid TCP server IP";
        case WizardErrorCode::kInvalidTcpServerPort:
            return "TCP port must be 1..65535";
        case WizardErrorCode::kUartConnectFailed:
            return "UART connection failed";
        case WizardErrorCode::kCapabilityReadFailed:
            return "Capability read failed";
        case WizardErrorCode::kScanFailed:
            return "Wi-Fi scan failed";
        case WizardErrorCode::kApplyFailed:
            return "Apply failed";
        case WizardErrorCode::kWifiConnectFailed:
            return "Wi-Fi connection failed";
        case WizardErrorCode::kNoIpReceived:
            return "No IP address received";
        case WizardErrorCode::kTcpConnectFailed:
            return "TCP connection failed";
        case WizardErrorCode::kFullResyncFailed:
            return "Full resync failed";
        case WizardErrorCode::kUartBootstrapUnavailable:
            return "UART bootstrap unavailable";
        case WizardErrorCode::kTcpHandoffFailed:
            return "TCP handoff failed (Wi-Fi connected)";
        case WizardErrorCode::kAuthFailed:
            return "Wi-Fi authentication failed";
        case WizardErrorCode::kApNotFound:
            return "Wi-Fi network not found";
        case WizardErrorCode::kDhcpTimeout:
            return "DHCP timeout - no IP address";
        case WizardErrorCode::kServerUnreachable:
            return "TCP server unreachable";
        case WizardErrorCode::kNone:
            break;
    }
    return "";  // 防御：非法枚举值
}

// ---- 状态机 ----

WizardError WifiWizardState::makeError(WizardErrorCode code) {
    return WizardError{code, wizardErrorKey(code)};
}

bool WifiWizardState::isEditable() const {
    return step_ <= WizardStep::kTcpConfig || step_ == WizardStep::kError;
}

bool WifiWizardState::isApplying() const {
    return step_ >= WizardStep::kApplying && step_ <= WizardStep::kFullResync;
}

WizardError WifiWizardState::error() const {
    return step_ == WizardStep::kError ? makeError(error_) : WizardError{};
}

bool WifiWizardState::next() {
    if (!isEditable() || validationError()) {
        return false;
    }
    switch (step_) {
        case WizardStep::kInit:
            step_ = WizardStep::kConnectUart;
            break;
        case WizardStep::kConnectUart:
            step_ = WizardStep::kReadCapabilities;
            break;
        case WizardStep::kReadCapabilities:
            step_ = WizardStep::kScan;
            break;
        case WizardStep::kScan:
            step_ = WizardStep::kSelectSsid;
            break;
        case WizardStep::kSelectSsid:
            step_ = WizardStep::kPassword;
            break;
        case WizardStep::kPassword:
            step_ = WizardStep::kTcpConfig;
            break;
        default:
            return false;  // kTcpConfig 之后必须走 beginApply；kApplying+ / 终态不可 next
    }
    return true;
}

bool WifiWizardState::back() {
    if (!isEditable()) {
        return false;
    }
    switch (step_) {
        case WizardStep::kInit:
            return false;
        case WizardStep::kConnectUart:
            step_ = WizardStep::kInit;
            break;
        case WizardStep::kReadCapabilities:
            step_ = WizardStep::kConnectUart;
            break;
        case WizardStep::kScan:
            step_ = WizardStep::kReadCapabilities;
            break;
        case WizardStep::kSelectSsid:
            step_ = WizardStep::kScan;
            break;
        case WizardStep::kPassword:
            step_ = WizardStep::kSelectSsid;
            break;
        case WizardStep::kTcpConfig:
            step_ = WizardStep::kPassword;
            break;
        default:
            return false;  // kError：用 retry 而非 back
    }
    return true;
}

bool WifiWizardState::cancel() {
    if (!isEditable()) {
        return false;  // Apply 后 / kDone 不可取消
    }
    step_ = WizardStep::kInit;
    error_ = WizardErrorCode::kNone;
    retryStep_ = WizardStep::kInit;
    ssid_.clear();
    password_.clear();
    tcpServerIp_.clear();
    tcpServerPort_ = 0;
    return true;
}

bool WifiWizardState::cancelApplying() {
    if (!isApplying()) {
        return false;  // 仅异步 Apply 链可取消（编辑态/kError 用 cancel()）
    }
    clearPassword();  // 安全擦除密码驻留副本（std::fill 清零）
    step_ = WizardStep::kInit;
    error_ = WizardErrorCode::kNone;
    retryStep_ = WizardStep::kInit;
    ssid_.clear();
    tcpServerIp_.clear();
    tcpServerPort_ = 0;
    return true;
}

bool WifiWizardState::retry() {
    if (step_ != WizardStep::kError) {
        return false;
    }
    step_ = retryStep_;
    error_ = WizardErrorCode::kNone;
    return true;
}

bool WifiWizardState::setSsid(const std::string& ssid) {
    if (!isEditable()) {
        return false;
    }
    ssid_ = ssid;
    return true;
}

bool WifiWizardState::setPassword(const std::string& password) {
    if (!isEditable()) {
        return false;
    }
    password_ = password;
    return true;
}

bool WifiWizardState::setTcpServerIp(const std::string& ip) {
    if (!isEditable()) {
        return false;
    }
    tcpServerIp_ = ip;
    return true;
}

bool WifiWizardState::setTcpServerPort(uint32_t port) {
    if (!isEditable()) {
        return false;
    }
    tcpServerPort_ = port;
    return true;
}

// ---- 派生标志（实时由输入计算，无存储位）----

bool WifiWizardState::hasSsid() const {
    if (ssid_.empty() || ssid_.size() > kMaxSsidBytes) {
        return false;
    }
    for (char c : ssid_) {
        if (!isVisibleSsidByte(c)) {
            return false;
        }
    }
    return true;
}

bool WifiWizardState::wifiPasswordValid() const {
    // 空密码 = 开放网络（WPA3/开放允许），合法；非空必须 8..63 字节（WPA2）。
    if (password_.empty()) {
        return true;
    }
    return password_.size() >= kMinWpa2PasswordBytes &&
           password_.size() <= kMaxWpa2PasswordBytes;
}

bool WifiWizardState::wifiOpenNetwork() const {
    return password_.empty();
}

bool WifiWizardState::tcpConfigValid() const {
    return isIpv4DottedDecimal(tcpServerIp_) &&
           tcpServerPort_ >= kMinTcpPort && tcpServerPort_ <= kMaxTcpPort;
}

bool WifiWizardState::canApply() const {
    return hasSsid() && wifiPasswordValid() && tcpConfigValid();
}

WizardError WifiWizardState::validationError() const {
    switch (step_) {
        case WizardStep::kSelectSsid:
            if (ssid_.empty()) {
                return makeError(WizardErrorCode::kSsidMissing);
            }
            if (ssid_.size() > kMaxSsidBytes) {
                return makeError(WizardErrorCode::kSsidTooLong);
            }
            for (char c : ssid_) {
                if (!isVisibleSsidByte(c)) {
                    return makeError(WizardErrorCode::kSsidInvalidChars);
                }
            }
            return WizardError{};
        case WizardStep::kPassword:
            if (!password_.empty() && password_.size() < kMinWpa2PasswordBytes) {
                return makeError(WizardErrorCode::kPasswordTooShort);
            }
            if (password_.size() > kMaxWpa2PasswordBytes) {
                return makeError(WizardErrorCode::kPasswordTooLong);
            }
            return WizardError{};
        case WizardStep::kTcpConfig:
            if (!isIpv4DottedDecimal(tcpServerIp_)) {
                return makeError(WizardErrorCode::kInvalidTcpServerIp);
            }
            if (tcpServerPort_ < kMinTcpPort || tcpServerPort_ > kMaxTcpPort) {
                return makeError(WizardErrorCode::kInvalidTcpServerPort);
            }
            return WizardError{};
        default:
            return WizardError{};
    }
}

// ---- Apply / 异步流程 ----

bool WifiWizardState::beginApply() {
    if (step_ != WizardStep::kTcpConfig || !canApply()) {
        return false;
    }
    step_ = WizardStep::kApplying;
    // M7-F：不再在此清除密码（发送方消费后经 clearPassword() 清除）。
    return true;
}

void WifiWizardState::clearPassword() {
    if (password_.empty()) {
        return;
    }
    std::fill(password_.begin(), password_.end(), '\0');
    password_.clear();
}

bool WifiWizardState::markConnecting() {
    if (step_ != WizardStep::kApplying) {
        return false;
    }
    step_ = WizardStep::kConnecting;
    return true;
}

bool WifiWizardState::markGotIp() {
    if (step_ != WizardStep::kConnecting) {
        return false;
    }
    step_ = WizardStep::kGotIp;
    return true;
}

bool WifiWizardState::markTcpConnected() {
    if (step_ != WizardStep::kGotIp) {
        return false;
    }
    step_ = WizardStep::kTcpConnected;
    return true;
}

bool WifiWizardState::markFullResync() {
    if (step_ != WizardStep::kTcpConnected) {
        return false;
    }
    step_ = WizardStep::kFullResync;
    return true;
}

bool WifiWizardState::markDone() {
    if (step_ != WizardStep::kFullResync) {
        return false;
    }
    step_ = WizardStep::kDone;
    return true;
}

void WifiWizardState::markError(WizardErrorCode code) {
    if (code == WizardErrorCode::kNone) {
        return;  // 防御：空错误不进入错误态
    }
    error_ = code;
    // retry 目标：预 Apply 操作步（Step 1..3）回到原步重试；Apply / 异步步
    // （Step 7..11）回到 kTcpConfig 重新应用。
    retryStep_ = (step_ <= WizardStep::kScan) ? step_ : WizardStep::kTcpConfig;
    step_ = WizardStep::kError;
}

// ---- 持久化导出（严禁密码）----

std::map<std::string, std::string> WifiWizardState::toSettingsMap(bool includeSsid) const {
    std::map<std::string, std::string> settings;
    settings["tcpServerIp"] = tcpServerIp_;
    settings["tcpServerPort"] = std::to_string(tcpServerPort_);
    if (includeSsid) {
        settings["ssid"] = ssid_;
    }
    // 密码永不出现：本模型没有任何密码序列化路径（含密码在内的凭据由
    // 后续里程碑仅经内存传递到 wire 构造，wire 本身尚未冻结）。
    return settings;
}

}  // namespace pc
}  // namespace espview