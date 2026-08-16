# ESPView Build / Flash 工作流（M7-D5）

三个批处理脚本把原先手工执行的 verify_host / verify_qt / ESP-IDF profile +
`idf.py` 多条命令收敛为一条命令，并提供可读错误与退出码。**不要求开发者手工敲十几条命令。**

| 脚本 | 作用 |
| --- | --- |
| `espview_build.bat` | 一次构建 host（复用 `verify_host.bat`）+ Qt（复用 `verify_qt.bat`）+ ESP32（`idf.py -B build\<profile> build`） |
| `espview_flash.bat` | 经 ESP-IDF 环境调用 `idf.py flash` 烧录指定固件 |
| `espview_build_flash.bat` | 构建 + 烧录组合，参数透传 |
| `espview_e_ab_harness.py` | M7-E A/B/C 硬件实验（OLED active/suspended/disabled + Wi-Fi scan，可重复、可 diff） |

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
| `-b <profile>` | ESP32 构建子目录（`esp32\build\<profile>`），默认 `uart_hw`；每个 profile 使用自己的 `build\<profile>\sdkconfig`（首次从 `esp32\sdkconfig` 引导拷贝），互不漂移 |
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
| `--no-reset` | 烧录后不复位芯片。ESP-IDF v6.0.2 的 `idf.py flash` 没有原生 `--no-reset`，脚本直接调用 esptool（`--before default-reset --after no-reset write-flash @flash_args`）烧录并保持芯片运行 |
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

先构建再烧录；构建参数（`-host`/`-qt`/`-esp32`/`-b`/`--check`）与烧录参数（`-p`/`--no-reset`/`--dry-run`）均可透传。默认 `-host -qt -esp32 -p COM4 -b uart_hw`；指定了任一 `-host`/`-qt`/`-esp32` 时只构建指定目标。`--check` 只做 preflight 且自动把烧录参数加 `--dry-run`（绝不真烧）。退出码 = 首个失败步骤的码（build 0/1/2/3，flash 0/2/3/4/5/6）。

## Profile 说明

固件行为由 ESP32 Kconfig 决定（本阶段**不做任何 Kconfig 修改**），profile = `esp32\build\<name>` 构建目录，用 `-b` 选择：

| Profile | 状态 | 特征 |
| --- | --- | --- |
| `uart_hw`（当前） | 现成构建目录 | **Development UART**：UART 传输、`ESPVIEW_DEFAULT_MODE=2`（Mirror）、OLED、LVGL、TEST hooks（F11/F12 验收钩子） |
| `tcp_production`（规划中） | 未创建 | **Production TCP**：TCP + OLED、NO TEST hooks（生产默认 `sdkconfig.defaults` 已把 TEST hooks 置 n） |
| `dev_uart`（规划中） | 未创建 | **Developer UART**：UART + OLED + diagnostics |

新 profile = 用 menuconfig 配置后另建构建目录（如 `idf.py -B build\tcp_production` 生成），脚本无需改动，直接 `-b tcp_production` 构建/烧录。

**M7-F F4：profile sdkconfig 隔离**——每个 profile 使用自己的 `esp32\build\<profile>\sdkconfig`（脚本通过 `idf.py -DSDKCONFIG=...` 显式指定）。首次构建/烧录时若该文件不存在，脚本从本地 `esp32\sdkconfig` 引导拷贝一份（保留 UART 引脚、波特率与 Wi-Fi 凭据），此后各 profile 完全隔离，共享的 `esp32\sdkconfig` 不再被构建/烧录覆盖。

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

## A/B/C 硬件实验（M7-E，espview_e_ab_harness.py）

对照同一块板子上 Wi-Fi 扫描期间 OLED 的三种行为，跑相同的空口扫描并输出
结构化文本（可重复、可 diff）：

| mode | 固件配置 | OLED 预期 |
| --- | --- | --- |
| A | `CONFIG_ESPVIEW_SCAN_SUSPEND_OLED=n`（+ OLED enable） | active（扫描中持续刷新/发 preview） |
| B | `CONFIG_ESPVIEW_SCAN_SUSPEND_OLED=y`（默认，+ OLED enable） | suspended（扫描中挂起，preview 停发） |
| C | `CONFIG_ESPVIEW_OLED_ENABLE=n` | disabled（无 oled 诊断行） |

每个 mode 流程：构建 profile（`--build`，等价 idf.py 流程，`SDKCONFIG_DEFAULTS`
= 仓库 `sdkconfig.defaults` + `%TEMP%` 白名单 override，只含上述两个 Kconfig 键）
→ 烧录（`--flash`，复用 `espview_flash.bat -b <profile> --no-reset`）→ 打开
COM4 @ 115200 → DTR/RTS 复位 → HELLO 握手（PC HELLO 含 nameLen 字段）→
`WIFI_SCAN_REQ`（ACK_REQ，maxEntries=32）→ 记录。**默认不构建/不烧录**（假设
固件已就绪，避免并行代理冲突）；测试结果默认只打印 stdout，`--result-file`
可追加写入仓库外临时文件。

记录字段（`# key=value` 汇总 + `[evt]` 事件流）：
- 时间：`started_iso`、事件行 `t=+<相对秒>`、`hello_ms`/`ack_ms`/`scan_duration_ms`/`scan_esp_phase1_ms`
- 扫描：`scan_req_tx`/`ack_rx`/`scan_result_rx`、`scan_count`、`scan_total`、`scan_truncated`、`scan_seq`、`wifi_status_phases`
- RSSI/channel：`rssi_min/max/avg`、`channels`（每条记录另含 ssid/bssid/rssi/ch/auth）
- OLED：`oled_lines`、`oled_err_first/last/delta`、`oled_ok_last`、`oled_config`、`oled_state_observed`、`preview_before/during/after`
- UART/会话：`uart_disconnects`、`readfile_errors`、`write_errors`、`reopens`、`reboots_expected/unexpected`（`rst:` 横幅）、`session_transitions`、`peer_timeouts`、ACK 字段、`crc_errors`/`bad_magic`/`protocol_errors`

用法与退出码：

```bat
py -3.10 scripts\espview_e_ab_harness.py                    :: A+B+C，不构建不烧录（假设已烧好）
py -3.10 scripts\espview_e_ab_harness.py --build --flash     :: 全流程：构建+烧录+实验
py -3.10 scripts\espview_e_ab_harness.py --modes A,B --iterations 3
py -3.10 scripts\espview_e_ab_harness.py --strict --result-file %TEMP%\abc.txt
py -3.10 scripts\espview_e_ab_harness.py --dry-run --build --flash   :: 只打印计划
```

退出码：`0` 全部 PASS / `1` 任一 mode FAIL / `2` 用法、环境或构建/烧录失败。
安全：脚本从不读取 `esp32/sdkconfig`，只发 `WIFI_SCAN_REQ`（不配置网络、密码零参与）；
SSID 为非秘密 metadata（同既有探针）。依赖 `pyserial`（`py -3.10 -m pip install pyserial`）。
