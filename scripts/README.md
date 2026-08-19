# ESPView Build / Flash / Verify 工作流（M8-A7）

本目录是 ESPView 的**工具链层**：构建、烧录、验证、静态检查、硬件实验 harness。
所有脚本只在 `scripts/` 内；固件（`esp32/`）、PC 端（`pc/`）、共享库（`shared/`）、
文档（`README.md`、`docs/`）由其他代理独占，本层只读引用。

## 入口一览

| 脚本 | 作用 | 子命令 |
| --- | --- | --- |
| `espview_build.bat` | 一次构建 host（复用 `verify_host.bat`）+ Qt（复用 `verify_qt.bat`）+ ESP32（`idf.py -B build\<profile> build`） | `build`（默认）、`verify`、`profile`、`check`、`dry-run` |
| `espview_flash.bat` | 经 ESP-IDF 环境烧录；参数预检（COM 口 / bin / flash_args）；`probe` 集成 `win32_com_probe` | `flash`（默认）、`probe`、`profile`、`check`、`dry-run` |
| `espview_verify.bat` | 验证入口：host / Qt / LVGL（opt-in）/ ESP32 preflight | `verify`（默认）、`check`、`dry-run`、`profile` |
| `espview_build_flash.bat` | 构建 + 烧录组合 | `build-flash`（默认）、`build`、`flash`、`check`、`dry-run`、`verify`、`profile` |
| `espview_e_ab_harness.py` | M7-E A/B/C 硬件实验（OLED active/suspended/disabled + Wi-Fi scan） | `--build --flash --dry-run` 等 |
| `espview_g1_harness.py` | M7-G G1 A/B/C/D 硬件实验（OLED × RF 矩阵，机器可解析） | `--build --flash --probe --dry-run` 等 |
| `check_docs.bat` / `check_docs.py` | 静态文档检查（无构建、无硬件） | `--help` |
| `espview_profile_sdkconfig.py` | Profile sdkconfig 管理器（白名单 + 隔离 + 防 drift） | `--list / --show / --check / --apply` |
| `espview_profiles.py` | 白名单 / label 表 / Kconfig 键映射（唯一事实源） | 内部模块 |
| `security_scan.py` | 凭据/secret 模式扫描（git tracked 文件；显式 allowlist 语义） | `--help`、`--list-allowlist` |
| `check_bat_crlf.py` | 全部 tracked `.bat` 必须 CRLF（只报告，不重写） | `--help` |
| `ci_esp32_build.py` | ESP32 profile 构建编排（CI 辅助，可选） | `--help` |
| `ci_collect_artifacts.py` | release 产物收集/重命名 + SHA256SUMS（禁传 sdkconfig/日志） | `--help` |
| `run_bench.bat` / `run_bench.sh` | M8-A6 本地基准：构建两个 bench 可执行 → 运行 → 与提交基线比较（`--quick` = CI smoke 档） | `--quick` |
| `bench_compare.py` | M8-A6 基准 CSV 比较：按 (op, payload) 比中位数行，回归 &gt;25% 失败；`stream_encode` alloc_count=0 门槛 | `--threshold`、`--alloc-zero-ops`、`--warn-only` |

CI 分层与触发矩阵见 [docs/ci.md](../docs/ci.md)；workflow 位于 `.github/workflows/`。

## 快速上手

```bat
scripts\espview_build.bat                    :: host + Qt + ESP32 全构建（默认 -b uart_hw）
scripts\espview_build.bat -host -qt          :: 只构建 host + Qt
scripts\espview_build.bat -esp32 -b tcp      :: 只构建 ESP32 固件（TCP profile）
scripts\espview_build.bat --check            :: 仅 preflight（PATH / MSYS2 / ESP-IDF 探测）
scripts\espview_build.bat --dry-run          :: 打印构建计划，不执行

scripts\espview_flash.bat                    :: 烧录 COM4 / uart_hw（参数预检）
scripts\espview_flash.bat -p COM5 --no-reset
scripts\espview_flash.bat --dry-run          :: 只做参数解析 + 存在性校验，不烧录
scripts\espview_flash.bat probe -p COM4 --pulse-reset   :: win32_com_probe 链路检查

scripts\espview_verify.bat                   :: host + Qt 验证（默认）
scripts\espview_verify.bat -lvgl             :: LVGL 验证（含 ESP32 构建，opt-in）

scripts\espview_build_flash.bat -esp32 -p COM4 --no-reset
scripts\espview_build_flash.bat check        :: 构建 preflight + 烧录 dry-run，绝不真烧

scripts\espview_build.bat profile list       :: 白名单 + 7 属性表
scripts\check_docs.bat                       :: 静态文档检查
```

## Profile 系统（M8-A7 更新）

### 白名单与 label 表

`-b / --profile` 只接受白名单 profile（`espview_profiles.py` 为唯一事实源），
每个 profile 回答 7 个属性：**Transport / OLED / Test transport switch / Console /
Wi-Fi / Display / Render**。`uart_hw` 是 `uart` 的 legacy 别名。

| Profile | Transport | OLED | Test switch | Console | Wi-Fi | Display | Render | 用途 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `uart`（`uart_hw` 别名） | UART | ON | ON | NONE | OFF | LVGL | FAST | UART 开发基线（F12 hooks） |
| `tcp` | TCP | ON | OFF | NONE | ON | LVGL | FAST | TCP 生产 |
| `oled` | UART | ON | OFF | NONE | ON | LVGL | FAST | OLED + 扫描实验 |
| `oled-off` | UART | OFF | OFF | NONE | ON | LVGL | FAST | OLED 关对比 |
| `diagnostic` | UART | ON | OFF | NONE | OFF | TestPattern | FAST | 确定性帧回归 |
| `g1_a` | UART | OFF | OFF | NONE | OFF | LVGL | FAST | G1 A：OLED OFF + RF OFF |
| `g1_b` | UART | ON | OFF | NONE | OFF | LVGL | FAST | G1 B：OLED ON + RF OFF |
| `g1_c` | UART | OFF | OFF | NONE | ON | LVGL | FAST | G1 C：OLED OFF + RF ON |
| `g1_d` | UART | ON | OFF | NONE | ON | LVGL | FAST | G1 D：OLED ON + RF ON |

- `Console` 恒为 `NONE`（`CONFIG_ESP_CONSOLE_NONE=y`），所有 profile 一致。
- `uart_hw` = `uart` 的 legacy 别名：属性相同，构建目录 = `esp32\build\esp32\uart_hw`（M8-C C4 per-target）。
- 查询：`scripts\espview_build.bat profile list` / `profile show <name>` /
  `profile check <name>`（等价 `espview_flash.bat profile ...`）。

### 构建产物可识别

每次 `-esp32` 构建前脚本打印 traceability（target + profile + label + 7 属性 +
独立 sdkconfig + build dir），固件产物路径固定为 `esp32\build\<target>\<profile>\espview_esp32.bin`。

### Build/Flash 隔离（M8-C C4）

- 构建目录 = `esp32\build\<target>\<profile>\`（`-t` 与 `-b` 共同决定）：
  同一 profile 名称在不同 IDF target 下绝不共享目录/sdkconfig（防 target 漂移）。
- `espview_flash.bat` 是 **artifact-only**：绝不重新 configure、绝不 apply profile、
  绝不改写 sdkconfig；只烧录已有构建产物，并对 `CONFIG_IDF_TARGET` 做只读交叉校验
  （build dir 与 `-t` 不匹配时报错退出）。

### Profile 完全隔离（防 drift）

- 每个 profile 使用**自己的** `esp32\build\<target>\<profile>\sdkconfig`；
- 首次构建/烧录时由 `espview_profile_sdkconfig.py --apply` 从本地
  `esp32\sdkconfig`（或 `sdkconfig.defaults`）**整文件拷贝引导**（不检查内容，
  凭据/引脚原样保留），随后强制应用该 profile 的白名单 Kconfig 键；
- 每次构建/烧录都重新 `--apply`，白名单键永不漂移（不再出现 mode_c 误带
  `OLED=y` / `TCP=y` 之类问题）；共享 `esp32\sdkconfig` 永不被覆盖；
- Kconfig choice 组（UART/TCP、LVGL/TestPattern）显式处理，同类互斥；
- 脚本**从不打印 sdkconfig 内容**，白名单拒绝任何形似凭据的键
  （`SSID/PASSWORD/PSK/TOKEN/SECRET`）。

### 凭据与隐私

- Wi-Fi SSID / 密码只存在于本地未跟踪的 `esp32\sdkconfig`
  （已被 `.gitignore` 排除）与各 profile 的独立 sdkconfig（同被忽略）。
- 所有脚本**从不读取、打印或硬编码 Wi-Fi 凭据**；`build\set_tcp_cfg.py`
  （未跟踪、含硬编码真凭据）**不再被任何脚本调用**——TCP profile 的凭据
  通过整文件引导拷贝保留在 profile 自己的 sdkconfig 中。
- 如需为 TCP profile 配置凭据：先 `espview_build.bat -esp32 -b tcp` 构建一次，
  再 `idf.py -B build\esp32\tcp menuconfig` 设置 SSID/password，重新构建即可
  （凭据只落在 `esp32\build\esp32\tcp\sdkconfig`）。

## espview_build.bat

### 参数

| 参数 | 含义 |
| --- | --- |
| `-host` | host 全套（shared/protocol 单测 + ctest + pc 工具），调用 `verify_host.bat` |
| `-qt` | Qt 6 GUI 构建（`espview_virtual_display.exe`），调用 `verify_qt.bat` |
| `-esp32` | ESP32 固件构建（`idf.py -B build\<profile> build`） |
| `-b <profile>` | 白名单 profile（默认 `uart_hw`，= `uart` 别名） |
| `--profile <profile>` | 同 `-b` |
| `--check` | 只做 preflight（MSYS2 / ESP-IDF 探测 + profile 校验），不构建 |
| `--dry-run` | 打印精确构建计划，不执行 |
| `verify` | 委托 `espview_verify.bat`（剩余参数透传） |
| `profile` | `list` / `show <name>` / `check <name>` |
| `-h` / `--help` | 帮助 |

不带参数默认 `-host -qt -esp32`。任一步失败立即停止并返回该步退出码。

### 退出码

| 码 | 含义 |
| --- | --- |
| `0` | 全部通过（含 dry-run 计划打印） |
| `1` | 构建/测试步骤失败 |
| `2` | 参数错误（未知参数 / 未知 profile / 非法 profile 名） |
| `3` | preflight 失败（MSYS2 / ESP-IDF / python 缺失） |

## espview_flash.bat

### 参数

| 参数 | 含义 |
| --- | --- |
| `-p <port>` | 串口，默认 `COM4`；裸数字也可（`4` = `COM4`） |
| `--baud <n>` | 波特率（probe 用），默认 `115200` |
| `-b <profile>` | 白名单 profile，默认 `uart_hw`（烧 `esp32\build\uart_hw\espview_esp32.bin`） |
| `--no-reset` | 烧录后不复位（esptool `--before default-reset --after no-reset write-flash @flash_args`）；`flash_args` 缺失时给出明确错误（退出码 5） |
| `--dry-run` / `--check` | 参数解析 + COM 口 + bin + flash_args 存在性校验，不烧录 |
| `--any-profile` | 跳过白名单（仅 harness 内部目录如 M7-E `mode_a/b/c` 使用） |
| `probe` / `verify` | 运行 `build\win32_probe\win32_com_probe.exe --port <p> --baud <b> [--pulse-reset] [--timeout-ms N]` |
| `profile` | `list` / `show <name>` / `check <name>` |

### 退出码

| 码 | 含义 |
| --- | --- |
| `0` | 通过（含 dry-run 校验通过） |
| `2` | 参数错误（非法端口 / profile） |
| `3` | ESP-IDF profile 缺失 / probe exe 缺失 / 探测失败 |
| `4` | COM 口不存在（`GetPortNames()` 检查；probe 打开失败也映射到此） |
| `5` | 固件产物缺失（bin 或 `flash_args`；先构建） |
| `6` | 烧录失败（idf.py / esptool）或 probe 运行失败 |

`probe` 找不到 exe 时给出构建提示（`pc\src\win32_com_probe.cpp`，见
`pc\CMakeLists.txt`），可用环境变量 `WIN32_COM_PROBE` 覆盖路径。

## espview_verify.bat

默认 `-host -qt`；`-lvgl` 调用 `verify_lvgl.bat`（含 ESP32 `idf.py build`，
opt-in）；`-esp32` 只做 ESP-IDF preflight（真实构建请用 `espview_build.bat`）；
`--check` 只探测工具链；`--dry-run` 打印计划。退出码 `0/1/2/3` 同 build。

## espview_build_flash.bat

先构建再烧录；构建参数（`-host`/`-qt`/`-esp32`/`-b`/`--check`）与烧录参数
（`-p`/`--no-reset`/`--dry-run`/`--any-profile`）均可透传。默认
`-host -qt -esp32 -p COM4 -b uart_hw`；`--dry-run` = 只打印计划；`check` =
构建 preflight + 烧录 dry-run（绝不真烧）；`verify`/`profile` 委托对应脚本。
退出码 = 首个失败步骤的码。

## G1 硬件实验（M7-G §六，espview_g1_harness.py）

A/B/C/D 四 profile（OLED × RF 矩阵）对照同一块板子：

| mode | profile | OLED | RF |
| --- | --- | --- | --- |
| A | `g1_a` | OFF | OFF（`CONFIG_ESP_WIFI_ENABLED=n`） |
| B | `g1_b` | ON | OFF |
| C | `g1_c` | OFF | ON |
| D | `g1_d` | ON | ON |

流程（可选步骤）：构建（`--build`，经 `espview_profile_sdkconfig.py --apply`
+ `idf.py`，每次强制白名单键、防 drift）→ 烧录（`--flash`，复用
`espview_flash.bat -b <p> --no-reset`）→ 链路检查（`--probe`，win32_com_probe
`--pulse-reset`）→ COM 口 → DTR/RTS 复位 → HELLO 握手 → RF-ON 模式发
`WIFI_SCAN_REQ`（ACK_REQ，maxEntries=32）并记录 `WIFI_STATUS` 相位（RF start =
scanning、GOT_IP = got_ip）与 `WIFI_SCAN_RESULT`；RF-OFF 模式只观测
（`rf_start_seen` 必须为 0）。**默认不构建/不烧录**（避免并行代理冲突）。

统计字段（固定格式 `# key=value`，机器可解析）：`boot_ok`、`ch340_open_ok`、
`hello_ok`、`hello_ms`、`rf_start_seen/ms`、`got_ip_seen/ms`、
`scan_req_tx`、`ack_rx`、`ack_status`、`scan_result_rx`、`scan_count/total`、
`scan_truncated`、`rssi_min/max`、`channels`、`uart_disconnects`、
`readfile_errors`、`reopens`、`reboots_expected/unexpected`、
`session_transitions`、`peer_timeouts`、`packets_rx`、`crc_errors`、
`bad_magic`、`protocol_errors`。

原始日志默认保存到 `build\g1_logs\g1_<mode>_it<N>_raw.bin`（串口原始字节）
与 `..._events.txt`（事件流 + 汇总），可用 `--log-dir` 覆盖。

```bat
py -3.10 scripts\espview_g1_harness.py                      :: A+B+C+D，不构建不烧录
py -3.10 scripts\espview_g1_harness.py --build --flash --probe
py -3.10 scripts\espview_g1_harness.py --modes A,D --iterations 3 --strict
py -3.10 scripts\espview_g1_harness.py --dry-run --build --flash
```

退出码：`0` 全部 PASS / `1` 任一 mode FAIL / `2` 用法、环境或构建/烧录/probe 失败。
安全：从不读取 `esp32/sdkconfig` 内容、从不发送/记录凭据（不发 `WIFI_CONFIG`，
密码零参与）；SSID 为非秘密 metadata。依赖 `pyserial`。

> RF-OFF（A/B）profile 依赖固件可编译 `CONFIG_ESP_WIFI_ENABLED=n`
> （G1 固件侧工作）；若构建失败，harness 会给出明确提示。

## A/B/C 硬件实验（M7-E，espview_e_ab_harness.py）

M7-E 遗留 harness（OLED active/suspended/disabled + Wi-Fi scan），用法不变：

```bat
py -3.10 scripts\espview_e_ab_harness.py                    :: A+B+C，不构建不烧录
py -3.10 scripts\espview_e_ab_harness.py --build --flash     :: 全流程
py -3.10 scripts\espview_e_ab_harness.py --dry-run
```

其内部构建目录 `mode_a/mode_b/mode_c` 不在白名单内（`-b` 需加
`--any-profile`），仅作为 M7-E harness 内部产物保留。

## check_docs（M7-G G10）

```bat
scripts\check_docs.bat
```

静态检查（无构建、无硬件）：README 引用的文件/命令/脚本/profile 存在；
README 列出的消息/能力能在代码或 DESIGN.md 找到；不出现已删除的帧满
标志消息名、载荷上限尺寸的错误表述、frameSeq 未定义的表述、真实
Wi-Fi 凭据模式（带真实值的密码键、Wi-Fi 密码相关配置键名、
scripts 中真实私网 IP）；"已验证" 但找不到证据的文字；
`esp32/sdkconfig*` 未被 git 跟踪。退出码 `0` 干净 / `1` 有问题 / `2` 用法或 IO 错误。

## 环境变量

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `MINGW64_BIN` | `C:\msys64\mingw64\bin` | MSYS2 MinGW64 工具目录 |
| `ESPIDF_PROFILE` | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` | ESP-IDF PowerShell profile |
| `ESPVIEW_PYTHON` | `py`（回退 `python`） | Python 解释器（单 exe 路径，不含参数） |
| `WIN32_COM_PROBE` | `build\win32_probe\win32_com_probe.exe` | win32_com_probe 路径覆盖 |
| `ESPVIEW_COM3` | 未设置 | `verify_lvgl.bat` 的 COM3 实机 sanity 开关 |

## 常见问题（FAQ）

**COM 口被占用 / 找不到端口**：关闭占用串口的程序；拔插 USB 后确认 CH340 端口
并用 `-p` 指定；脚本退出码 `4`。也可 `espview_flash.bat probe -p COM4` 做
链路诊断。

**profile 报 unknown**：`espview_build.bat profile list` 看白名单；非白名单
目录（如 M7-E `mode_a/b/c`）烧录需 `--any-profile`。

**RF-OFF 构建失败**：见上文 G1 说明——固件需支持 `CONFIG_ESP_WIFI_ENABLED=n`。

**idf.py 找不到**：确认 Espressif 安装完整、`ESPIDF_PROFILE` 存在；先跑
`espview_build.bat --check`。

**MSYS2 / cmake 找不到**：确认 `MINGW64_BIN` 下有 `g++.exe`、`cmake.exe`、
`ctest.exe`。

**提示固件 bin / flash_args 不存在**：先构建 `espview_build.bat -esp32 -b <profile>`
（flash 退出码 `5`）。

**想验证烧录流程但不想真烧**：`espview_flash.bat --dry-run`；全流程预检
`espview_build_flash.bat check`。

## 行尾与编码约定

- `scripts\*.bat`：CRLF（cmd `goto` 依赖）。
- `scripts\*.py` / `*.md`：LF。
- PowerShell 传参统一 `-D SDKCONFIG=...`（空格形式），规避 5.1 的 `@args` 拆参坑。
