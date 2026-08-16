# Wi-Fi / TCP 模式

> Wi-Fi/TCP 是第二传输：ESP32 = STA 客户端，PC = TCP Server。协议字节流与 UART
> 完全一致（Packet/Message/Frame + CRC32）。本文描述**当前真实状态**：编译期链路 +
> experimental 的 provisioning，AP outage 未完全验证。

## 1. 拓扑与端口

```
ESP32 (Wi-Fi STA)  ──TCP client──►  PC (TCP Server 0.0.0.0:8765)
```

- 默认端口 **8765**（Kconfig `ESPVIEW_TCP_SERVER_PORT`，范围 1024..65535）。
- PC 侧启动：`build\verify_qt\espview_virtual_display.exe --transport tcp --tcp-bind 0.0.0.0 --tcp-port 8765`
- 凭据/服务器地址只在 ESP32 固件侧（本地未跟踪 `esp32\sdkconfig`）配置，见
  [docs/security.md](security.md)。

## 2. 编译期链路（当前状态，已实现）

- 固件侧在 `idf.py menuconfig` 中选 `ESPView → Protocol transport → Wi-Fi STA + TCP`
  （Kconfig `ESPVIEW_TRANSPORT_TCP`；默认 UART）。
- 相关 Kconfig：`ESPVIEW_WIFI_SSID` / Wi-Fi 密码键 /
  `ESPVIEW_TCP_SERVER_IP` / `ESPVIEW_TCP_SERVER_PORT` /
  `ESPVIEW_TCP_CONNECT_TIMEOUT_MS`（10000）/ `ESPVIEW_TCP_RECONNECT_DELAY_MS`（3000）。
- SSID/密码默认空字符串，只通过本机未跟踪 sdkconfig 或 menuconfig 注入（**占位符**）。
- PC 侧 `HostTcpTransport` 同时支持 server（监听/accept）与 client（连 ESP32）两种
  角色；有 host loopback 测试（`pc\src\tcp_transport_test.cpp`，127.0.0.1，无需硬件）。
- 断线重连 → 会话重置 → HELLO → **FULL resync**（与 UART 同语义）；TCP 大帧
  FULL 实测 235–254ms（默认 power save，2026-08-14，见 DESIGN.md V.8）。

## 3. Wi-Fi provisioning（experimental / 受限）

配网协议族（M7-D3 冻结，wire additive）：

| TYPE | 消息 | 方向 | 语义 |
| --- | --- | --- | --- |
| 0x06 | `WIFI_SCAN_REQ` | PC→ESP | 扫描请求（必须 ACK_REQ） |
| 0x07 | `WIFI_SCAN_RESULT` | ESP→PC | 扫描结果（fire-and-forget，≤64 条，RSSI 降序） |
| 0x08 | `WIFI_CONFIG` | PC→ESP | 下发 SSID/密码/TCP server（必须 ACK_REQ） |
| 0x09 | `WIFI_STATUS` | ESP→PC | 状态/相位（fire-and-forget，**绝无密码字段**） |

- **链路形态**：UART bootstrap → 扫描 → 配置 → Wi-Fi 连接 → TCP connect → HELLO → FULL
  （DESIGN.md AK.5 冻结语义）。
- **现状受限**：UART 控制路径（ACK）端到端验收通过；但本板卡在 RF 上电时存在
  CH340 掉线现象（固定措辞见下），扫描结果/配网全流程的**物理空口验收依赖供电充足
  环境**；TCP handoff 的完整运行时切换仍为遗留项。
- **AP outage 未完全验证**：AP 断电（router 掉电）不在本轮验收内（DESIGN.md X.15）；
  已覆盖的是 TCP 断开 → 指数退避重连 → PC server 重启 → re-accept → HELLO → FULL resync。

固定措辞（CH340/RF）：**RF 上电与 USB-UART/板级环境不稳定高度相关；物理机制属 hypothesis/high confidence；电源余量/EMI/USB 瞬态待硬件验证。**

## 4. Wi-Fi Wizard（PC 端，M7-D4）

入口：Qt GUI `Tools → Wi-Fi Wizard`。状态机（`pc\src\wifi_wizard_state.cpp`，
纯 C++17）共 14 步：

| 步骤 | 名称 | 说明 |
| --- | --- | --- |
| 0 | Init | 向导说明 / UART bootstrap 准备 |
| 1 | Connect UART | 连接 ESP32（statusChanged=Connected 自动前进） |
| 2 | Read Capabilities | 读取能力（capabilitiesReceived 自动前进） |
| 3 | Scan | `WIFI_SCAN_REQ` → 结果列表 |
| 4 | Select SSID | 选择目标 SSID |
| 5 | Password | 密码输入（空 = 开放网络） |
| 6 | TCP Config | TCP server IP + port |
| 7 | Applying | Apply（`WIFI_CONFIG` ACK ok → 8） |
| 8 | Connecting | ESP32 连接中（phase=kGotIp → 9） |
| 9 | Got IP | 已获 IP |
| 10 | TCP Connected | phase=kTcpConnected |
| 11 | FULL Resync | TCP 连后首帧 FULL commit → Done |
| 12 | Done | 成功（Close） |
| 13 | Error | 失败（Retry / Cancel） |

要点：

- 本地校验：SSID 1..32 可见字节；密码空或 8..63 字节；TCP server 严格 IPv4 + port 1..65535。
- 密码只在内存（QLineEdit + WizardState），`beginApply()` 成功后/关闭/取消即清零；
  `toSettingsMap()` 只导出 tcpServerIp/tcpServerPort（SSID 默认不导出）。
- 老固件对 `WIFI_SCAN_REQ` 回 ACK ERR → 提示「固件不支持 Wi-Fi provisioning」，
  不发真实凭据（探针降级）。
- UART 链路不可用时显示「UART bootstrap unavailable」，**不得伪装成密码错误**。

## 5. 验证命令

```bat
:: host loopback（无需硬件）：TCP transport 测试
scripts\verify_host.bat

:: UART provisioning 探测（真实硬件；退出码 0=PASS 收到 SCAN_RESULT）
build\verify_host\pc\wifi_provision_probe.exe --port COM4 --baud 115200

:: A/B/C 硬件实验（OLED active/suspended/disabled；默认不构建不烧录）
py -3.10 scripts\espview_e_ab_harness.py
py -3.10 scripts\espview_e_ab_harness.py --modes A,B --iterations 3 --strict
py -3.10 scripts\espview_e_ab_harness.py --build --flash
```

## 6. 安全提示

- 配网是**明文**（UART 下发，CRC32 仅完整性、非加密），M7-D 只做局域网开发工具用途，
  **不宣称安全 provisioning、不实现 TLS**；配网完成后建议断开物理通道。
- PC TCP Server 默认 bind `0.0.0.0` 仅适合受信局域网；默认建议 bind `127.0.0.1`
  （Peer token 机制留待后续）。
- 凭据策略与 QSettings 白名单见 [docs/security.md](security.md)。
