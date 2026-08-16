# 常见故障排查（Troubleshooting）

> 每条给出真实排查步骤与命令。涉及 CH340/RF 的措辞遵循固定表述：
> **RF 上电与 USB-UART/板级环境不稳定高度相关；物理机制属 hypothesis/high confidence；电源余量/EMI/USB 瞬态待硬件验证。**

## 1. CH340 掉线 / `ReadFile err=5` / `PermissionError 5`

现象：Wi-Fi RF 上电（boot 期或扫描请求后）期间串口链路断开；少数复现 ESP32
无复位横幅挂死（需手动复位）。

排查步骤：

1. 确认是否是 RF 相关：烧/刷回 UART-only 固件（`scripts\espview_build.bat -esp32 -b uart_hw`
   后 `scripts\espview_flash.bat -p COM4`），UART 模式下该链路稳定无掉线（M6-C 基线）。
2. 取证：`build\verify_host\pc\win32_com_probe.exe --port COM4 --pulse-reset`
   （结构化 `[evt] t=+<ms>` 日志；判别实验：无 RF 固件 45s 无掉线、RF 固件
   boot ~747ms 掉线，见 DESIGN.md AK.3）。
3. 恢复掉线句柄：关闭占用程序 → 拔插 USB → 设备管理器确认端口 → close+reopen。
4. 硬件路径（推荐顺序）：带供电 USB HUB / 外接 5V 供电 / 换回板载 CH340 / 优质线材。
5. 不要下「供电不足已证实」结论：当前只有 Observed 现象与高可信假设，物理机制
   （电源余量 vs EMI vs CH340 驱动）未经电压/电流实测。

## 2. Wi-Fi 扫描失败 / 无 SCAN_RESULT

现象：`WIFI_SCAN_REQ` 后 ACK 正常（~0.1s），但收不到 `WIFI_SCAN_RESULT`；或
PC 侧 `ReadFile err=5`。

排查步骤：

1. 先确认 UART 控制路径：`build\verify_host\pc\wifi_provision_probe.exe --port COM4 --baud 115200`
   （probe 工具由 `pc\CMakeLists.txt` 注册，`verify_host.bat` 不默认构建；
   补建：`cmake --build build\verify_host\pc --target wifi_provision_probe -j 8`）
   （退出码 0 = 收到 SCAN_RESULT；1 = 超时/无结果，本板卡电源受限环境下 1 属预期）。
2. 检查探针是否先 `SET_MODE(1)` 停掉虚拟帧流（显示流占用 sendMutex 时 tryTransmit
   会丢控制消息；D6 探针先停帧流再扫描，ACK 稳定）。
3. 固件侧已知修正：`startScan` 首次懒初始化后 `esp_wifi_scan_start` 返回
   `ESP_ERR_WIFI_NOT_STARTED` → 先 `esp_wifi_start()` 再重试一次（AI.2 已修复，
   确保固件含 M7-D6 提交 `a2f765d` 之后代码）。
4. 扫描配置：PASSIVE + `scan_time.passive=120`、PM 80MHz、TX 功率 2dBm（AI.3 降流组合，
   降低挂死概率，不保证消除掉线）。
5. RF 掉线场景按 §1 硬件路径处理；供电改善后复用
   `py -3.10 scripts\espview_e_ab_harness.py --modes A,B,C` 完成扫描期对比。
6. 老固件回 ACK ERR（kInvalidParam）→ 向导提示「固件不支持 Wi-Fi provisioning」，
   属探针降级正常行为。

## 3. OLED 镜像 / 无显示

现象：OLED 不亮、花屏、或文字水平镜像。

排查步骤：

1. 看遥测：`py -3.10 scripts\pc_oled_monitor.py`（或 GUI `--diag-log`），
   找 `oled a=<addr> c=<ctrl> err=<n> ok=<0|1>`。
   - 无 `oled` 行 → `CONFIG_ESPVIEW_OLED_ENABLE=n` 或旧固件；用
     `scripts\espview_build.bat -esp32 -b uart_hw`（OLED=y 的现成 profile）重建。
   - `a=` 不是 0x3C → AUTO 探测到别的地址或控制器类型，核对接线与 Kconfig
     `ESPVIEW_OLED_ADDR_AUTO`。
   - `err=` 持续增长 → 走有界恢复（bus_reset → 重建，MAX_REINIT=3 + 退避）；
     检查 I2C 上拉（板级无外部上拉时驱动开内部上拉）、SDA/SCL 是否接反。
2. 镜像：水平镜像的根因是字体/列序约定——固件按页反转列序上传 + 字体 bit0=最左
   约定（2026-08-15 修正，见 DESIGN.md Y.1）。确保固件为 M7-A 修正后版本；自购新屏
   仍镜像时，检查 Kconfig 控制器选择（AUTO 默认 SSD1306；SH1106 变体需强制指定）。
3. 无显示但 `ok=1`：检查对比度/初始化是否被应用帧覆盖场景（Split=Diagnostics 页、
   VirtualOnly=诊断页持续刷新；PhysicalOnly/Mirror=应用帧），见
   [docs/display-modes.md](display-modes.md)。

## 4. No Preview / Drawer 无物理预览

现象：Split Drawer 顶部没有 OLED 预览位图。

排查步骤：

1. 确认 preview 开关：QSettings 键 `ui/previewEnabled`（GUI 设置里启用）。
2. 确认物理可用：遥测 `oled ... ok=1`（PC 侧唯一真实观测源；无遥测 → Unknown）。
3. 频率：ESP32 默认 2Hz 上行（UART 115200 下 1Hz），等 ≤1s 观察；
   预览为 fire-and-forget，丢帧自重同步。
4. 扫描窗口内 preview 整函数跳过（挂起期间字节流量 = 0），扫描结束恢复；
   若长时间无预览，检查 `CONFIG_ESPVIEW_SCAN_SUSPEND_OLED` 恢复路径
   （ScanTransaction 终态必须恰好 resume 一次）。
5. 断线重连后 PC 清空预览位图，等重连后首帧。

## 5. TCP 连不上（PC Server 收不到连接）

现象：GUI 显示 TCP server 已监听，但 ESP32 不连 / 反复断。

排查步骤：

1. PC 侧确认监听：`espview_virtual_display.exe --transport tcp --tcp-bind 0.0.0.0 --tcp-port 8765`
   （bind 0.0.0.0 时防火墙可能拦截入站；受信局域网内可临时放行或先 bind 127.0.0.1 验证）。
2. 固件侧确认凭据/地址（本地未跟踪 `esp32\sdkconfig`，占位符示例）：
   `ESPVIEW_WIFI_SSID="<your-wifi-ssid>"`、Wi-Fi 密码 Kconfig 键（占位符 `<your-wifi-password>`）、
   `ESPVIEW_TCP_SERVER_IP="<pc-lan-ip>"`、`ESPVIEW_TCP_SERVER_PORT=8765`；
   必须用 TCP transport 构建（`ESPVIEW_TRANSPORT_TCP=y`）。
3. 看 ESP32 诊断行（ERROR 文本通道）`trx` 的 RSSI/channel/reconnect/tx/rx：
   - 无 RSSI/CH → STA 未连接（SSID/密码错、AP 不可见）；
   - 有 IP 但反复 reconnect → TCP server 不可达/防火墙；
   - 连接成功 → 等待 HELLO + 首帧 FULL。
4. 验证链路（无需硬件）：`scripts\verify_host.bat`（含 `tcp_transport_test`，
   127.0.0.1 loopback）。
5. AP 断电（AP outage）未纳入验收（DESIGN.md X.15）：已覆盖的是 TCP 断开 →
   指数退避重连 → PC server 重启 → re-accept → HELLO → FULL resync。

## 6. 无画面（Virtual 黑屏）

现象：GUI 打开，状态面板已连接，但 VirtualScreen 全黑。

排查步骤：

1. PC 只保存「最后一份完整提交帧」：无 committed FULL 前 PARTIAL 不提交，等 FULL
   resync；UART 下首帧 FULL ≈13.5s，先等 20s。
2. 检查握手：HELLO 后应收到 CAPABILITIES；无则按 §1 检查串口/复位。
3. 显示模式影响：PhysicalOnly(1) 下虚拟侧禁用（画面保持清屏占位，属正常语义）；
   VirtualOnly(0)/Mirror(2)/Split(3) 虚拟侧应有画面，见
   [docs/display-modes.md](display-modes.md)。
4. 取证：`--dump-png <dir>` 保存每个新 FULL commit；`--diag-log <file>` 追全诊断。

## 7. 构建/烧录失败

- preflight：`scripts\espview_build.bat --check`（退出码 3 = MSYS2/ESP-IDF 探测失败；
  检查 `MINGW64_BIN` / `ESPIDF_PROFILE`）。
- 固件 bin 不存在：flash 退出码 5 → 先 `scripts\espview_build.bat -esp32 -b <profile>`。
- COM 口不存在：flash 退出码 4 → 设备管理器确认端口；关闭占用程序。
- 只想验证流程不真烧：`scripts\espview_flash.bat --dry-run`。
- profile 参数：`-b <name>`（合法字符 A-Z a-z 0-9 _ -），不是 `--profile`。
