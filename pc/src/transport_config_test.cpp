// ESPView M6-D — TransportConfig host tests（纯 C++17，无 Qt / 无 ESP-IDF）。
//
// 规范来源：docs/DESIGN.md W 节（M6-D §十八.3 invalid config / §七 默认值）。
// 覆盖：
//   - 默认值：Transport=TCP、UART COM4/115200、TCP bind 0.0.0.0:8765（§七）；
//   - 合法 UART / TCP 配置；
//   - 非法配置：空 COM 口、baud=0、TCP 空 bind、TCP port=0（本地可判定错误）；
//   - TransportConfig 相等/不等（Apply 幂等判断用）。
// 真实 open 失败（COM 占用 / bind 失败）不属于本模块：由 Worker 状态回调 +
// 既有 reconnect policy 异步呈现。

#include <cstdio>
#include <string>
#include <vector>

#include "test_util.h"
#include "transport_config.h"

namespace {

using espview::pc::TransportConfig;
using espview::pc::TransportKind;
using espview::pc::applyCliOverrides;
using espview::pc::persistedSettingsKeys;
using espview::pc::validateTransportConfig;

void runConfigTests() {
    std::printf("[transport_config]\n");

    // 1. 默认值（§七：Transport TCP / UART COM4 115200 / TCP 0.0.0.0:8765）
    {
        TransportConfig cfg;
        CHECK_EQ(static_cast<int>(cfg.kind), static_cast<int>(TransportKind::kTcp));
        CHECK(cfg.uartPort == "COM4");
        CHECK_EQ(cfg.uartBaud, 115200u);
        CHECK(cfg.tcpBind == "0.0.0.0");
        CHECK_EQ(cfg.tcpPort, 8765u);
        CHECK(validateTransportConfig(cfg).empty());
    }

    // 2. 合法 UART（含 921600 experimental）
    {
        TransportConfig cfg;
        cfg.kind = TransportKind::kUart;
        cfg.uartPort = "COM4";
        cfg.uartBaud = 115200;
        CHECK(validateTransportConfig(cfg).empty());
        cfg.uartBaud = 921600;
        CHECK(validateTransportConfig(cfg).empty());
    }

    // 3. 非法 UART：空 COM 口
    {
        TransportConfig cfg;
        cfg.kind = TransportKind::kUart;
        cfg.uartPort = "";
        cfg.uartBaud = 115200;
        CHECK(!validateTransportConfig(cfg).empty());
    }

    // 4. 非法 UART：baud = 0
    {
        TransportConfig cfg;
        cfg.kind = TransportKind::kUart;
        cfg.uartPort = "COM4";
        cfg.uartBaud = 0;
        CHECK(!validateTransportConfig(cfg).empty());
    }

    // 5. 合法 TCP
    {
        TransportConfig cfg;
        cfg.kind = TransportKind::kTcp;
        cfg.tcpBind = "0.0.0.0";
        cfg.tcpPort = 8765;
        CHECK(validateTransportConfig(cfg).empty());
    }

    // 6. 非法 TCP：空 bind
    {
        TransportConfig cfg;
        cfg.kind = TransportKind::kTcp;
        cfg.tcpBind = "";
        cfg.tcpPort = 8765;
        CHECK(!validateTransportConfig(cfg).empty());
    }

    // 7. 非法 TCP：port = 0
    {
        TransportConfig cfg;
        cfg.kind = TransportKind::kTcp;
        cfg.tcpBind = "0.0.0.0";
        cfg.tcpPort = 0;
        CHECK(!validateTransportConfig(cfg).empty());
    }

    // 8. 相等 / 不等（Apply “already running” 幂等判断）
    {
        TransportConfig a;
        TransportConfig b;
        CHECK(a == b);
        b.kind = TransportKind::kUart;
        CHECK(a != b);
        a.kind = TransportKind::kUart;
        CHECK(a == b);
        b.uartPort = "COM5";
        CHECK(a != b);
        a.uartPort = "COM5";
        CHECK(a == b);
        b.tcpPort = 9000;
        CHECK(a != b);
    }

    // ---- 9. M6-E §18：CLI 覆盖（CLI > QSettings > default）----
    {
        // 模拟 QSettings 已预置 base（默认 TCP/COM4/115200/0.0.0.0:8765）。
        TransportConfig cfg;
        const std::vector<std::string> args = {
            "--transport", "uart", "--port", "COM7", "--baud", "921600",
            "--tcp-port", "9000", "--tcp-bind", "127.0.0.1"};
        std::string err;
        CHECK(applyCliOverrides(cfg, args, &err));
        CHECK(err.empty());
        CHECK_EQ(static_cast<int>(cfg.kind), static_cast<int>(TransportKind::kUart));
        CHECK(cfg.uartPort == "COM7");
        CHECK_EQ(cfg.uartBaud, 921600u);
        CHECK_EQ(cfg.tcpPort, 9000u);
        CHECK(cfg.tcpBind == "127.0.0.1");
    }

    // ---- 10. 未知 --transport 值：false + 非空错误；cfg 不变 ----
    {
        TransportConfig cfg;
        const TransportConfig before = cfg;
        std::vector<std::string> args = {"--transport", "bogus"};
        std::string err;
        CHECK(!applyCliOverrides(cfg, args, &err));
        CHECK(!err.empty());
        CHECK(cfg == before);
    }

    // ---- 11. 非法数值忽略（--baud 0 / --tcp-port 0 / --tcp-port 99999）----
    {
        TransportConfig cfg;
        cfg.uartBaud = 115200;
        cfg.tcpPort = 8765;
        std::vector<std::string> args = {"--baud", "0", "--tcp-port", "0",
                                         "--tcp-port", "99999"};
        std::string err;
        CHECK(applyCliOverrides(cfg, args, &err));
        CHECK(err.empty());
        CHECK_EQ(cfg.uartBaud, 115200u);
        CHECK_EQ(cfg.tcpPort, 8765u);
    }

    // ---- 12. 非 transport 参数忽略：返回 true 且 cfg 不变 ----
    {
        TransportConfig cfg;
        const TransportConfig before = cfg;
        std::vector<std::string> args = {"--dump-png", "out", "--diag-log", "d.log",
                                         "--no-reset", "--autoclose-ms", "5000", "--help"};
        std::string err;
        CHECK(applyCliOverrides(cfg, args, &err));
        CHECK(err.empty());
        CHECK(cfg == before);
    }

    // ---- 13. persistedSettingsKeys：恰好 10 个白名单键；结构上无凭据键 ----
    {
        const std::vector<std::string> keys = persistedSettingsKeys();
        CHECK_EQ(keys.size(), 10u);  // M7-D2：+ui/previewEnabled（Physical Preview 使能）
        const std::vector<std::string> expected = {
            "transport/type", "uart/port", "uart/baud", "tcp/port", "window/size",
            "display/mode", "ui/language", "ui/previewEnabled", "split/drawerVisible",
            "split/drawerWidth"};
        for (const std::string& k : expected) {
            bool found = false;
            for (const std::string& kk : keys) {
                if (kk == k) {
                    found = true;
                }
            }
            CHECK(found);
        }
        // 结构性保证：任何键都不含 wifi/ssid/password/psk/credential（大小写不敏感）。
        for (const std::string& k : keys) {
            std::string lower = k;
            for (char& c : lower) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            CHECK(lower.find("wifi") == std::string::npos);
            CHECK(lower.find("ssid") == std::string::npos);
            CHECK(lower.find("password") == std::string::npos);
            CHECK(lower.find("psk") == std::string::npos);
            CHECK(lower.find("credential") == std::string::npos);
        }
        // （f）TransportConfig 不含任何凭据字段：本结构只有 kind/uartPort/uartBaud/
        //   tcpBind/tcpPort/uartNoReset，无 SSID/密码/PSK —— 由构造保证，无需 sizeof。
    }
    std::printf("[transport_config] done\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView TransportConfig host tests (M6-D) ==\n");
    runConfigTests();
    std::printf("----\nchecks: %d, failures: %d\n", espview::proto::test::gChecks.load(),
                espview::proto::test::gFailures.load());
    return espview::proto::test::gFailures.load() == 0 ? 0 : 1;
}
