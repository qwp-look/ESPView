# ESPView Build / Flash 工作流（M7-D5）

三个批处理脚本把原先手工执行的 verify_host / verify_qt / ESP-IDF profile +
`idf.py` 多条命令收敛为一条命令，并提供可读错误与退出码。**不要求开发者手工敲十几条命令。**

| 脚本 | 作用 |
| --- | --- |
| `espview_build.bat` | 一次构建 host（复用 `verify_host.bat`）+ Qt（复用 `verify_qt.bat`）+ ESP32（`idf.py -B build\<profile> build`） |
| `espview_flash.bat` | 经 ESP-IDF 环境调用 `idf.py flash` 烧录指定固件 |
| `espview_build_flash.bat` | 构建 + 烧录组合，参数透传 |

## 快速上手

```bat
scripts\espview_build.bat              :: host + Qt + ESP32 全构建
scripts\espview_build.bat -host -qt    :: 只构建 host + Qt
scripts\espview_build.bat -esp32       :: 只构建 ESP32 固件（uart_hw）
scripts\espview_build.bat --check      :: 仅 preflight（PATH / MSYS2 / ESP-IDF 探测）

scripts\espview_flash.bat              :: 烧录 COM4 / uart_hw
scripts\espview_flash.bat -p COM5 --no-reset
scripts\espview_flash.bat --dry-run    :: 只做参数解析 + 存在性校验，不烧录

scripts\espview_build_flash.bat -esp32 -p COM4 --no-reset
```

## espview_build.bat

### 参数

| 参数 | 含义 |
| --- | --- |
| `-host` | host 全套（shared/protocol 单测 + ctest + pc 工具），调用 `verify_host.bat` |
| `-qt` | Qt 6 GUI 构建（`espview_virtual_display.exe`），调用 `verify_qt.bat` |
| `-esp32` | ESP32 固件构建（`idf.py -B build\<profile> build`） |
| `-b <profile>` | ESP32 构建子目录（`esp32\build\<profile>`），默认 `uart_hw` |
| `--check` | 只做 preflight（MSYS2 / ESP-IDF 探测），不构建 |
| `-h` / `--help` | 帮助 |

不带参数默认 `-host -qt -esp32` 全做。任一步失败立即停止并返回该步退出码。

### 退出码

| 码 | 含义 |
| --- | --- |
| `0` | 全部通过 |
| `1` | 构建/测试步骤失败（来自 cmake / ctest / idf.py） |
| `2` | 参数错误（未知参数 / 非法 profile 名） |
| `3` | preflight 失败（找不到 MSYS2 MinGW64 或 ESP-IDF） |

### Preflight 检查项

1. MSYS2 MinGW64：`%MINGW64_BIN%`（默认 `C:\msys64\mingw64\bin`）下存在 `g++.exe`、`cmake.exe`、`ctest.exe`。
2. ESP-IDF：先探测 profile 加载（`C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1`），确认 `idf.py` 可用并打印版本；失败给出中文 + 英文可读错误。

## espview_flash.bat

### 参数

| 参数 | 含义 |
| --- | --- |
| `-p <port>` | 串口，默认 `COM4`；裸数字也可（`4` = `COM4`） |
| `-b <profile>` | 构建子目录，默认 `uart_hw`（即烧 `esp32\build\uart_hw\espview_esp32.bin`） |
| `--no-reset` | 烧录后不复位芯片。ESP-IDF v6.0.2 的 `idf.py flash` 没有原生 `--no-reset`，脚本映射为 esptool `--after no-reset`（经 `idf.py --extra-args` 透传） |
| `--dry-run` | 只做参数解析 + COM 口 + bin 存在性校验，**不实际烧录** |
| `-h` / `--help` | 帮助 |

### 退出码

| 码 | 含义 |
| --- | --- |
| `0` | 通过（含 dry-run 校验通过） |
| `2` | 参数错误（非法端口 / profile） |
| `3` | ESP-IDF profile 缺失或探测失败 |
| `4` | COM 口不存在（用 `[System.IO.Ports.SerialPort]::GetPortNames()` 检查） |
| `5` | 固件 bin 不存在（先跑 `espview_build.bat -esp32`） |
| `6` | 烧录失败（idf.py / esptool） |

脚本只显示固件 bin 路径与 profile 名，**不打印、不写入任何凭据**（详见下方安全说明）。

## espview_build_flash.bat

先构建再烧录；构建参数（`-host`/`-qt`/`-esp32`/`-b`/`--check`）与烧录参数（`-p`/`--no-reset`/`--dry-run`）均可透传。默认 `-host -qt -esp32 -p COM4 -b uart_hw`；指定了任一 `-host`/`-qt`/`-esp32` 时只构建指定目标。退出码 = 首个失败步骤的码（build 0/1/2/3，flash 0/2/3/4/5/6）。

## Profile 说明

固件行为由 ESP32 Kconfig 决定（本阶段**不做任何 Kconfig 修改**），profile = `esp32\build\<name>` 构建目录，用 `-b` 选择：

| Profile | 状态 | 特征 |
| --- | --- | --- |
| `uart_hw`（当前） | 现成构建目录 | **Development UART**：UART 传输、`ESPVIEW_DEFAULT_MODE=2`（Mirror）、OLED、LVGL、TEST hooks（F11/F12 验收钩子） |
| `tcp_production`（规划中） | 未创建 | **Production TCP**：TCP + OLED、NO TEST hooks（生产默认 `sdkconfig.defaults` 已把 TEST hooks 置 n） |
| `dev_uart`（规划中） | 未创建 | **Developer UART**：UART + OLED + diagnostics |

新 profile = 用 menuconfig 配置后另建构建目录（如 `idf.py -B build\tcp_production` 生成），脚本无需改动，直接 `-b tcp_production` 构建/烧录。

### 凭据与隐私

- Wi-Fi SSID / 密码只存在于本地未跟踪的 `esp32\sdkconfig`（已被 `.gitignore` 排除）。
- 三个脚本**从不读取或打印 `esp32\sdkconfig`**，也不会把任何凭据写入文件。
- 设计约定详见 `docs/DESIGN.md` §X.7–X.8（生产/测试 profile 分离、持久化白名单、凭据不入库）。

## 环境变量

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `MINGW64_BIN` | `C:\msys64\mingw64\bin` | MSYS2 MinGW64 工具目录（g++/cmake/ctest） |
| `ESPIDF_PROFILE` | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` | ESP-IDF PowerShell profile 路径 |

## 常见问题（FAQ）

**COM 口被占用 / 找不到端口**

- 关闭占用串口的程序（`idf.py monitor`、PC 端应用、串口助手）。
- 拔插 USB，确认设备管理器里 CH340 枚举的端口号；用 `-p` 指定实际端口。
- 脚本用 `[System.IO.Ports.SerialPort]::GetPortNames()` 检查端口是否存在，不存在返回退出码 `4`。

**idf.py 找不到**

- 确认 Espressif 安装完整、profile 文件存在（`C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1`）。
- 安装位置不同时设置 `ESPIDF_PROFILE`。
- 先跑 `scripts\espview_build.bat --check`（或 `-esp32 --check`）单独验证探测步骤。

**MSYS2 / cmake 找不到**

- 确认 `C:\msys64\mingw64\bin` 下有 `g++.exe`、`cmake.exe`、`ctest.exe`；位置不同时设置 `MINGW64_BIN`。

**提示固件 bin 不存在**

- 先构建：`scripts\espview_build.bat -esp32 -b <profile>`（flash 退出码 `5`）。

**想验证烧录流程但不想真烧**

- `scripts\espview_flash.bat --dry-run`：参数解析 + COM 口 + bin 存在性校验全过但不动硬件。