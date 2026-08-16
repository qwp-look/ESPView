# UART 链路（Protocol Transport）

> ESPView 的默认传输 = UART over USB（CH340）。本文给出真实参数、COM 口处理、
> 掉线说明与实测性能，全部与 [docs/DESIGN.md](DESIGN.md) E/J/N/X 节一致。

## 1. Baseline（冻结）

| 参数 | 值 |
| --- | --- |
| 波特率 | **115200**（8N1，M1-3C 冻结的正式 baseline） |
| 协议 UART | ESP32 UART0（默认 TX=GPIO1 / RX=GPIO3） |
| console | 关闭（`CONFIG_ESP_CONSOLE_NONE=y`） |
| 单包 | 20B header + ≤4096B payload（4116B 实测约 356ms @115200） |
| 整帧 FULL（320×240 RGB565） | 153600 B ≈ **13.5s**，有效 payload ≈ **11.1 KB/s** |
| 常态更新 | dirty-rect PARTIAL（小矩形时负载 <5%，见 DESIGN.md J 节实测） |

为什么 FULL 这么慢：这是 115200 baud 的物理上限（约 11.1 KB/s 有效吞吐），不是软件缺陷。
UI 动画密集或大面积变化时可能掉帧；协议按「整帧提交/整帧丢弃」处理（TX 背压）。

## 2. 921600：experimental only

- 921600 是 **experimental**：经典 ESP32 + 当前 CH340/Windows/驱动组合下，大帧突发实测
  不可靠（丢字节）；短包/控制面可用（理论上限 ≈92 KB/s、单包 ≈45ms）。
- 2M 波特率在部分 CH340 版本上不稳。
- 结论：**不要用 921600 做验收**；需要吞吐请走 TCP（Wi-Fi，见 [docs/wifi.md](wifi.md)）。

## 3. COM 口处理

- 端口号以 Windows 设备管理器为准（CH340 枚举出的 COMx）；本项目验收用 COM3/COM4。
- 脚本与工具统一 `-p COMx` 参数，裸数字等价（`4` = `COM4`）；默认 COM4。
- 端口存在性用 `[System.IO.Ports.SerialPort]::GetPortNames()` 检查
  （`scripts\espview_flash.bat` 退出码 4 = COM 口不存在）。
- 端口被占用时：关闭占用程序（`idf.py monitor`、PC 端应用、串口助手），拔插 USB 后
  重新确认端口号。

## 4. CH340 掉线说明（固定措辞）

- **RF 上电与 USB-UART/板级环境不稳定高度相关；物理机制属 hypothesis/high confidence；电源余量/EMI/USB 瞬态待硬件验证。**（不得写为「已确认供电不足」）
- 现象：Wi-Fi RF 上电（boot 期或扫描请求后）期间，PC 侧偶发 `ReadFile err=5` /
  `PermissionError 5`（CH340 链路断开），少数复现 ESP32 无复位横幅挂死。
- 判别证据：无 RF 的 `uart_hw` 固件 45s 无掉线（HELLO t=663ms）；RF 固件 boot
  ~747ms 掉线（DESIGN.md AK.3 证据矩阵）。UART-only 模式下该链路稳定。
- 排查步骤见 [docs/troubleshooting.md](troubleshooting.md)「CH340 掉线」。

## 5. 真实命令

```bat
:: 烧录 UART 验收固件（uart_hw profile）到 COM4
scripts\espview_flash.bat -p COM4

:: 构建 + 烧录组合（构建参数与烧录参数都可透传；--check 隐含 flash --dry-run）
scripts\espview_build_flash.bat -esp32 -p COM4

:: host-only 验证（无需硬件）：协议套件 + ctest + com3_frame_test --selftest-queue
scripts\verify_host.bat

:: 被动监控 OLED 诊断行（a=0x3C c=SSD1306 err= ok=），COM 口可换
py -3.10 scripts\pc_oled_monitor.py

:: LVGL + UART 帧流 sanity（COM4 @ 115200，真实硬件）
py -3.10 scripts\pc_com3_lvgl_sanity.py

:: 注：probe/input 工具由 pc\CMakeLists.txt 注册，verify_host.bat 默认只构建
:: com3_frame_test / tcp_transport_test / transport_config_test；用下面命令补建：
cmake -S pc -B build\verify_host\pc -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=OFF
cmake --build build\verify_host\pc --target wifi_provision_probe win32_com_probe input_send_test -j 8

:: Wi-Fi provisioning 探测工具（UART 控制路径；不接收/不打印任何凭据）
build\verify_host\pc\wifi_provision_probe.exe --port COM4 --baud 115200

:: 最小 Win32 串口探针（掉线取证，--pulse-reset 触发 EN 复位）
build\verify_host\pc\win32_com_probe.exe --port COM4 --pulse-reset
```

## 6. UART 会话语义（速览）

- HELLO 握手（ESP32→PC 被动 HELLO；PC 回 HELLO 后 CAPABILITIES 下发）→ FULL resync。
- 断线/重连/切换后一律会话重置 + FULL resync；重连成功后 PC 清黑画面，等首帧 FULL。
- ACK 只服务控制消息（SET_MODE / WIFI_SCAN_REQ / WIFI_CONFIG）；数据面（FRAME_*）
  丢帧自重同步、背压整帧丢弃。
