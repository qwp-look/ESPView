// ESPView M7-D — WifiWizardState host tests（纯 C++17，零 Qt / 零协议 wire 依赖）。
//
// 规范来源：M7-D 任务书：
//   - 步骤流：next / back / cancel / retry；
//   - SSID 校验：空 / 1 / 32 / 33 字节（802.11 SSID 1..32 可见字节）；
//   - 密码校验：7 / 8 / 63 / 64 字节（非空 8..63；空 = 开放网络，标记 open network）；
//   - TCP IP / port 校验：IPv4 点分十进制；端口 0 / 65536 非法、1 / 65535 合法；
//   - Apply 后不可编辑（直到 Done / Error）；
//   - 错误恢复（code + i18n key + Retry）；
//   - toSettingsMap 无 password 键（关键断言：密码永不持久化）。
//
// 使用 shared/protocol/tests/test_util.h 的 CHECK / CHECK_EQ 框架，经
// test_main.cpp 登记进协议套件二进制（与 display/transport/oled 纯模型测试一致）。

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "test_util.h"
#include "wifi_wizard_state.h"

namespace {

using espview::pc::WifiWizardState;
using espview::pc::WizardErrorCode;
using espview::pc::WizardStep;
using espview::pc::kMaxSsidBytes;
using espview::pc::kMaxTcpPort;
using espview::pc::kMaxWpa2PasswordBytes;
using espview::pc::kMinTcpPort;
using espview::pc::kMinWpa2PasswordBytes;
using espview::pc::wizardErrorKey;
using espview::pc::wizardStepTitleKey;

const std::string kValidSsid = "MyWiFi";
const std::string kValidPassword = "secret123";  // 8 字节
const std::string kValidIp = "192.168.1.1";
constexpr uint32_t kValidPort = 8765;

// 走到 kTcpConfig 且输入全部合法的便利函数。
WifiWizardState makeReadyToApply() {
    WifiWizardState s;
    s.next();  // kInit -> kConnectUart
    s.next();  // kConnectUart -> kReadCapabilities
    s.next();  // kReadCapabilities -> kScan
    s.next();  // kScan -> kSelectSsid
    s.setSsid(kValidSsid);
    s.next();  // kSelectSsid -> kPassword
    s.setPassword(kValidPassword);
    s.next();  // kPassword -> kTcpConfig
    s.setTcpServerIp(kValidIp);
    s.setTcpServerPort(kValidPort);
    return s;
}

std::string toLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// 关键断言：toSettingsMap 的键集合永不含任何密码/凭据语义键。
void checkNoCredentialKeys(const std::map<std::string, std::string>& m) {
    static const char* kForbidden[] = {"password", "psk", "passphrase", "credential", "secret"};
    for (const auto& kv : m) {
        const std::string lower = toLowerAscii(kv.first);
        for (const char* word : kForbidden) {
            CHECK(lower.find(word) == std::string::npos);
        }
    }
}

void testStepFlow() {
    std::printf("[wifi_wizard_state] step flow\n");
    WifiWizardState s;
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kInit));
    CHECK(s.isEditable());
    CHECK(!s.isFinished());
    CHECK(!s.isApplying());

    // next 主序：kInit -> ... -> kTcpConfig
    CHECK(s.next());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kConnectUart));
    CHECK(s.next());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kReadCapabilities));
    CHECK(s.next());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kScan));
    CHECK(s.next());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kSelectSsid));

    // back：kSelectSsid -> kScan -> kReadCapabilities -> kConnectUart -> kInit
    CHECK(s.back());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kScan));
    CHECK(s.back());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kReadCapabilities));
    CHECK(s.back());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kConnectUart));
    CHECK(s.back());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kInit));
    CHECK(!s.back());  // kInit 不能 back
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kInit));

    // cancel：编辑阶段任意步 → kInit 并清空输入
    s.next();
    s.next();
    s.setSsid(kValidSsid);
    CHECK(s.ssid() == kValidSsid);
    CHECK(s.cancel());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kInit));
    CHECK(s.ssid().empty());
    CHECK(s.password().empty());
    CHECK(s.tcpServerIp().empty());
    CHECK_EQ(s.tcpServerPort(), 0u);
}

void testSsidValidation() {
    std::printf("[wifi_wizard_state] ssid validation\n");
    // 空 SSID：未选择 → next 被拦，错误 = kSsidMissing
    {
        WifiWizardState s;
        s.next();  // kConnectUart
        s.next();  // kReadCapabilities
        s.next();  // kScan
        s.next();  // kSelectSsid
        CHECK(!s.hasSsid());
        CHECK(!s.next());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kSsidMissing));
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kSelectSsid));
    }
    // 1 字节（最小合法）
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        CHECK(s.setSsid("A"));
        CHECK(s.hasSsid());
        CHECK(s.next());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kPassword));
    }
    // 32 字节（最大合法）
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        const std::string ssid32(32, 'X');
        CHECK(s.setSsid(ssid32));
        CHECK(s.hasSsid());
        CHECK(!s.validationError());
        CHECK(s.next());
    }
    // 33 字节：kSsidTooLong，next 被拦
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        const std::string ssid33(33, 'X');
        CHECK(s.setSsid(ssid33));
        CHECK(!s.hasSsid());
        CHECK(!s.next());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kSsidTooLong));
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kSelectSsid));
    }
    // 非可见字节（0x01）：kSsidInvalidChars
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        const std::string ssidBad = std::string("WiFi") + char(0x01);
        CHECK(s.setSsid(ssidBad));
        CHECK(!s.hasSsid());
        CHECK(!s.next());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kSsidInvalidChars));
    }
    // 常量自检：kMaxSsidBytes == 32
    CHECK_EQ(static_cast<long long>(kMaxSsidBytes), 32LL);
}

void testPasswordValidation() {
    std::printf("[wifi_wizard_state] password validation\n");
    // 空密码：开放网络，合法且标记 open network
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();  // kPassword
        CHECK(s.setPassword(""));
        CHECK(s.wifiPasswordValid());
        CHECK(s.wifiOpenNetwork());
        CHECK(!s.validationError());
        CHECK(s.next());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kTcpConfig));
    }
    // 7 字节：kPasswordTooShort
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        const std::string pw7(7, 'a');
        CHECK(s.setPassword(pw7));
        CHECK(!s.wifiPasswordValid());
        CHECK(!s.next());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kPasswordTooShort));
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kPassword));
    }
    // 8 字节（最小合法）
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        CHECK(s.setPassword(std::string(8, 'b')));
        CHECK(s.wifiPasswordValid());
        CHECK(!s.wifiOpenNetwork());
        CHECK(s.next());
    }
    // 63 字节（最大合法）
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        CHECK(s.setPassword(std::string(63, 'c')));
        CHECK(s.wifiPasswordValid());
        CHECK(s.next());
    }
    // 64 字节：kPasswordTooLong
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        CHECK(s.setPassword(std::string(64, 'd')));
        CHECK(!s.wifiPasswordValid());
        CHECK(!s.next());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kPasswordTooLong));
    }
    // 派生标志与密码值分离：setter 变更后标志实时重算
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        CHECK(s.setPassword(std::string(7, 'e')));
        CHECK(!s.wifiPasswordValid());
        CHECK(s.setPassword(std::string(8, 'e')));
        CHECK(s.wifiPasswordValid());
        CHECK(s.setPassword(""));
        CHECK(s.wifiOpenNetwork());
    }
    // 常量自检
    CHECK_EQ(static_cast<long long>(kMinWpa2PasswordBytes), 8LL);
    CHECK_EQ(static_cast<long long>(kMaxWpa2PasswordBytes), 63LL);
}

void testTcpConfigValidation() {
    std::printf("[wifi_wizard_state] tcp config validation\n");
    // 合法 IP（含 0.0.0.0 与 255.255.255.255 边界）
    for (const char* ip : {"192.168.1.1", "0.0.0.0", "255.255.255.255", "10.0.0.1"}) {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        s.setPassword(kValidPassword);
        s.next();  // kTcpConfig
        CHECK(s.setTcpServerIp(ip));
        CHECK(s.setTcpServerPort(kValidPort));
        CHECK(s.tcpConfigValid());
        CHECK(!s.validationError());
    }
    // 非法 IP
    for (const char* bad : {"", "256.1.1.1", "1.2.3", "1.2.3.4.5", "abc", "1.2.3.", ".1.2.3",
                            "1..2.3", "192.168.1.1 ", "1.2.3.4x", "999.1.1.1"}) {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        s.setPassword(kValidPassword);
        s.next();
        CHECK(s.setTcpServerIp(bad));
        CHECK(!s.tcpConfigValid());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kInvalidTcpServerIp));
    }
    // 端口：0 与 65536 非法；1 与 65535 合法
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        s.setPassword(kValidPassword);
        s.next();
        CHECK(s.setTcpServerIp(kValidIp));
        CHECK(s.setTcpServerPort(0));
        CHECK(!s.tcpConfigValid());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kInvalidTcpServerPort));
    }
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        s.setPassword(kValidPassword);
        s.next();
        CHECK(s.setTcpServerIp(kValidIp));
        CHECK(s.setTcpServerPort(65536));
        CHECK(!s.tcpConfigValid());
        CHECK_EQ(static_cast<int>(s.validationError().code),
                 static_cast<int>(WizardErrorCode::kInvalidTcpServerPort));
    }
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        s.setPassword(kValidPassword);
        s.next();
        CHECK(s.setTcpServerIp(kValidIp));
        CHECK(s.setTcpServerPort(kMinTcpPort));
        CHECK(s.tcpConfigValid());
        CHECK(s.setTcpServerPort(kMaxTcpPort));
        CHECK(s.tcpConfigValid());
    }
    // 常量自检
    CHECK_EQ(kMinTcpPort, 1u);
    CHECK_EQ(kMaxTcpPort, 65535u);
}

void testApplyLocksEditing() {
    std::printf("[wifi_wizard_state] apply locks editing\n");
    WifiWizardState s = makeReadyToApply();
    CHECK(s.canApply());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kTcpConfig));
    CHECK(s.beginApply());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kApplying));
    CHECK(!s.isEditable());
    CHECK(s.isApplying());
    CHECK(!s.isFinished());

    // Apply 后：输入 setter 全部拒绝且不改状态
    CHECK(!s.setSsid("OtherSSID"));
    CHECK(s.ssid() == kValidSsid);
    CHECK(!s.setPassword("another9"));
    CHECK(!s.setTcpServerIp("10.0.0.9"));
    CHECK(!s.setTcpServerPort(9999));
    CHECK(s.tcpServerIp() == kValidIp);
    CHECK_EQ(s.tcpServerPort(), kValidPort);

    // Apply 后：导航全部拒绝
    CHECK(!s.next());
    CHECK(!s.back());
    CHECK(!s.cancel());
    CHECK(!s.retry());

    // M7-F：beginApply 不再清除密码（发送方消费后调 clearPassword）；
    // 此处显式验证 clearPassword 的安全擦除语义。
    CHECK(s.password() == kValidPassword);
    s.clearPassword();
    CHECK(s.password().empty());
    s.clearPassword();  // 幂等（空密码 no-op）
}

void testApplyFlowToDone() {
    std::printf("[wifi_wizard_state] apply flow to done\n");
    WifiWizardState s = makeReadyToApply();
    CHECK(s.beginApply());
    CHECK(s.markConnecting());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kConnecting));
    CHECK(s.markGotIp());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kGotIp));
    CHECK(s.markTcpConnected());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kTcpConnected));
    CHECK(s.markFullResync());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kFullResync));
    CHECK(s.markDone());
    CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kDone));
    CHECK(s.isFinished());
    CHECK(!s.isEditable());
    CHECK(!s.isApplying());

    // 终态：一切导航/输入拒绝
    CHECK(!s.next());
    CHECK(!s.back());
    CHECK(!s.cancel());
    CHECK(!s.retry());
    CHECK(!s.setSsid("X"));
    CHECK(!s.beginApply());

    // 顺序错误：推进只在正确前置步骤生效（状态不变）
    WifiWizardState t = makeReadyToApply();
    CHECK(!t.markConnecting());  // 还在 kTcpConfig，不可直接推进
    CHECK_EQ(static_cast<int>(t.step()), static_cast<int>(WizardStep::kTcpConfig));
    CHECK(t.beginApply());
    CHECK(!t.markGotIp());  // 当前是 kApplying，跳过 kConnecting 非法
    CHECK_EQ(static_cast<int>(t.step()), static_cast<int>(WizardStep::kApplying));
}

void testErrorRecovery() {
    std::printf("[wifi_wizard_state] error recovery\n");
    // 预 Apply 操作步错误：kScan 失败 → kError → retry 回到 kScan
    {
        WifiWizardState s;
        s.next(); s.next(); s.next();  // kScan
        s.markError(WizardErrorCode::kScanFailed);
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kError));
        CHECK(s.isFinished());
        CHECK(s.isEditable());  // kError 可修复输入
        CHECK_EQ(static_cast<int>(s.error().code),
                 static_cast<int>(WizardErrorCode::kScanFailed));
        CHECK(std::strcmp(s.error().i18nKey, "Wi-Fi scan failed") == 0);
        CHECK(s.retry());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kScan));
        CHECK(!s.error());
        CHECK(s.next());  // 可继续向导
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kSelectSsid));
    }
    // Apply 阶段错误：retry 回到 kTcpConfig 重新应用
    {
        WifiWizardState s = makeReadyToApply();
        CHECK(s.beginApply());
        CHECK(s.markConnecting());
        s.markError(WizardErrorCode::kWifiConnectFailed);
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kError));
        CHECK_EQ(static_cast<int>(s.error().code),
                 static_cast<int>(WizardErrorCode::kWifiConnectFailed));
        CHECK(s.retry());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kTcpConfig));
        CHECK(s.isEditable());
        // M7-F：密码保留到 clearPassword（重试无需重新输入；clearPassword 幂等）
        CHECK(s.password() == kValidPassword);
        s.clearPassword();
        CHECK(s.password().empty());
        CHECK(s.setPassword(kValidPassword));
        CHECK(s.beginApply());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kApplying));
    }
    // 非错误态 retry 无效；kNone markError 防御性忽略
    {
        WifiWizardState s;
        CHECK(!s.retry());
        s.markError(WizardErrorCode::kNone);
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kInit));
    }
    // kError 下 cancel 可用（重置向导）
    {
        WifiWizardState s;
        s.next(); s.next(); s.next();
        s.markError(WizardErrorCode::kScanFailed);
        CHECK(s.cancel());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kInit));
        CHECK(!s.error());
    }
    // 每个错误码（校验 1..7 + 运行期 10..17）都有非空 i18n key
    const WizardErrorCode kCodes[] = {
        WizardErrorCode::kSsidMissing, WizardErrorCode::kSsidTooLong,
        WizardErrorCode::kSsidInvalidChars, WizardErrorCode::kPasswordTooShort,
        WizardErrorCode::kPasswordTooLong, WizardErrorCode::kInvalidTcpServerIp,
        WizardErrorCode::kInvalidTcpServerPort, WizardErrorCode::kUartConnectFailed,
        WizardErrorCode::kCapabilityReadFailed, WizardErrorCode::kScanFailed,
        WizardErrorCode::kApplyFailed, WizardErrorCode::kWifiConnectFailed,
        WizardErrorCode::kNoIpReceived, WizardErrorCode::kTcpConnectFailed,
        WizardErrorCode::kFullResyncFailed,
    };
    for (WizardErrorCode code : kCodes) {
        const char* key = wizardErrorKey(code);
        CHECK(key != nullptr);
        CHECK(key[0] != '\0');
    }
    CHECK(wizardErrorKey(WizardErrorCode::kNone)[0] == '\0');
}

void testSettingsMapNoPassword() {
    std::printf("[wifi_wizard_state] settings map: no password (critical)\n");
    WifiWizardState s = makeReadyToApply();
    CHECK(s.password() == kValidPassword);  // 密码仅在内存
    CHECK(s.ssid() == kValidSsid);
    CHECK(s.tcpServerIp() == kValidIp);
    CHECK_EQ(s.tcpServerPort(), kValidPort);

    // 默认：只导出 TCP IP / port（SSID 为可选 metadata，默认不保存）
    std::map<std::string, std::string> m = s.toSettingsMap();
    CHECK_EQ(static_cast<long long>(m.size()), 2LL);
    CHECK(m.count("tcpServerIp") == 1);
    CHECK(m.count("tcpServerPort") == 1);
    CHECK(m.count("ssid") == 0);
    CHECK(m.at("tcpServerIp") == kValidIp);
    CHECK(m.at("tcpServerPort") == "8765");
    checkNoCredentialKeys(m);

    // includeSsid=true：额外导出 ssid，仍然绝无密码
    std::map<std::string, std::string> m2 = s.toSettingsMap(true);
    CHECK_EQ(static_cast<long long>(m2.size()), 3LL);
    CHECK(m2.count("ssid") == 1);
    CHECK(m2.at("ssid") == kValidSsid);
    checkNoCredentialKeys(m2);

    // 关键断言：任意键名（小写化后）不含 password / psk / passphrase / credential / secret
    const std::map<std::string, std::string>* maps[] = {&m, &m2};
    for (const auto* map : maps) {
        CHECK(map->find("password") == map->end());
        CHECK(map->find("Password") == map->end());
        CHECK(map->find("wifiPassword") == map->end());
        CHECK(map->find("wifi_password") == map->end());
        CHECK(map->find("psk") == map->end());
        CHECK(map->find("credential") == map->end());
    }
}

void testKeysAndDerivedFlags() {
    std::printf("[wifi_wizard_state] keys and derived flags\n");
    // 每个步骤都有非空标题 key
    for (int step = 0; step <= 13; ++step) {
        const char* key = wizardStepTitleKey(static_cast<WizardStep>(step));
        CHECK(key != nullptr);
        CHECK(key[0] != '\0');
    }
    // 已知锚点：步骤标题 key 即英文原文
    CHECK(std::strcmp(wizardStepTitleKey(WizardStep::kInit), "Wi-Fi Wizard") == 0);
    CHECK(std::strcmp(wizardStepTitleKey(WizardStep::kConnectUart), "Connect ESP32") == 0);
    CHECK(std::strcmp(wizardStepTitleKey(WizardStep::kPassword), "Enter Password") == 0);
    CHECK(std::strcmp(wizardStepTitleKey(WizardStep::kDone), "Done") == 0);
    CHECK(std::strcmp(wizardStepTitleKey(WizardStep::kError), "Error") == 0);

    // canApply 门控：缺任一输入则 false
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        s.setPassword(kValidPassword);
        s.next();
        CHECK(!s.canApply());  // 缺 IP
        s.setTcpServerIp(kValidIp);
        CHECK(!s.canApply());  // 缺 port
        s.setTcpServerPort(kValidPort);
        CHECK(s.canApply());
    }
    // beginApply 校验门控：输入非法时拒绝并留在 kTcpConfig
    {
        WifiWizardState s;
        s.next(); s.next(); s.next(); s.next();
        s.setSsid(kValidSsid);
        s.next();
        s.setPassword(kValidPassword);
        s.next();
        s.setTcpServerIp("999.1.1.1");
        s.setTcpServerPort(kValidPort);
        CHECK(!s.beginApply());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kTcpConfig));
    }
}


// M7-G2 — cancelApplying()（异步 Apply 链真取消）与 WIFI_STATUS 细分错误码。
void testCancelApplyingAndNewErrors() {
    // 1) kApplying..kFullResync 各异步步均可取消 -> 回 kInit、密码清零、错误清空。
    for (int stepIdx = static_cast<int>(WizardStep::kApplying);
         stepIdx <= static_cast<int>(WizardStep::kFullResync); ++stepIdx) {
        WifiWizardState s = makeReadyToApply();
        CHECK(s.beginApply());
        const auto target = static_cast<WizardStep>(stepIdx);
        if (stepIdx == static_cast<int>(WizardStep::kConnecting)) {
            CHECK(s.markConnecting());
        } else if (stepIdx == static_cast<int>(WizardStep::kGotIp)) {
            CHECK(s.markConnecting());
            CHECK(s.markGotIp());
        } else if (stepIdx == static_cast<int>(WizardStep::kTcpConnected)) {
            CHECK(s.markConnecting());
            CHECK(s.markGotIp());
            CHECK(s.markTcpConnected());
        } else if (stepIdx == static_cast<int>(WizardStep::kFullResync)) {
            CHECK(s.markConnecting());
            CHECK(s.markGotIp());
            CHECK(s.markTcpConnected());
            CHECK(s.markFullResync());
        }
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(target));
        CHECK(s.isApplying());
        CHECK(s.cancelApplying());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kInit));
        CHECK(s.error().code == WizardErrorCode::kNone);
        CHECK(s.password().empty());
        CHECK(!s.isApplying());
    }

    // 2) 编辑态 / 终态不可 cancelApplying。
    {
        WifiWizardState s = makeReadyToApply();  // kTcpConfig（编辑态）
        CHECK(!s.cancelApplying());
        WifiWizardState d = makeReadyToApply();
        CHECK(d.beginApply());
        CHECK(d.markConnecting());
        CHECK(d.markGotIp());
        CHECK(d.markTcpConnected());
        CHECK(d.markFullResync());
        CHECK(d.markDone());
        CHECK(!d.cancelApplying());
        WifiWizardState e = makeReadyToApply();
        e.markError(WizardErrorCode::kScanFailed);
        CHECK(!e.cancelApplying());  // kError 用 cancel() / retry()
    }

    // 3) 细分错误码：wizardErrorKey 非空、可进错误态、retry 回 kTcpConfig。
    const WizardErrorCode codes[] = {
        WizardErrorCode::kAuthFailed, WizardErrorCode::kApNotFound,
        WizardErrorCode::kDhcpTimeout, WizardErrorCode::kServerUnreachable,
    };
    for (const auto code : codes) {
        WifiWizardState s = makeReadyToApply();
        CHECK(s.beginApply());
        CHECK(s.markConnecting());
        s.markError(code);
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kError));
        CHECK(s.error().code == code);
        CHECK(std::strlen(wizardErrorKey(code)) != 0u);
        CHECK(s.retry());
        CHECK_EQ(static_cast<int>(s.step()), static_cast<int>(WizardStep::kTcpConfig));
        CHECK(s.error().code == WizardErrorCode::kNone);
    }
}
}  // namespace

// 由 shared/protocol/tests/test_main.cpp 登记调用（协议套件二进制）。
void runWifiWizardStateTests() {
    std::printf("[wifi_wizard_state]\n");
    testStepFlow();
    testSsidValidation();
    testPasswordValidation();
    testTcpConfigValidation();
    testApplyLocksEditing();
    testApplyFlowToDone();
    testErrorRecovery();
    testSettingsMapNoPassword();
    testKeysAndDerivedFlags();
    testCancelApplyingAndNewErrors();
    std::printf("[wifi_wizard_state] done\n");
}