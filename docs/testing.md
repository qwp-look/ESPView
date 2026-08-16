# ESPView 测试与验证指南（M7-G9）

> 范围：host-only 测试、Qt 构建验证、ESP32 固件构建验证、真实硬件（UART / Wi-Fi / OLED）测试的入口、真实命令、通过标准与当前 baseline。内容与 `docs/DESIGN.md`（§Q 测试分层、各里程碑实测章节、AK 硬件证据矩阵）保持一致，详细设计与证据见 DESIGN.md。
>
> 环境基线（Windows + PowerShell）：MSYS2 MinGW64（`C:\msys64\mingw64\bin`，可被 `MINGW64_BIN` 覆盖）；ESP-IDF v6.0.2（PowerShell profile `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1`，可被 `ESPIDF_PROFILE` 覆盖）；COM 脚本依赖 pyserial（默认用 ESP-IDF venv python `C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe`，可被 `ESPVIEW_PYTHON` 覆盖）。

## 1. 测试分层总览

| 类别 | 入口 | 硬件 | Wi-Fi | COM | 说明 |
|---|---|---|---|---|---|
| Host-only | `scripts\verify_host.bat` / ctest | 否 | 否 | 否 | 协议/显示/传输/OLED host 单测 + loopback |
| Qt 构建检查 | `scripts\verify_qt.bat` | 否 | 否 | 否 | 只验证 `espview_virtual_display` 能构建；GUI 运行才需要链路 |
| ESP32 固件构建 | `scripts\verify_lvgl.bat` / `scripts\espview_build.bat -esp32` | 否 | 否 | 否（可选 COM sanity） | `idf.py build`；可选 `ESPVIEW_COM3` LVGL sanity |
| Hardware UART | `com3_frame_test` / `pc_com3_*.py` | 是 | 否 | 是 | 115200 8N1 baseline 验收 |
| Hardware Wi-Fi/TCP | GUI TCP 模式 / `wifi_provision_probe` / `espview_e_ab_harness.py` | 是 | 是 | 是（UART 引导） | Wi-Fi RF 上电掉线为当前板卡环境限制（见 §8） |
| OLED 观测 | `scripts\pc_oled_monitor.py` | 是（OLED） | 否 | 是 | I2C 0x3C 诊断；host 侧 OLED 单测无需硬件 |

## 2. Host-only（不需要硬件 / Wi-Fi / COM）

### 2.1 一键入口 `scripts\verify_host.bat`

真实命令（等价脚本内部步骤）：

```bat
set MINGW64_BIN=C:\msys64\mingw64\bin   :: 可省略（默认值）
scripts\verify_host.bat
```

脚本内部步骤（HEAD 实测）：

```bat
[1/4]   cmake -S shared\protocol -B build\verify_host\protocol -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
[1/4]   cmake --build build\verify_host\protocol --target espview_protocol_tests scan_transaction_test -j 8
[2/4]   ctest --test-dir build\verify_host\protocol --output-on-failure
[3/5]   cmake -S pc -B build\verify_host\pc -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=OFF
[3/5]   cmake --build build\verify_host\pc --target com3_frame_test tcp_transport_test transport_config_test -j 8
[3b/5]  build\verify_host\pc\transport_config_test.exe
[4/5]   build\verify_host\pc\com3_frame_test.exe --selftest-queue
[5/5]   build\verify_host\pc\tcp_transport_test.exe
```

通过标准：脚本末尾输出 `verify_host: ALL PASS` 且退出码 0；任一步失败输出 `verify_host: FAILED` 且退出码 1。

### 2.2 ctest（协议 + scan 套件）

```bat
cmake --build build\verify_host\protocol --target espview_protocol_tests scan_transaction_test -j 8
ctest --test-dir build\verify_host\protocol --output-on-failure
```

通过标准（2026-08-16，HEAD c48efcd 实测）：`100% tests passed out of 2`（ctest 2/2）；`espview_protocol_tests` 384,440 checks / 0 failures；`scan_transaction_test` 192 checks / 0 failures。

套件组成（均并入 `espview_protocol_tests` 单进程）：

- `shared/protocol/tests`：CRC32、Packet、Encoder、Streaming Encoder、Decoder、FrameAssembler、Pipeline、FramePipeline、ProtocolEndpoint、Endpoint 并发、Capabilities、PhysicalPreview、Wi-Fi Provisioning 事务。
- `shared/display/tests`：RemoteDisplay（writeRect bounds / RGB565 / streaming / FULL 首帧 / PARTIAL / disconnect-FULL / backpressure / flush 生命周期）、DisplayRouter、DisplayUiState、PhysicalStatus、PhysicalCapabilitySnapshot、SplitState。
- `shared/transport/tests`：TransportManager、TransportSink、TransportPipeline。
- `shared/oled/tests`：OledFb、OledStatus、PhysicalRenderer、OledPreview（见 2.6）。
- `pc/src` 纯模型（无 Qt）：`physical_preview_state_test`、`wifi_wizard_state_test`。
- `shared/wifi/tests`：`scan_transaction_test`（独立可执行，单独注册 ctest）。

### 2.3 ByteQueue 自检

```bat
build\verify_host\pc\com3_frame_test.exe --selftest-queue
```

通过标准：输出 `selfTestByteQueue: PASS` 且退出码 0（不需要串口）。回归目标：ByteQueue 粘滞死循环（out 非空 + 并发 push + 连续 popAll），旧实现稳定失败、当前实现稳定通过。

### 2.4 Protocol 单测（细目）

全部并入 `espview_protocol_tests`，覆盖：MAGIC/VERSION/CRC 校验与重同步、payload 边界（4096B）、CHUNKED 拆包与重拼、seq 回绕、帧级错误处理（CRC 失败 / seq gap / PARTIAL 无基准拒绝 / FULL resync）、连接状态机（HELLO / PING-PONG / 心跳超时 / 断线重连）、控制消息 ACK、并发发送序列化、Streaming 逐包编码与整包编码逐位等价。

### 2.5 Transport loopback（127.0.0.1，无真实 Wi-Fi）

```bat
build\verify_host\pc\tcp_transport_test.exe
build\verify_host\pc\transport_config_test.exe
```

通过标准（HEAD 实测）：

- `tcp_transport_test.exe`：126 checks / 0 failures。覆盖 Transport 语义（connect / disconnect / reconnect / partial / sticky / short write / remote close / timeout / invalid address / 多连接 BUSY）与 Protocol integration（HELLO / PING-PONG / FULL 153600B / CHUNKED FULL / PARTIAL / CRC corruption / seq gap / FULL resync / reconnect resync，像素逐字节校验）。
- `transport_config_test.exe`：97 checks / 0 failures。覆盖默认值（Transport=TCP、UART COM4/115200、TCP `0.0.0.0:8765`）、合法配置、非法配置（空 COM、baud=0、空 bind、port=0）与相等比较。

### 2.6 补充 host 测试

```bat
cmake --build build\verify_host\pc --target i18n_test -j 8
build\verify_host\pc\i18n_test.exe
cmake --build build\verify_host\protocol --target oled_preview_test -j 8
build\verify_host\protocol\oled_build\oled_preview_test.exe
```

- `i18n_test`（纯 C++17，无 Qt）：HEAD 实测 1,981 checks / 0 failures（English + 简体中文键值完整性 + M7-E power-neutral 文案断言，不含 power/insufficient/电源/电量/不足）。注：D6 记录为 1,889，随文案新增增长，以实际输出为准。
- `oled_preview_test`（`shared/oled` 独立目标）：按设计不注册 ctest（避免 Not Run），构建后直接运行即可。

## 3. Qt 构建验证

```bat
scripts\verify_qt.bat
```

脚本内部步骤：

```bat
[1/3] cmake -S pc -B build\verify_qt -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=ON
[2/3] cmake --build build\verify_qt --target espview_virtual_display -j 8
[3/3] 检查 build\verify_qt\espview_virtual_display.exe 存在
```

通过标准：输出 `verify_qt: ALL PASS (espview_virtual_display.exe)` 且退出码 0。要求 MSYS2 MinGW64 + Qt 6（本机 Qt 6.11.1，默认 `CMAKE_PREFIX_PATH=C:/msys64/mingw64`，可用 `-DCMAKE_PREFIX_PATH` 覆盖）。此步骤不要求 COM / ESP32。

GUI 运行提示（真实链路时才涉及）：UART 模式打开 COM4；TCP 模式 PC 侧监听 `0.0.0.0:8765`，Windows 防火墙入站规则仅放行 `build\verify_qt\espview_virtual_display.exe`（Private/Public），GUI 必须从该路径启动（DESIGN §T.10，否则连接被拦）。

## 4. ESP32 固件验证

```bat
scripts\verify_lvgl.bat
set ESPVIEW_COM3=COM4   :: 可选：附加 LVGL COM sanity（需先烧录固件）
scripts\verify_lvgl.bat
```

脚本内部步骤：

```bat
[1/3] host 测试（同 2.2：configure + build + ctest，ctest 2/2）
[2/3] powershell -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'; Set-Location <repo>\esp32; idf.py build"
[3/3] 仅当 %ESPVIEW_COM3% 非空：
      <ESPVIEW_PYTHON> scripts\pc_com3_lvgl_sanity.py --port %ESPVIEW_COM3% --baud 115200
```

通过标准：输出 `verify_lvgl: ALL PASS` 且退出码 0；ctest 2/2；`idf.py build` 成功（默认 LVGL demo app，`CONFIG_ESPVIEW_APP_LVGL=y`）。`ESPVIEW_COM3` 未设置时第 3 步自动跳过。

构建 / 烧录工具链（`scripts/README.md` 另述）：

```bat
scripts\espview_build.bat -esp32            :: 构建默认 profile uart_hw
scripts\espview_build.bat --check           :: 仅 preflight（MSYS2 / ESP-IDF 探测）
scripts\espview_flash.bat -p COM4           :: 烧录 uart_hw
scripts\espview_flash.bat --dry-run         :: 参数 + 存在性校验，不真烧
```

Profile 语义：`esp32\build\<profile>`（默认 `uart_hw`）；每个 profile 使用自己的 `build\<profile>\sdkconfig`（F4 起隔离，首次从本地 `esp32\sdkconfig` 引导拷贝），互不漂移。退出码见 `scripts/README.md`。

## 5. Hardware 测试

### 5.1 UART baseline（COM4/COM3 @ 115200 8N1）

端口说明：板载 CH340 在早期里程碑（M1–M5）枚举为 **COM3**，M6-A 起在开发机上为 **COM4**（DESIGN §T.10/§U.1）；M7-E/F 使用外置 USB-SERIAL CH340 模块同样为 **COM4**（§AJ.7b）。工具均用 `--port` 指定实际枚举端口（默认值随工具而异：`pc_com3_*.py` 默认 COM3，`pc_oled_monitor.py` / `wifi_provision_probe` / `win32_com_probe` 默认 COM4）。

真实命令（需先烧录对应固件；`--no-reset` 表示不触发 DTR/RTS 复位）：

```bat
build\pc\com3_frame_test.exe --port COM4 --baud 115200 --mode full-small|full-large|partial|corruption|seq-gap|reconnect [--no-reset]
```

通过标准（DESIGN §M2/§Q/§M5-A 实测记录）：

- `full-small` / `full-large`：FULL 帧逐字节校验一致；`full-large` 为单 RECT 320×240 = 153600B，CHUNKED 重组正确，输出带宽统计。
- `partial`：FULL 后 PARTIAL 提交；重连后 PARTIAL 无基准拒绝 + FULL 恢复。
- `corruption` / `seq-gap`：RX 注入翻转/丢包 → CRC/SEQ 错误被检测 → 帧作废 → 下一 FULL 恢复。
- `reconnect`：断线（等对端超时）→ 重连 → HELLO → PARTIAL 无基准拒绝 → FULL 恢复。

Python 硬件脚本（需 pyserial）：

```bat
<python> scripts\pc_com3_test.py --port COM4 --baud 115200          :: M1-1 字节流 + PING/PONG
<python> scripts\pc_com3_session_test.py --port COM4 --baud 115200  :: M1-2 会话验收
<python> scripts\pc_com3_lvgl_sanity.py --port COM4 --baud 115200   :: M5-A LVGL sanity
```

通过标准：脚本自身 PASS/FAIL 退出码；`pc_com3_session_test.py` 要求 PING 100/100、0 CRC、0 协议错误、无意外断开，SET_MODE（WINDOW/DEVICE/MIRROR）ACK 全部成功；`pc_com3_lvgl_sanity.py` 要求收到 ESP32 HELLO、首帧 FULL、观察期内 ≥1 FULL + ≥1 PARTIAL、`END.frameId == BEGIN.frameId`、无 CRC/半包累计异常，输出 dirty ratio。

时间预算：320×240 RGB565 整帧 153600B @ 115200 实测 ≈ **13.5 s**（有效 payload ≈ 11.1 KB/s）；PARTIAL 秒级（LVGL 实测首帧 ≈13.2 s、后续 PARTIAL ≈2.3 s，§M5-A）。硬件验收须预留相应时间；FULL 期间由对端 PING 维持会话。

### 5.2 Wi-Fi / TCP

拓扑：ESP32 = Wi-Fi STA + TCP client；PC = TCP server `0.0.0.0:8765`（GUI 监听）。ESP32 侧 Kconfig：`CONFIG_ESPVIEW_TRANSPORT_TCP=y`、`CONFIG_ESPVIEW_WIFI_SSID/PASSWORD`（仅本地未跟踪 sdkconfig）、`CONFIG_ESPVIEW_TCP_SERVER_IP`（PC 的 LAN IP）、`CONFIG_ESPVIEW_TCP_SERVER_PORT=8765`。

GUI TCP 验收：以 TCP 模式启动 `build\verify_qt\espview_virtual_display.exe` → ESP32 连入 → HELLO → 首帧 FULL commit → PARTIAL 流动。通过标准：FULL 像素逐字节校验；TCP FULL 153600B 实测 elapsed ≈ 373–716 ms（默认 power save，5 轮，§U.4）；断线重连 FULL resync（§W.7）。

探测 / 实验工具：

```bat
build\pc\wifi_provision_probe.exe --port COM4 --baud 115200 [--timeout-ms 40000] [--no-reset]
build\pc\win32_com_probe.exe --port COM4 --baud 115200 [--pulse-reset] [--timeout-ms 30000]
py -3.10 scripts\espview_e_ab_harness.py [--build --flash] [--modes A,B,C] [--iterations 3] [--strict]
```

通过标准：

- `wifi_provision_probe`（M7-D6）：退出码 0 = 收到 `WIFI_SCAN_RESULT`（或老固件 ACK ERR 降级）；2 = 用法错误；3 = 打开串口失败。安全：不接收/不打印任何 Wi-Fi 凭据。
- `win32_com_probe`（M7-F F1 判别工具）：到时退出 0；用于 RF 上电掉线判别（uart_hw 无 RF 45s 无掉线 vs TCP/RF 固件 ~747ms 掉线，§AK.2/AK.3）。
- `espview_e_ab_harness.py`（M7-E A/B/C OLED 挂起实验）：退出码 0 = 全部 PASS；1 = 任一 mode FAIL；2 = 用法/环境/构建烧录失败。默认不构建不烧录（假设固件已就绪）；`--strict` 与 `--result-file` 见 `scripts/README.md`。安全：只发 `WIFI_SCAN_REQ`，密码零参与。

### 5.3 OLED（I2C 0x3C）

硬件：128×64 SSD1306/SH1106，I2C 400 kHz，SDA=GPIO21、SCL=GPIO22（内部上拉）；地址自动探测 0x08..0x77、优先 0x3C/0x3D，本板实测 **0x3C**、控制器 AUTO → SSD1306（§Y.1）。

```bat
<python> scripts\pc_oled_monitor.py --port COM4 --baud 115200 [--duration 1800] [--no-reset]
```

通过标准（§Y.7/Z.8 实测）：HELLO 握手成功后每 3s 收到诊断行，`oled a=0x3C`、`err=0`、`ok=1`；15 分钟 298 条诊断行 0 错误；`mem` 行 free heap 无漂移；与帧流并行不污染协议链路（0 CRC / 0 bad_magic）。

Host 侧 OLED 测试（无硬件）：OledFb、命令/分段生成、恢复策略、OledStatus、PhysicalRenderer（RGB565→Mono1）全部并入协议套件；`oled_preview_test` 独立目标（见 2.6）。

## 6. 测试矩阵

| 测试类 | 入口 | 硬件 | Wi-Fi | COM | 通过标准 | 当前 baseline | experimental |
|---|---|---|---|---|---|---|---|
| Host 协议套件 | `verify_host.bat` / ctest | 否 | 否 | 否 | ctest 2/2、0 failures | 384,440 checks（2026-08-16 实测） | — |
| ScanTransaction | ctest | 否 | 否 | 否 | 0 failures | 192 checks | — |
| ByteQueue 自检 | `com3_frame_test.exe --selftest-queue` | 否 | 否 | 否 | `selfTestByteQueue: PASS` | PASS | — |
| TCP loopback | `tcp_transport_test.exe` | 否 | 否 | 否 | 0 failures | 126 checks | — |
| TransportConfig | `transport_config_test.exe` | 否 | 否 | 否 | 0 failures | 97 checks | — |
| i18n | `i18n_test.exe` | 否 | 否 | 否 | 0 failures | 1,981 checks（重建实测） | — |
| OLED Preview（host） | `oled_preview_test.exe` | 否 | 否 | 否 | 0 failures | PASS | 未注册 ctest |
| Qt 构建 | `verify_qt.bat` | 否 | 否 | 否 | ALL PASS + exe 存在 | PASS | — |
| ESP32 构建 | `verify_lvgl.bat` / `espview_build.bat -esp32` | 否* | 否 | 否* | `idf.py build` 成功 | PASS（uart_hw 等全 profile） | — |
| UART 帧管线 | `com3_frame_test --mode ...` | 是 | 否 | 是 | 六模式全过、逐字节校验 | PASS @115200 | 921600 大帧不可靠 |
| UART 会话 | `pc_com3_session_test.py` | 是 | 否 | 是 | PING 100/100、0 CRC、ACK 全成 | PASS | — |
| LVGL COM sanity | `pc_com3_lvgl_sanity.py` | 是 | 否 | 是 | 首帧 FULL + FULL/PARTIAL + 0 CRC | PASS | — |
| OLED 观测 | `pc_oled_monitor.py` | 是（OLED） | 否 | 是 | `a=0x3C err=0 ok=1`、heap 无漂移 | PASS（15 min 0 err） | — |
| TCP 硬件 | GUI TCP + ESP32 client | 是 | 是 | 可选 | FULL 逐字节、FULL resync | PASS（FULL ≈373–716 ms） | `ESPVIEW_WIFI_PS_NONE` 实验性 |
| Wi-Fi 扫描探测 | `wifi_provision_probe.exe` | 是 | 是 | 是 | 退出码 0（收到 SCAN_RESULT） | 本板卡预期 FAIL（电源限制） | experimental |
| A/B/C 扫描期实验 | `espview_e_ab_harness.py` | 是 | 是 | 是 | 三 mode 全 PASS（退出码 0） | 本板卡 boot 期掉线不可达 | 依赖稳定供电 |
| RF 掉线判别 | `win32_com_probe.exe --pulse-reset` | 是 | （RF 固件） | 是 | 到时退出 0、无 err=5 | uart_hw 45s 无掉线 | — |

\* `verify_lvgl.bat` 仅当设置 `ESPVIEW_COM3` 时才需要硬件（可选第 3 步）。

## 7. Known Limitations（与测试相关，如实标注）

- **921600 experimental**：仅短包/控制面可用；大帧突发在当前板级链路（经典 ESP32 + 板载 CH340 + 当前 Windows/驱动）实测不可靠，不作为大帧验收速率。正式 baseline 冻结为 **115200 8N1**（§E/§J/§X.16）。烧录统一使用 `-b 115200`（460800 下 esptool 不可靠，§U.3）；2M 波特率部分 CH340 版本不稳。
- **UART FULL ≈ 13.5 s**：320×240 RGB565 整帧 @115200 实测 ≈13.5 s（有效 payload ≈11.1 KB/s），高频整帧推送不可能；硬件测试需预留时间预算，FULL 期间依赖 PING 维持会话（§Y.7）。
- **AP outage 未完全硬件验证**：AP 断电强制验收 deferred（§X.15）。已覆盖路径：TCP 断开 → 指数退避重连 → PC server 重启 → 重新 accept → HELLO → FULL resync；Wi-Fi STA DISCONNECTED → 自动重连路径未在 router 可控断电下实测。
- **外置 CH340 / RF boot instability**：Wi-Fi RF 上电与 CH340 USB 掉线时间强相关（**高可信假设，非已证实**；物理机制——电源余量 vs EMI vs 驱动——未经电压/电流实测，§AK.4）。影响所有 Wi-Fi/TCP 硬件测试的 boot 阶段。软件降流（PASSIVE scan、PM 80MHz、TX 功率 2dBm）降低挂死概率但无法可靠消除掉线；推荐带供电 USB HUB / 外接 5V / 优质线材。
- **部分硬件测试依赖稳定供电**：`wifi_provision_probe` 在当前板卡预期 FAIL；`espview_e_ab_harness.py` 在当下硬件止步于 boot+握手（未进入扫描期，§AJ.7b）。供电改善后可直接复用（`--modes A,B,C`，无需重新构建 B/C）。
- **配置漂移纪律（F4 已修复）**：per-profile sdkconfig 隔离；做硬件对比实验必须核验 `build\<profile>\config\sdkconfig.h`，防止共享 sdkconfig 被后续构建覆盖（§AJ.7b 修正记录）。
- **结果分级纪律**：Observed / Hypothesis 不得表述为 Confirmed（§AK.4）；本文档只引用已记录的实测与 DESIGN.md 证据。

## 8. 凭据与安全

- 所有测试不引入凭据：Wi-Fi SSID/密码只存在于本地未跟踪 `esp32\sdkconfig`（`.gitignore` 覆盖 `esp32/sdkconfig*`，例外 `esp32/sdkconfig.defaults`）；脚本不读不写其内容。
- `espview_e_ab_harness.py` / `wifi_provision_probe` 只发 `WIFI_SCAN_REQ`，密码零参与；SSID 视为非秘密 metadata。
- 示例配置（占位符）见 `examples/sdkconfig.wifi-tcp.defaults.example`，禁止在其中写入真实凭据。

## 9. 变更纪律

- 本文件是仓库测试入口的权威索引；修改代码 / 脚本后请同步更新 §6 矩阵与 baseline 数字，并保持与 `docs/DESIGN.md` 各实测章节一致。
