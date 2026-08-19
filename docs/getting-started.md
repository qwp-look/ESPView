# ESPView 快速上手（Getting Started）

> 面向第一次拿到仓库、想在本机把「ESP32 固件 + PC Qt 虚拟显示」跑起来的开发者。
> 适用于协议冻结的 v0.1（里程碑 M0..M8-A7 已提交）。
> 设计细节与冻结语义以 [docs/DESIGN.md](DESIGN.md) 为准；本文只给出可用路径与命令。

## 1. 你需要什么

| 类别 | 要求 | 说明 |
| --- | --- | --- |
| PC | Windows 10/11 | 当前工具链均为 Windows 路径（MSYS2 / ESP-IDF profile / COM 口） |
| 编译器 | MSYS2 MinGW64（`g++.exe`、`cmake.exe`、`ctest.exe`） | 默认 `C:\msys64\mingw64\bin`，可用环境变量 `MINGW64_BIN` 覆盖 |
| GUI | Qt 6（MSYS2 MinGW64 版，含 Widgets + SerialPort） | 只有 `-qt` 目标需要 |
| 固件 | ESP-IDF v6.0.2（PowerShell profile） | 默认 `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1`，可用环境变量 `ESPIDF_PROFILE` 覆盖 |
| 脚本 | Python 3.10 + `pyserial` | 仅硬件验收脚本需要（`py -3.10 -m pip install pyserial`） |
| 硬件 | 经典 ESP32 开发板（验收板 ESP32-D0WDQ6 rev v1.1）+ 板载或外置 CH340 | 详见 [docs/hardware.md](hardware.md) |
| 可选 | 128×64 I2C OLED（SSD1306/SH1106） | 诊断显示 + 物理显示后端，详见 [docs/oled.md](oled.md) |

## 2. 克隆与工具链检查

```powershell
# 1) 克隆（URL 用你自己的仓库地址）
git clone <your-repo-url> ESPView
cd ESPView

# 2) 只做 preflight（MSYS2 / ESP-IDF 探测），不构建
scripts\espview_build.bat --check
```

`--check` 通过时会打印 preflight OK（MSYS2 MinGW64 与 ESP-IDF v6.0.2 均可探测到）。

## 3. 构建

```bat
:: 全量构建：host 测试 + Qt GUI + ESP32 固件（默认 profile uart_hw）
scripts\espview_build.bat

:: 子集：只构建 host+Qt / 只构建 ESP32
scripts\espview_build.bat -host -qt
scripts\espview_build.bat -esp32 -b uart_hw
```

说明：

- `-b <profile>` 选择 ESP32 构建目录（默认 `uart_hw`）；每个 profile 使用自己的
  `esp32\build\<target>\<profile>\sdkconfig`（首次从本地 `esp32\sdkconfig` 引导拷贝），互不漂移。
- `uart_hw` 是当前现成的 UART 验收固件：UART 传输、`ESPVIEW_DEFAULT_MODE=2`（Mirror）、
  OLED、LVGL、TEST hooks（F11/F12 验收钩子）。
- 退出码：`0` 全过 / `1` 构建或测试失败 / `2` 参数错误 / `3` preflight 失败。

## 4. 烧录固件

```bat
:: 烧录默认 profile（uart_hw）到 COM4
scripts\espview_flash.bat -p COM4

:: 只校验参数 + COM 口 + bin 存在性，不真烧
scripts\espview_flash.bat --dry-run
```

- 端口：`-p COMx`，裸数字也可以（`4` = `COM4`）；默认 `COM4`。
- `--no-reset`：烧录后不复位芯片（直接调 esptool，`--after no-reset`）。
- 退出码：`0` 通过 / `2` 参数错误 / `3` ESP-IDF 不可用 / `4` COM 口不存在 /
  `5` 固件 bin 不存在（先跑 `scripts\espview_build.bat -esp32`）/ `6` 烧录失败。
- 脚本只显示固件路径与 profile 名，不读取、不打印任何 Wi-Fi 凭据。

## 5. 启动 Qt 虚拟显示并连接 UART

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
build\verify_qt\espview_virtual_display.exe --transport uart --port COM4 --baud 115200
```

预期行为（首次启动）：

1. GUI 打开：窗口左侧是 VirtualScreen（320×240 等比缩放），下方是状态面板。
2. 应用对 ESP32 发复位脉冲并等待 HELLO 握手（被动等待 + 7s 主动重试）。
3. 握手完成后收到 CAPABILITIES，随后 ESP32 开始推 LVGL 画面。
4. **第一帧 FULL 在 UART 115200 下约需 13.5 秒**（320×240 RGB565 = 153600 B，
   有效 payload ≈ 11.1 KB/s）；之后常态是 dirty-rect PARTIAL 增量，小改动远快于此。

常用 GUI 参数（详见 `espview_virtual_display.exe --help`）：

| 参数 | 含义 |
| --- | --- |
| `--transport uart|tcp` | 传输类型（默认读 QSettings，否则 tcp） |
| `--port <COM>` | 串口名（UART 模式） |
| `--baud 115200` | 波特率（UART 模式；baseline = 115200） |
| `--tcp-bind <ip>` | TCP Server 监听地址（TCP 模式） |
| `--tcp-port 8765` | TCP Server 端口（TCP 模式） |
| `--dump-png <dir>` | 每个新 FULL commit 保存 `full_<frameId>.png` |
| `--diag-log <file>` | 追加诊断行到文件 |
| `--no-reset` | 跳过 UART DTR/RTS 复位脉冲 |

## 6. 可选：Wi-Fi/TCP 模式

TCP 模式当前是**编译期链路**（ESP32 固件侧选 Wi-Fi STA + TCP）；Wi-Fi provisioning
（UART 引导配网）处于 experimental/受限状态，详见 [docs/wifi.md](wifi.md)。

快速路径（PC 侧）：

```powershell
build\verify_qt\espview_virtual_display.exe --transport tcp --tcp-bind 0.0.0.0 --tcp-port 8765
```

- ESP32 固件需以 TCP transport 构建，并通过本地未跟踪的 `esp32\sdkconfig`
  （或 `idf.py menuconfig`）注入 Wi-Fi SSID/密码与 PC server IP（**全部用占位符，
  绝不提交真实凭据**，见 [docs/security.md](security.md)）。
- 没有真实局域网凭据时，UART 模式是推荐的第一条验收路径。

## 7. 验证你的环境

| 入口 | 作用 | 需要硬件？ |
| --- | --- | --- |
| `scripts\verify_host.bat` | 协议 host 套件 + ctest + PC 工具（含 TCP loopback） | 否 |
| `scripts\verify_qt.bat` | Qt GUI 目标编译检查 | 否 |
| `scripts\espview_build.bat --check` | 工具链 preflight | 否 |
| `scripts\espview_flash.bat --dry-run` | 烧录参数与文件校验 | 需要 COM 口存在（不真烧） |

## 8. 下一步阅读

- [docs/hardware.md](hardware.md) — 硬件要求与接线
- [docs/uart.md](uart.md) — UART 链路细节（115200 baseline / 921600 experimental）
- [docs/wifi.md](wifi.md) — Wi-Fi/TCP 现状与配网向导
- [docs/display-modes.md](display-modes.md) — 四种显示模式
- [docs/troubleshooting.md](troubleshooting.md) — 常见故障排查
- [docs/architecture-overview.md](architecture-overview.md) — 高层架构
