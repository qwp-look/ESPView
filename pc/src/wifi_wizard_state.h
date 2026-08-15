// ESPView M7-D — Wi-Fi Wizard 纯状态模型（纯 C++17，零 Qt / 零协议 wire 依赖）。
//
// 规范来源：M7-D 任务书：
//   - 步骤：UART Bootstrap → 向导 Step 1 连接 ESP32 → Step 2 读取能力 →
//     Step 3 扫描 Wi-Fi → Step 4 选 SSID → Step 5 密码输入 → Step 6 TCP server
//     配置 → Step 7 Apply → Step 8 ESP32 连接中 → Step 9 GOT_IP → Step 10
//     TCP Connected → Step 11 FULL resync → Done；失败进入 Error（可 Retry）。
//   - 本文件只做 UI 状态建模与本地输入校验；不接触协议消息（WIFI_SCAN /
//     WIFI_CONFIG / WIFI_STATUS 尚未冻结），协议 wire 由后续里程碑实现。
//   - 密码永不持久化：密码仅驻留内存，Apply 成功后即清除；toSettingsMap 只导出
//     SSID（可选 metadata，默认不保存）/ TCP IP / port，任何情况下都不含密码键；
//     本模型不提供任何密码序列化 API。
//   - 所有用户可见字符串以 i18n key（英文原文）表示，经 trText(lang, key)
//     渲染；i18n.cpp 词条由另一代理统一补充（本文件只引用 key，不依赖其存在）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace espview {
namespace pc {

// 向导步骤（数值序即向导主序；kDone / kError 为终态）。
enum class WizardStep : int {
    kInit = 0,              // 初始页（向导说明 / UART bootstrap 准备）
    kConnectUart = 1,       // Step 1：连接 ESP32
    kReadCapabilities = 2,  // Step 2：读取能力
    kScan = 3,              // Step 3：扫描 Wi-Fi
    kSelectSsid = 4,        // Step 4：选择 SSID
    kPassword = 5,          // Step 5：密码输入
    kTcpConfig = 6,         // Step 6：TCP server 配置
    kApplying = 7,          // Step 7：Apply（不可编辑）
    kConnecting = 8,        // Step 8：ESP32 连接中
    kGotIp = 9,             // Step 9：GOT_IP
    kTcpConnected = 10,     // Step 10：TCP Connected
    kFullResync = 11,       // Step 11：FULL resync
    kDone = 12,             // 成功
    kError = 13,            // 失败（可 Retry）
};

// 稳定错误码：1..7 为本地输入校验错误，10..17 为运行期（后续协议回调）错误。
enum class WizardErrorCode : int {
    kNone = 0,
    // ---- 输入校验 ----
    kSsidMissing = 1,          // 未选择 SSID
    kSsidTooLong = 2,          // > 32 字节
    kSsidInvalidChars = 3,     // 含非可见字节
    kPasswordTooShort = 4,     // 非空且 < 8 字节
    kPasswordTooLong = 5,      // > 63 字节
    kInvalidTcpServerIp = 6,   // 非 IPv4 点分十进制
    kInvalidTcpServerPort = 7, // 端口不在 1..65535
    // ---- 运行期错误（后续里程碑由协议回调经 markError 上报）----
    kUartConnectFailed = 10,   // Step 1 失败
    kCapabilityReadFailed = 11,// Step 2 失败
    kScanFailed = 12,          // Step 3 失败
    kApplyFailed = 13,         // Step 7 Apply 失败
    kWifiConnectFailed = 14,   // Step 8 连接失败
    kNoIpReceived = 15,        // Step 9 未收到 GOT_IP
    kTcpConnectFailed = 16,    // Step 10 TCP 连接失败
    kFullResyncFailed = 17,    // Step 11 FULL resync 失败
};

// 错误：稳定 code + i18n key（英文原文，经 trText 渲染；code==kNone 时 key 为空）。
struct WizardError {
    WizardErrorCode code = WizardErrorCode::kNone;
    const char* i18nKey = "";

    explicit operator bool() const { return code != WizardErrorCode::kNone; }
};

// 步骤标题 i18n key（英文原文）：每个步骤都有非空标题。
const char* wizardStepTitleKey(WizardStep step);

// 错误 i18n key（英文原文）：每个错误码都有非空 key。
const char* wizardErrorKey(WizardErrorCode code);

// ---- 校验常量（802.11 / WPA2 简化规则）----
constexpr std::size_t kMaxSsidBytes = 32;        // 802.11 SSID 1..32 可见字节
constexpr std::size_t kMinWpa2PasswordBytes = 8; // 非空密码最短 8 字节
constexpr std::size_t kMaxWpa2PasswordBytes = 63;// 非空密码最长 63 字节
constexpr uint32_t kMinTcpPort = 1;
constexpr uint32_t kMaxTcpPort = 65535;

// M7-D — Wi-Fi Wizard 状态机（纯 C++17，零 Qt）。线程模型与
// DisplayUiState/SplitState 一致：GUI 线程单线程使用，零锁。
class WifiWizardState {
public:
    WifiWizardState() = default;

    // ---- 状态查询 ----
    WizardStep step() const { return step_; }
    bool isEditable() const;  // kInit..kTcpConfig，或 kError 待修复（可改输入）
    bool isFinished() const { return step_ == WizardStep::kDone || step_ == WizardStep::kError; }
    bool isApplying() const;  // kApplying..kFullResync（已锁定，不可编辑）
    WizardError error() const;  // kError 时的错误（code + key）；非错误态返回空

    // ---- 用户导航（编辑阶段）----
    // next：校验当前步骤输入；合法则前进到下一步，否则返回 false 并留在原步
    // （validationError() 说明原因）。kInit..kPassword 可 next；kTcpConfig 之后
    // 必须走 beginApply（Step 7 Apply）。
    bool next();
    // back：回上一步（kInit 时返回 false）。Apply 后 / 终态不可 back。
    bool back();
    // cancel：取消向导 → 重置 kInit 并清空全部输入（编辑阶段 / kError 可用）。
    bool cancel();
    // retry：kError → 回到出错步骤（retry 目标），清除错误；非错误态返回 false。
    bool retry();

    // ---- 输入（编辑阶段；Apply 后返回 false 且不改状态）----
    // 校验在 validationError() / next() / beginApply() 按需进行，setter 不做
    // 阻断式校验（GUI 需自由输入并实时显示内联错误）。
    bool setSsid(const std::string& ssid);
    bool setPassword(const std::string& password);  // 仅驻留内存；Apply 成功后清除
    bool setTcpServerIp(const std::string& ip);
    bool setTcpServerPort(uint32_t port);

    // 只读访问。password() 仅供 Apply 前的（未来）wire 构造使用，绝不进入
    // toSettingsMap，模型也不提供任何密码序列化 API。
    const std::string& ssid() const { return ssid_; }
    const std::string& password() const { return password_; }
    const std::string& tcpServerIp() const { return tcpServerIp_; }
    uint32_t tcpServerPort() const { return tcpServerPort_; }

    // ---- 派生标志（与密码值分离：实时由当前输入计算，无存储位可被篡改）----
    bool hasSsid() const;            // 已选 1..32 个可见字节的 SSID
    bool wifiPasswordValid() const;  // 空（开放网络）或 8..63 字节
    bool wifiOpenNetwork() const;    // 密码为空 → 开放网络标记
    bool tcpConfigValid() const;     // IPv4 点分十进制 + 端口 1..65535
    bool canApply() const;           // SSID + 密码 + TCP 配置全部有效

    // 当前步骤的输入校验错误；无错误返回 code==kNone。
    WizardError validationError() const;

    // ---- Apply / 异步流程推进 ----
    // beginApply：kTcpConfig → kApplying（校验 canApply）；成功后立即清除密码
    // （密码仅在 Apply 前驻留内存）。返回 false 表示不在 kTcpConfig 或输入非法。
    bool beginApply();
    // 以下推进由后续里程碑的协议回调驱动（本模型只做状态转换，零协议依赖）：
    // kApplying → kConnecting → kGotIp → kTcpConnected → kFullResync → kDone。
    // 每个推进只在“当前正是其前置步骤”时成功，否则返回 false（状态不变）。
    bool markConnecting();
    bool markGotIp();
    bool markTcpConnected();
    bool markFullResync();
    bool markDone();
    // 任意步骤失败 → kError（记录 code + retry 目标）。预 Apply 操作步
    // （Step 1..3）retry 回到原步；Apply/异步步（Step 7..11）retry 回到
    // kTcpConfig 重新应用。kNone 忽略（防御）。
    void markError(WizardErrorCode code);

    // ---- 持久化导出（严禁密码）----
    // 只导出 TCP server IP/port；includeSsid=true 时额外导出 SSID（可选
    // metadata，GUI 默认不保存）。返回键集合：{tcpServerIp, tcpServerPort[, ssid]}。
    // 密码永不出现：本模型不存在任何密码序列化路径。
    std::map<std::string, std::string> toSettingsMap(bool includeSsid = false) const;

private:
    static WizardError makeError(WizardErrorCode code);

    WizardStep step_ = WizardStep::kInit;
    WizardErrorCode error_ = WizardErrorCode::kNone;
    WizardStep retryStep_ = WizardStep::kInit;  // kError 时的 retry 目标
    std::string ssid_;
    std::string password_;  // 仅内存驻留；Apply 成功后清除
    std::string tcpServerIp_;
    uint32_t tcpServerPort_ = 0;
};

}  // namespace pc
}  // namespace espview