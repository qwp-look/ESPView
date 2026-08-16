// ESPView M7-C3 — i18n host tests（独立可执行 main，纯 C++17，零 Qt / 零 COM3 依赖）。
//
// 规范来源：任务书 §二十二 11-14：
//   11. English 语言返回英文文案；
//   12. Chinese 语言返回中文文案（任务书 §十四 词汇，中文自然通顺）；
//   13. 语言切换是纯函数：切换只改变 lookup 结果、不持有任何状态——断言
//       任意调用顺序 / 交错调用结果一致、重复调用幂等、uiKeys() 稳定；
//       模块没有"当前语言"全局状态（语言始终作为参数传入）；
//   14. 全部必需 key 两种语言均有非空翻译（遍历 key 清单断言，并钉死
//       §十四 必需词条的中英对照）。
//
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -I pc/src pc/src/i18n.cpp pc/src/i18n_test.cpp -o build/i18n_test

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "i18n.h"

namespace {

using espview::pc::UiLang;
using espview::pc::trText;
using espview::pc::uiKeys;

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

// §十四 必需词条 + M7-C 模式名：钉死中英对照（若目录与此表不一致，测试失败）。
struct RequiredEntry {
    const char* en;
    const char* zh;
};

const RequiredEntry kRequired[] = {
    // 任务书 §十四 核心词汇
    {"Main Title", "主标题"},
    {"Transport", "传输"},
    {"Display", "显示"},
    {"Connection", "连接"},
    {"Session", "会话"},
    {"Virtual", "虚拟"},
    {"Physical", "物理"},
    {"Mirror", "镜像"},
    {"Split", "分屏"},
    {"Application", "应用"},
    {"Diagnostics", "诊断"},
    {"Connected", "已连接"},
    {"Disconnected", "已断开"},
    {"Reconnecting", "重连中"},
    {"Switching", "切换中"},
    {"Ready", "就绪"},
    {"Degraded", "降级"},
    {"Error", "错误"},
    {"Apply", "应用"},
    {"Cancel", "取消"},
    {"Settings", "设置"},
    {"Language", "语言"},
    {"Wi-Fi", "Wi-Fi"},
    {"Wizard", "向导"},
    {"Tools", "工具"},
    {"Save", "保存"},
    {"Close", "关闭"},
    {"Retry", "重试"},
    // M7-D4 Wi-Fi Wizard 词条（钉死中英对照）
    {"Connect ESP32", "连接 ESP32"},
    {"Read Capabilities", "读取能力"},
    {"Scan Wi-Fi", "扫描 Wi-Fi"},
    {"Select SSID", "选择 SSID"},
    {"Enter Password", "输入密码"},
    {"TCP Server Config", "TCP 服务器配置"},
    {"Applying", "正在应用"},
    {"Connecting to Wi-Fi", "正在连接 Wi-Fi"},
    {"Got IP", "已获取 IP"},
    {"TCP Connected", "TCP 已连接"},
    {"Full Resync", "全帧重同步"},
    {"SSID is required", "请选择 SSID"},
    {"SSID too long (max 32 characters)", "SSID 过长（最多 32 字符）"},
    {"SSID contains invalid characters", "SSID 包含无效字符"},
    {"Password too short (min 8 characters)", "密码过短（至少 8 字符）"},
    {"Password too long (max 63 characters)", "密码过长（最多 63 字符）"},
    {"Invalid TCP server IP", "TCP 服务器 IP 无效"},
    {"UART connection failed", "UART 连接失败"},
    {"Capability read failed", "读取能力失败"},
    {"Wi-Fi scan failed", "Wi-Fi 扫描失败"},
    {"Apply failed", "应用失败"},
    {"Wi-Fi connection failed", "Wi-Fi 连接失败"},
    {"No IP address received", "未收到 IP 地址"},
    {"TCP connection failed", "TCP 连接失败"},
    {"Full resync failed", "全帧重同步失败"},
    {"Next", "下一步"},
    {"Back", "上一步"},
    {"Scan", "扫描"},
    {"Scanning...", "扫描中……"},
    {"Refresh", "刷新"},
    {"Open network", "开放网络"},
    {"Open network (no password)", "开放网络（无密码）"},
    {"Server IP", "服务器 IP"},
    {"No Wi-Fi networks found", "未发现 Wi-Fi 网络"},
    {"Firmware does not support Wi-Fi provisioning", "固件不支持 Wi-Fi 配网"},
    {"Wi-Fi provisioning", "Wi-Fi 配网"},
    {"Wi-Fi provisioning complete", "Wi-Fi 配网完成"},
    {"Wi-Fi provisioning failed", "Wi-Fi 配网失败"},
    {"Password is stored in memory only and cleared after apply", "密码仅保存在内存中，应用后立即清除"},
    {"This wizard configures Wi-Fi over the UART bootstrap link", "本向导通过 UART 引导链路配置 Wi-Fi"},
    {"Connect the ESP32 to this PC over UART first", "请先将 ESP32 通过 UART 连接到本机"},
    {"Waiting for the device capabilities", "正在等待设备能力信息"},
    {"Click Scan to search for nearby Wi-Fi networks", "点击扫描以搜索附近的 Wi-Fi 网络"},
    {"Select the network to configure", "选择要配置的网络"},
    {"Leave empty for an open network", "开放网络可留空"},
    {"ESP32 will connect to this TCP server after GOT_IP", "ESP32 获取 IP 后将连接到此 TCP 服务器"},
    {"Applying configuration to ESP32...", "正在向 ESP32 应用配置……"},
    {"ESP32 is connecting to Wi-Fi...", "ESP32 正在连接 Wi-Fi……"},
    {"IP address acquired", "已获取 IP 地址"},
    {"ESP32 connected to the TCP server", "ESP32 已连接 TCP 服务器"},
    {"Waiting for the first FULL frame", "等待第一帧全帧"},
    // M7-C 显示模式名
    {"Virtual Only", "仅虚拟显示"},
    {"Physical Only", "仅物理显示"},
    // M7-C3 状态面板/抽屉扩展词条（钉死防目录缺词）
    {"RSSI", "RSSI"},
    {"Channel", "信道"},
    {"Heap", "堆"},
    {"Frame", "帧"},
    {"Errors", "错误"},
    {"Success", "成功"},
    {"Failure", "失败"},
    {"Active", "正常"},
    {"Disabled", "已禁用"},
    {"Unavailable", "不可用"},
    {"Router", "路由"},
    {"FULL resync", "全帧重同步"},
    {"ESP32 Physical / Diagnostics", "ESP32 物理 / 诊断"},
    {"Waiting for connection", "等待连接"},
    // M7-C4 i18n 完整化：任务书 §十三 动态状态词（M7-C3 未覆盖）
    {"Waiting for FULL", "等待全帧"},
    {"Physical unavailable", "物理不可用"},
    {"No signal", "无信号"},
    {"No data", "无数据"},
    {"Warnings", "警告"},
    {"IP", "IP"},
    {"Capabilities", "能力"},
    // M7-C4：状态面板/主窗口动态文案与错误原因（拼装格式串也须翻译）
    {"NO", "否"},
    {"PING/PONG", "PING/PONG"},
    {"FULL", "全帧"},
    {"PARTIAL", "部分帧"},
    {"Transport ✓ / Session CONNECTED", "传输 ✓ / 会话已连接"},
    {"DISCONNECTED", "已断开"},
    {"ERROR", "错误"},
    {"CONNECTED (FULL resync done)", "已连接（全帧重同步完成）"},
    {"TRANSPORT SWITCHING ...", "传输切换中 ……"},
    {"Switch failed: %1", "切换失败：%1"},
    {"Transport %1  ·  Session %2  ·  Reconnects %3", "传输 %1 · 会话 %2 · 重连 %3"},
    {"Peer: %1", "对端：%1"},
    {"Client: —", "客户端：—"},
    {"%1 fps · last id=%2 %3", "%1 帧/秒 · 最近 id=%2 %3"},
    {"%1 B (↑ %2 B/s)", "%1 B（↑ %2 B/s）"},
    {"decode %1 · CRC %2 · seqGap %3 · session %4",
     "解码 %1 · CRC %2 · 序列缺口 %3 · 会话 %4"},
    {"PING sent %1 / recv %2 · PONG sent %3 / recv %4 · timeouts %5",
     "PING 发送 %1 / 接收 %2 · PONG 发送 %3 / 接收 %4 · 超时 %5"},
    {"Switches %1 · OK %2 · Fail %3 · Last %4 ms · LastErr %5",
     "切换 %1 · 成功 %2 · 失败 %3 · 最近 %4 ms · 最近错误 %5"},
    {"Waiting for HELLO", "等待 HELLO"},
    {"Stopped", "已停止"},
    {"UART port is empty", "UART 端口为空"},
    {"TCP port must be 1..65535", "TCP 端口必须在 1..65535"},
    {"invalid display mode", "无效显示模式"},
    {"physical display unavailable", "物理显示不可用"},
    {"SET_MODE failed (ACK ERR)", "SET_MODE 失败（ACK 错误）"},
    {"kAborted", "发送端中止"},
    {"kPartialWithoutBase", "部分帧缺少基准帧"},
    {"%1 dBm / ch %2", "%1 dBm / 信道 %2"},
    {"ESP %1 / host %2", "ESP %1 / 主机 %2"},
};

// M7-E Power-Aware Wi-Fi Provisioning：camelCase 标识 key。
// 与既有词条不同，英文文案不是 key 本身，中英文案均单独钉死（见 [15]）。
struct M7eEntry {
    const char* key;
    const char* en;
    const char* zh;
};

const M7eEntry kM7e[] = {
    {"preparingDisplay", "Preparing display", "正在准备显示"},
    {"displayPausedForWifiScan", "Display paused for Wi-Fi scan", "显示已暂停（Wi-Fi 扫描中）"},
    {"scanComplete", "Scan complete", "扫描完成"},
    {"scanFailed", "Scan failed", "扫描失败"},
    {"restoringDisplay", "Restoring display", "正在恢复显示"},
    {"wifiConnected", "Wi-Fi connected", "Wi-Fi 已连接"},
    {"tcpConnecting", "Connecting to TCP server", "正在连接 TCP 服务器"},
    {"tcpConnected", "TCP connected", "TCP 已连接"},
    {"displayTemporarilyPausedDuringWifiScan",
     "Display temporarily paused during Wi-Fi scan",
     "Wi-Fi 扫描期间显示已临时暂停"},
};

// M7-G7 i18n 完整性：G3 Display Mode UI 遗留键 + 全 GUI 审计补键。
// 中英均钉死：en = 英文原文（恒等），zh = 简体中文（见目录）。
struct M7g7Entry {
    const char* en;
    const char* zh;
};

const M7g7Entry kM7g7[] = {
    // ---- G3 Display Mode UI 遗留 10 键 ----
    {"Applied", "已应用"},
    {"Selected", "已选择"},
    {"Just now", "刚刚"},
    {"Updated %1 s ago", "更新于 %1 秒前"},
    {"Stale — last update %1 s ago", "过期 — 最后更新于 %1 秒前"},
    {"Both displays show the same application view.", "两个显示屏显示相同的应用画面。"},
    {"Only the PC display is active; the physical side keeps showing diagnostics.",
     "仅 PC 显示屏激活；物理侧继续显示诊断。"},
    {"Only the physical display is active; the PC side is cleared.",
     "仅物理显示屏激活；PC 侧已清除。"},
    {"PC shows the application; physical shows diagnostics.",
     "PC 显示应用；物理侧显示诊断。"},
    {"(cleared)", "（已清除）"},
    // ---- 全 GUI 审计补键（Main / Wi-Fi Wizard / VirtualScreen）----
    {"Wi-Fi Wizard", "Wi-Fi 向导"},
    {"TCP port", "TCP 端口"},
    {"PNG image (*.png)", "PNG 图像 (*.png)"},
    {"[RSSI %1 dBm · ch %2]", "[RSSI %1 dBm · 信道 %2]"},
    {"No signal — waiting for FULL frame", "无信号 — 等待全帧"},
    // ---- Wi-Fi Wizard 错误词条（enDict 补齐，键集对称）----
    {"UART bootstrap unavailable", "UART 引导链路不可用 - 请检查串口与线缆"},
    {"TCP handoff failed (Wi-Fi connected)", "Wi-Fi 已连接，但 TCP 交接失败"},
    {"Wi-Fi authentication failed", "Wi-Fi 认证失败（请检查密码）"},
    {"Wi-Fi network not found", "未找到该 Wi-Fi 网络"},
    {"DHCP timeout - no IP address", "DHCP 超时，未获取到 IP 地址"},
    {"TCP server unreachable", "TCP 服务器不可达"},
    {"Wi-Fi provisioning requires the UART bootstrap link",
     "Wi-Fi 配网需要使用 UART 引导链路"},
    {"Waiting for ESP32 TCP handoff...", "正在等待 ESP32 TCP 交接……"},
};

// 11. English 返回英文（key 即英文原文；未命中回退英文原文）。
void test11English() {
    std::printf("[11] English returns English\n");
    for (const RequiredEntry& e : kRequired) {
        CHECK(std::strcmp(trText(UiLang::kEnglish, e.en), e.en) == 0);
    }
    // 遍历全部 key：除 M7-E camelCase key 外，English 恒等于 key 本身
    for (const std::string& k : uiKeys()) {
        bool isM7e = false;
        for (const M7eEntry& e : kM7e) {
            if (k == e.key) {
                isM7e = true;
                break;
            }
        }
        if (isM7e) {
            continue;
        }
        CHECK(std::strcmp(trText(UiLang::kEnglish, k.c_str()), k.c_str()) == 0);
    }
    // M7-E camelCase key 的英文文案：见 [15]（单独钉死）
    // 未命中 key → 英文原文（返回 key 本身）
    CHECK(std::strcmp(trText(UiLang::kEnglish, "no.such.key"), "no.such.key") == 0);
    CHECK(std::strcmp(trText(UiLang::kChinese, "no.such.key"), "no.such.key") == 0);
}

// 12. Chinese 返回中文（§十四 词条全部钉死对照）。
void test12Chinese() {
    std::printf("[12] Chinese returns Chinese\n");
    for (const RequiredEntry& e : kRequired) {
        CHECK(std::strcmp(trText(UiLang::kChinese, e.en), e.zh) == 0);
    }
    // 中英混写词条仍须有译文（与本表一致）
    CHECK(std::strcmp(trText(UiLang::kChinese, "Wi-Fi"), "Wi-Fi") == 0);
    // 模式名抽查（自然中文）
    CHECK(std::strcmp(trText(UiLang::kChinese, "Mirror"), "镜像") == 0);
    CHECK(std::strcmp(trText(UiLang::kChinese, "Split"), "分屏") == 0);
}

// 13. 语言切换是纯函数：无全局状态、只改变 lookup 结果。
//     本模块不持有 transport / display / framebuffer；此处断言：
//       a) 交替 / 任意顺序调用结果一致（无顺序依赖）；
//       b) 重复调用幂等（无状态累积）；
//       c) uiKeys() 多次调用稳定（目录不被调用副作用修改）；
//       d) "切换语言"可观测效果仅剩返回值差异（同 key 两种语言结果对比）。
void test13LanguageSwitchIsPure() {
    std::printf("[13] language switch is a pure function (no state)\n");

    // a) 交错调用：先 zh 后 en 与先 en 后 zh 结果一致
    for (const RequiredEntry& e : kRequired) {
        const char* zhFirst = trText(UiLang::kChinese, e.en);
        const char* enAfter = trText(UiLang::kEnglish, e.en);
        const char* enFirst = trText(UiLang::kEnglish, e.en);
        const char* zhAfter = trText(UiLang::kChinese, e.en);
        CHECK(std::strcmp(zhFirst, e.zh) == 0);
        CHECK(std::strcmp(zhAfter, e.zh) == 0);
        CHECK(std::strcmp(enFirst, e.en) == 0);
        CHECK(std::strcmp(enAfter, e.en) == 0);
    }

    // b) 重复调用幂等：连续 64 次同一语言结果不变
    for (const RequiredEntry& e : kRequired) {
        const char* first = trText(UiLang::kChinese, e.en);
        bool stable = true;
        for (int i = 0; i < 64; ++i) {
            if (std::strcmp(trText(UiLang::kChinese, e.en), first) != 0) {
                stable = false;
            }
        }
        CHECK(stable);
    }

    // c) uiKeys() 稳定：重复调用返回相同键集（大小与内容不变）
    const std::vector<std::string>& keys1 = uiKeys();
    const std::vector<std::string>& keys2 = uiKeys();
    CHECK(&keys1 == &keys2);  // 返回同一 static 实例
    CHECK(keys1.size() == keys2.size());
    bool sameContents = true;
    for (size_t i = 0; i < keys1.size(); ++i) {
        if (keys1[i] != keys2[i]) {
            sameContents = false;
        }
    }
    CHECK(sameContents);

    // d) 语言只改变 lookup 结果：同一 key 的 zh 结果稳定且可复现；
    //    语言作为参数传入，模块无"当前语言"可变状态。
    const char* zhA = trText(UiLang::kChinese, "Transport");
    const char* zhB = trText(UiLang::kChinese, "Transport");
    const char* enA = trText(UiLang::kEnglish, "Transport");
    CHECK(std::strcmp(zhA, zhB) == 0);
    CHECK(std::strcmp(zhA, "传输") == 0);
    CHECK(std::strcmp(enA, "Transport") == 0);
    CHECK(std::strcmp(zhA, enA) != 0);  // 同一 key 两种语言结果不同（可翻译词条）
}

// 14. 全部必需 key 两种语言均有非空翻译（遍历 key 清单断言）。
void test14AllKeysBothLanguages() {
    std::printf("[14] all keys have non-empty translations in both languages\n");
    const std::vector<std::string>& keys = uiKeys();
    CHECK(keys.size() >= 30u);  // 任务书：至少 30 个 key
    for (const std::string& k : keys) {
        const char* en = trText(UiLang::kEnglish, k.c_str());
        const char* zh = trText(UiLang::kChinese, k.c_str());
        CHECK(en != nullptr && en[0] != '\0');
        CHECK(zh != nullptr && zh[0] != '\0');
    }
    // 必需词条必须全部在 key 清单里（防目录缺词）
    for (const RequiredEntry& e : kRequired) {
        bool found = false;
        for (const std::string& k : keys) {
            if (k == e.en) {
                found = true;
                break;
            }
        }
        CHECK(found);
    }
}

}  // namespace


// 15. M7-E Power-Aware Wi-Fi Provisioning：9 个 camelCase key 的中英文案
//     单独钉死（英文文案非恒等）；且文案只描述"显示临时暂停"，
//     绝不暗示"电源不足已证实"。
void test15M7eProvisioningCopy() {
    std::printf("[15] M7-E provisioning keys bilingual and power-neutral\n");
    const std::vector<std::string>& keys = uiKeys();
    for (const M7eEntry& e : kM7e) {
        CHECK(std::strcmp(trText(UiLang::kEnglish, e.key), e.en) == 0);
        CHECK(std::strcmp(trText(UiLang::kChinese, e.key), e.zh) == 0);
        bool found = false;
        for (const std::string& k : keys) {
            if (k == e.key) {
                found = true;
                break;
            }
        }
        CHECK(found);  // key 必须在目录清单里（防缺词）
        // 任务约束：不得暗示电源不足，只能描述显示临时暂停
        CHECK(std::strstr(e.en, "power") == nullptr);
        CHECK(std::strstr(e.en, "insufficient") == nullptr);
        CHECK(std::strstr(e.zh, "电源") == nullptr);
        CHECK(std::strstr(e.zh, "电量") == nullptr);
        CHECK(std::strstr(e.zh, "不足") == nullptr);
    }
    // 长说明句必须是完整可读句子
    CHECK(std::strlen(trText(UiLang::kEnglish, "displayTemporarilyPausedDuringWifiScan")) > 20u);
    CHECK(std::strlen(trText(UiLang::kChinese, "displayTemporarilyPausedDuringWifiScan")) > 10u);
}

// 16. M7-G7：新补 key 中英均非空且钉死（en = 英文原文；zh 走目录）；
//     全部键必须在 uiKeys() 清单内（防目录缺词）。
void test16M7g7NewKeys() {
    std::printf("[16] M7-G7 new keys bilingual and in uiKeys()\n");
    const std::vector<std::string>& keys = uiKeys();
    for (const M7g7Entry& e : kM7g7) {
        const char* en = trText(UiLang::kEnglish, e.en);
        const char* zh = trText(UiLang::kChinese, e.en);
        CHECK(en != nullptr && en[0] != '\0');
        CHECK(zh != nullptr && zh[0] != '\0');
        CHECK(std::strcmp(en, e.en) == 0);  // en = 英文原文（恒等）
        CHECK(std::strcmp(zh, e.zh) == 0);  // zh 钉死
        CHECK(std::strcmp(en, zh) != 0);    // 中英不同（可翻译词条）
        bool found = false;
        for (const std::string& k : keys) {
            if (k == e.en) {
                found = true;
                break;
            }
        }
        CHECK(found);  // 键必须在目录清单里（防缺词）
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView pc i18n host tests ==\n");
    test11English();
    test12Chinese();
    test13LanguageSwitchIsPure();
    test14AllKeysBothLanguages();
    test15M7eProvisioningCopy();
    test16M7g7NewKeys();
    std::printf("i18n_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
