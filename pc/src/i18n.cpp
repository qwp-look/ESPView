// ESPView M7-C3 — i18n：UI 字符串目录实现（纯 C++17，零 Qt 依赖）。
//
// 规范来源：任务书 §十四（词汇表）+ §二十二 11-14。
// 目录结构：两个 locale 的 std::map<std::string,std::string>（key → 文案）：
//   - enDict()：English（key 即英文原文，恒等映射）；
//   - zhDict()：中文简体（任务书 §十四 词汇 + 四模式名 + M2/M4 状态面板用词）。
// 未命中 key 回退英文原文（返回 key 本身）。模块无全局可变状态。

#include "i18n.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace espview {
namespace pc {
namespace {

// English 目录：key → 英文文案（key 即英文原文，映射恒等）。
const std::map<std::string, std::string>& enDict() {
    static const std::map<std::string, std::string> kEn = {
        // ---- 任务书 §十四 核心词汇 ----
        {"Main Title", "Main Title"},
        {"Transport", "Transport"},
        {"Display", "Display"},
        {"Connection", "Connection"},
        {"Session", "Session"},
        {"Virtual", "Virtual"},
        {"Physical", "Physical"},
        {"Mirror", "Mirror"},
        {"Split", "Split"},
        {"Application", "Application"},
        {"Diagnostics", "Diagnostics"},
        {"Connected", "Connected"},
        {"Disconnected", "Disconnected"},
        {"Reconnecting", "Reconnecting"},
        {"Switching", "Switching"},
        {"Ready", "Ready"},
        {"Degraded", "Degraded"},
        {"Error", "Error"},
        {"Apply", "Apply"},
        {"Cancel", "Cancel"},
        {"Settings", "Settings"},
        {"Language", "Language"},
        {"Wi-Fi", "Wi-Fi"},
        {"Wizard", "Wizard"},
        {"Save", "Save"},
        {"Close", "Close"},
        {"Retry", "Retry"},
        // ---- M7-C 四显示模式名 ----
        {"Virtual Only", "Virtual Only"},
        {"Physical Only", "Physical Only"},
        // ---- M2/M4/M6 UI 补充用词 ----
        {"ESPView", "ESPView"},
        {"Transport Type", "Transport Type"},
        {"Display Mode", "Display Mode"},
        {"Virtual Display", "Virtual Display"},
        {"Physical Display", "Physical Display"},
        {"Connecting", "Connecting"},
        {"Reconnect", "Reconnect"},
        {"Switch", "Switch"},
        {"Reset", "Reset"},
        {"Start", "Start"},
        {"Stop", "Stop"},
        {"Connect", "Connect"},
        {"Disconnect", "Disconnect"},
        {"State", "State"},
        {"Status", "Status"},
        {"Detail", "Detail"},
        {"Resolution", "Resolution"},
        {"Format", "Format"},
        {"Frames", "Frames"},
        {"FPS", "FPS"},
        {"Last discard", "Last discard"},
        {"Protocol", "Protocol"},
        {"Heartbeat", "Heartbeat"},
        {"Input", "Input"},
        {"Port", "Port"},
        {"Baud", "Baud"},
        {"Peer", "Peer"},
        {"Mode", "Mode"},
        {"UART", "UART"},
        {"TCP", "TCP"},
        {"RX", "RX"},
        {"TX", "TX"},
        {"RTT", "RTT"},
        {"File", "File"},
        {"Quit", "Quit"},
        {"Save PNG...", "Save PNG..."},
        {"Save current display", "Save current display"},
        {"OK", "OK"},
        {"Confirm", "Confirm"},
        {"Yes", "Yes"},
        {"No", "No"},
        {"Warning", "Warning"},
        {"Critical", "Critical"},
        {"Info", "Info"},
        {"Unknown", "Unknown"},
        {"Network", "Network"},
        {"SSID", "SSID"},
        {"Password", "Password"},
        {"Firmware", "Firmware"},
        {"Version", "Version"},
        {"Timeout", "Timeout"},
        {"Statistics", "Statistics"},
        {"Log", "Log"},
        {"Local server", "Local server"},
        // ---- M7-C3 状态面板 / Split Drawer / 主窗口（扩展目录）----
        {"Kind", "Kind"},
        {"Reconnects", "Reconnects"},
        {"RSSI", "RSSI"},
        {"Channel", "Channel"},
        {"Heap", "Heap"},
        {"Frame", "Frame"},
        {"Errors", "Errors"},
        {"Partial", "Partial"},
        {"Success", "Success"},
        {"Failure", "Failure"},
        {"RSSI / Ch", "RSSI / Ch"},
        {"Peer HELLO", "Peer HELLO"},
        {"Peer PING", "Peer PING"},
        {"Committed / discarded", "Committed / discarded"},
        {"Scene", "Scene"},
        {"OLED", "OLED"},
        {"OLED errors", "OLED errors"},
        {"HEAP free / largest / min", "HEAP free / largest / min"},
        {"Router", "Router"},
        {"Protocol decode/CRC/seq", "Protocol decode/CRC/seq"},
        {"Dropped frames", "Dropped frames"},
        {"Queue full", "Queue full"},
        {"FULL resync", "FULL resync"},
        {"OLED addr", "OLED addr"},
        {"Close drawer", "Close drawer"},
        {"ESP32 Physical / Diagnostics", "ESP32 Physical / Diagnostics"},
        {"Availability", "Availability"},
        {"Active", "Active"},
        {"Disabled", "Disabled"},
        {"Unavailable", "Unavailable"},
        {"Handshake", "Handshake"},
        {"Idle", "Idle"},
        {"Pending", "Pending"},
        {"Done", "Done"},
        {"Fault", "Fault"},
        {"N/A", "N/A"},
        {"Client", "Client"},
        {"Waiting for connection", "Waiting for connection"},
        {"Session not connected; the selected mode will be sent once connected.",
         "Session not connected; the selected mode will be sent once connected."},
        {"Send the selected display mode to the device (SET_MODE).",
         "Send the selected display mode to the device (SET_MODE)."},
        {"Packets/Messages", "Packets/Messages"},
        {"Port / Baud", "Port / Baud"},
        {"Frames (commit/discard)", "Frames (commit/discard)"},
        {"FPS / last", "FPS / last"},
        {"Errors (decode/CRC/seq/session)", "Errors (decode/CRC/seq/session)"},
        {"Input (sent/dropped/unsup/ignored)", "Input (sent/dropped/unsup/ignored)"},
    };
    return kEn;
}

// 简体中文目录：key → 中文文案（与 enDict 键集一一对应）。
const std::map<std::string, std::string>& zhDict() {
    static const std::map<std::string, std::string> kZh = {
        // ---- 任务书 §十四 核心词汇 ----
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
        {"Save", "保存"},
        {"Close", "关闭"},
        {"Retry", "重试"},
        // ---- M7-C 四显示模式名 ----
        {"Virtual Only", "仅虚拟显示"},
        {"Physical Only", "仅物理显示"},
        // ---- M2/M4/M6 UI 补充用词 ----
        {"ESPView", "ESPView"},
        {"Transport Type", "传输方式"},
        {"Display Mode", "显示模式"},
        {"Virtual Display", "虚拟显示"},
        {"Physical Display", "物理显示"},
        {"Connecting", "连接中"},
        {"Reconnect", "重新连接"},
        {"Switch", "切换"},
        {"Reset", "复位"},
        {"Start", "启动"},
        {"Stop", "停止"},
        {"Connect", "连接"},
        {"Disconnect", "断开"},
        {"State", "状态"},
        {"Status", "状态"},
        {"Detail", "详情"},
        {"Resolution", "分辨率"},
        {"Format", "格式"},
        {"Frames", "帧数"},
        {"FPS", "帧率"},
        {"Last discard", "最近丢弃"},
        {"Protocol", "协议"},
        {"Heartbeat", "心跳"},
        {"Input", "输入"},
        {"Port", "端口"},
        {"Baud", "波特率"},
        {"Peer", "对端"},
        {"Mode", "模式"},
        {"UART", "UART"},
        {"TCP", "TCP"},
        {"RX", "RX"},
        {"TX", "TX"},
        {"RTT", "RTT"},
        {"File", "文件"},
        {"Quit", "退出"},
        {"Save PNG...", "保存 PNG..."},
        {"Save current display", "保存当前显示"},
        {"OK", "确定"},
        {"Confirm", "确认"},
        {"Yes", "是"},
        {"No", "否"},
        {"Warning", "警告"},
        {"Critical", "严重"},
        {"Info", "信息"},
        {"Unknown", "未知"},
        {"Network", "网络"},
        {"SSID", "SSID"},
        {"Password", "密码"},
        {"Firmware", "固件"},
        {"Version", "版本"},
        {"Timeout", "超时"},
        {"Statistics", "统计"},
        {"Log", "日志"},
        {"Local server", "本地服务器"},
        // ---- M7-C3 状态面板 / Split Drawer / 主窗口（扩展目录）----
        {"Kind", "类型"},
        {"Reconnects", "重连次数"},
        {"RSSI", "RSSI"},
        {"Channel", "信道"},
        {"Heap", "堆"},
        {"Frame", "帧"},
        {"Errors", "错误"},
        {"Partial", "部分"},
        {"Success", "成功"},
        {"Failure", "失败"},
        {"RSSI / Ch", "RSSI / 信道"},
        {"Peer HELLO", "对端 HELLO"},
        {"Peer PING", "对端 PING"},
        {"Committed / discarded", "已提交 / 丢弃"},
        {"Scene", "场景"},
        {"OLED", "OLED"},
        {"OLED errors", "OLED 错误"},
        {"HEAP free / largest / min", "堆 空闲 / 最大 / 最小"},
        {"Router", "路由"},
        {"Protocol decode/CRC/seq", "协议 解码 / CRC / 序列"},
        {"Dropped frames", "丢弃帧"},
        {"Queue full", "队列已满"},
        {"FULL resync", "全帧重同步"},
        {"OLED addr", "OLED 地址"},
        {"Close drawer", "关闭抽屉"},
        {"ESP32 Physical / Diagnostics", "ESP32 物理 / 诊断"},
        {"Availability", "可用性"},
        {"Active", "正常"},
        {"Disabled", "已禁用"},
        {"Unavailable", "不可用"},
        {"Handshake", "握手"},
        {"Idle", "空闲"},
        {"Pending", "等待中"},
        {"Done", "完成"},
        {"Fault", "故障"},
        {"N/A", "无"},
        {"Client", "客户端"},
        {"Waiting for connection", "等待连接"},
        {"Session not connected; the selected mode will be sent once connected.",
         "会话未连接；连接后将发送所选模式。"},
        {"Send the selected display mode to the device (SET_MODE).",
         "向设备发送所选显示模式（SET_MODE）。"},
        {"Packets/Messages", "包 / 消息"},
        {"Port / Baud", "端口 / 波特率"},
        {"Frames (commit/discard)", "帧数（提交/丢弃）"},
        {"FPS / last", "帧率 / 最近"},
        {"Errors (decode/CRC/seq/session)", "错误（解码/CRC/序列/会话）"},
        {"Input (sent/dropped/unsup/ignored)", "输入（发送/丢弃/不支持/忽略自动）"},
    };
    return kZh;
}

}  // namespace

const char* trText(UiLang lang, const char* key) {
    if (key == nullptr) {
        return "";
    }
    const std::map<std::string, std::string>& dict =
        (lang == UiLang::kChinese) ? zhDict() : enDict();
    const auto it = dict.find(key);
    if (it != dict.end()) {
        return it->second.c_str();  // static 目录，指针在程序生命周期内稳定
    }
    return key;  // 未命中 → 英文原文
}

const std::vector<std::string>& uiKeys() {
    static const std::vector<std::string> kKeys = [] {
        std::vector<std::string> keys;
        keys.reserve(enDict().size());
        for (const auto& kv : enDict()) {
            keys.push_back(kv.first);
        }
        return keys;
    }();
    return kKeys;
}

}  // namespace pc
}  // namespace espview
