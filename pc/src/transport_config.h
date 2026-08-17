// ESPView M6-D — Transport runtime configuration（纯 C++17，零平台依赖，无 Qt）。
//
// 规范来源：docs/DESIGN.md W 节（M6-D）：
//   - 正式 Transport UI 只改变 PC 侧 ConnectionManager/SerialWorker 的 Transport
//     选择（UART COM 口 / TCP Server bind）；ESP32 侧 Transport 由编译期默认 +
//     CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH（test-only F12 钩子，生产固件禁用）。
//   - 默认值：Transport=TCP、UART COM4/115200、TCP bind 0.0.0.0:8765；
//     CLI（--transport/--port/--baud/--tcp-bind/--tcp-port）与 QSettings 可覆盖。
//   - switchTransport 语义：旧会话 stop（join）→ 会话重置（seq/ACK/decoder/
//     FrameAssembler/PARTIAL base/Input 队列）→ 新 Transport → HELLO → FULL resync。
//   - 本文件只做“本地配置校验”，不接触协议 wire format / 串口 / socket。
//
// 与 serial_worker.h 的 TransportKind 合并定义（本文件不依赖 Qt，host 测试可用）。

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "transport.h"  // M8-A3：TransportKind alias 到 shared/transport::TransportType

namespace espview {
namespace pc {

// M6-A/M6-D：运行时选择的 Transport 类型。
// M8-A3：唯一 canonical 类型（alias 到 shared/transport::TransportType，
//   新增 backend 只扩展 shared/transport 的枚举，不再维护第二套）。
using TransportKind = espview::transport::TransportType;

// GUI Apply / ConnectionManager.switchTransport 的统一配置快照。
struct TransportConfig {
    TransportKind kind = TransportKind::kTcp;  // M6-D 默认 TCP（与固件默认一致）
    std::string uartPort = "COM4";             // UART：COM 口（如 COM4）
    uint32_t uartBaud = 115200;                // UART：波特率（正式 baseline）
    std::string tcpBind = "0.0.0.0";           // TCP：Server 监听地址（PC = Server）
    uint16_t tcpPort = 8765;                   // TCP：Server 监听端口
    bool uartNoReset = false;                  // UART：跳过 DTR/RTS 复位脉冲
    //（runtime switch 用：M6-C --no-reset 语义；全新启动应保持复位以触发
    //  确定性 boot HELLO，除非对端会话已由 F12 test hook 建立。）
};

inline bool operator==(const TransportConfig& a, const TransportConfig& b) {
    return a.kind == b.kind && a.uartPort == b.uartPort && a.uartBaud == b.uartBaud &&
           a.tcpBind == b.tcpBind && a.tcpPort == b.tcpPort;
}
inline bool operator!=(const TransportConfig& a, const TransportConfig& b) {
    return !(a == b);
}

// 本地配置校验：返回空串 = 合法；否则返回人类可读原因（“Switch failed: <reason>”）。
// 只校验“本地可判定的配置错误”（空 COM 口 / baud=0 / 端口越界 / 空 bind）。
// 真实 open 失败（COM 占用 / TCP bind 失败）属于异步运行期错误，由 Worker 状态
// 回调 + 既有 reconnect policy 处理，不在此校验。
inline std::string validateTransportConfig(const TransportConfig& cfg) {
    if (cfg.kind == TransportKind::kUart) {
        if (cfg.uartPort.empty()) {
            return "UART port is empty";
        }
        if (cfg.uartBaud == 0) {
            return "UART baud must be > 0";
        }
    } else {
        if (cfg.tcpBind.empty()) {
            return "TCP bind address is empty";
        }
        if (cfg.tcpPort == 0) {
            return "TCP port must be 1..65535";
        }
    }
    return std::string();
}

// ---- CLI 覆盖（M6-E §18/§24.11-12）----
// CLI 覆盖 QSettings 后的配置（优先级 CLI > QSettings > default）。
// 只处理 transport 相关参数（--transport/--port/--baud/--tcp-port/--tcp-bind）；
// 未知参数忽略并返回 true。未知 --transport 值返回 false 并写入 error。
// 语义与 main.cpp 原解析块一致（区分大小写的选项名；数值参数仅当 >0 生效）。
inline bool applyCliOverrides(TransportConfig& cfg, const std::vector<std::string>& args,
                              std::string* error = nullptr) {
    const auto parseUInt = [](const std::string& s, unsigned long& out) -> bool {
        if (s.empty()) {
            return false;
        }
        for (char c : s) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        try {
            out = std::stoul(s);
        } catch (...) {
            return false;
        }
        return true;
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--transport" && i + 1 < args.size()) {
            const std::string raw = args[++i];
            std::string t = raw;
            for (char& c : t) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            if (t == "tcp") {
                cfg.kind = TransportKind::kTcp;
            } else if (t == "uart") {
                cfg.kind = TransportKind::kUart;
            } else {
                if (error != nullptr) {
                    *error = "Unknown transport '" + raw + "' (uart|tcp)";
                }
                return false;
            }
        } else if (a == "--port" && i + 1 < args.size()) {
            cfg.uartPort = args[++i];
        } else if (a == "--baud" && i + 1 < args.size()) {
            unsigned long v = 0;
            if (parseUInt(args[++i], v) && v > 0) {
                cfg.uartBaud = static_cast<uint32_t>(v);
            }
        } else if (a == "--tcp-port" && i + 1 < args.size()) {
            unsigned long v = 0;
            if (parseUInt(args[++i], v) && v > 0 && v <= 65535) {
                cfg.tcpPort = static_cast<uint16_t>(v);
            }
        } else if (a == "--tcp-bind" && i + 1 < args.size()) {
            cfg.tcpBind = args[++i];
        }
        // 其余参数（--dump-png/--diag-log/--no-reset/--autoclose-ms/--help）忽略。
    }
    return true;
}

// ---- QSettings 白名单（M6-E §24.11-12）----
// 唯一允许持久化的键。结构性保证：绝不保存 Wi-Fi SSID/密码/凭据
// （TransportConfig 本身也不含任何凭据字段；凭据只存在于 ESP32 本地
// 未跟踪 sdkconfig）。
// M7-C3：新增 display/mode（Display Mode UI）、ui/language（i18n）、
// split/drawerVisible + split/drawerWidth（Split Drawer 布局）——均为
// 非凭据 UI 状态键，与任务书 §十五 一致。
// M7-D2：新增 ui/previewEnabled（Physical Preview 使能开关；PhysicalPreviewState
// toSettingsMap 唯一键，AE.5）。
inline std::vector<std::string> persistedSettingsKeys() {
    return {"transport/type",  "uart/port",       "uart/baud",
            "tcp/port",        "window/size",     "display/mode",
            "ui/language",     "ui/previewEnabled",
            "split/drawerVisible",
            "split/drawerWidth"};
}
}  // namespace pc
}  // namespace espview
