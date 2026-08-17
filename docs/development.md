# ESPView — 开发者文档（Development）

> 面向在 ESPView 仓库内**做开发与验证**的开发者（含并行子代理）。本文档只描述仓库
> 当前真实状态（以 `git HEAD` 为准；当前里程碑 M8-A7——文档/资源预算/target 与 profile 抽象/S3 准备/仓库维护）；协议与架构的**唯一权威**是
> [docs/DESIGN.md](DESIGN.md)，脚本用法以 [scripts/README.md](../scripts/README.md) 最新为准。
>
> 阅读路径：功能总览见 [../README.md](../README.md)（仓库根）；文档索引见
> [docs/README.md](README.md)；CI 分层与 PR gate 见 [docs/ci.md](ci.md)；协作规范见
> [docs/contributing.md](contributing.md)。

## 1. Repository 结构

```
ESPView/
├── shared/                 # 平台无关核心，纯 C++17，零平台依赖（PC 与 ESP32 复用同一份源码）
│   ├── protocol/           # Packet/Message/Frame 三层协议：crc32/packet/message/encoder/
│   │                       #   decoder/frame_assembler/protocol_endpoint/runtime_stats + tests/
│   │                       #   （host 测试伞工程：挂载 input/display/transport/oled/wifi）
│   ├── input/              # InputEvent/InputCodec/InputManager/KeyboardMapper/
│   │                       #   CoordinateMapper/InputPolicy/HidLvglKeymap/LvglInputAdapter
│   ├── display/            # IDisplay/DisplayManager/RemoteDisplay/DisplayRouter/
│   │                       #   DisplayUiState/PhysicalStatus/SplitState/PhysicalCapabilitySnapshot
│   ├── transport/          # TransportManager/TransportSink（运行时传输选择 + TX pacing）
│   ├── oled/               # OledFb（128×64 页式 fb）+ SSD1306/SH1106 命令生成 + 恢复状态机 +
│   │                       #   PhysicalRenderer（RGB565→Mono1）+ OledPreviewSlot（M7-D2）
│   └── wifi/               # ScanTransaction（M7-E Wi-Fi 扫描期间 OLED 挂起/恢复事务状态机）
├── pc/                     # PC 侧（顶层构建入口是 pc/CMakeLists.txt，仓库根没有 CMakeLists.txt）
│   ├── CMakeLists.txt      # 全部 host/Qt 目标：com3_frame_test、espview_virtual_display、
│   │                       #   tcp_transport_test、transport_config_test、input_send_test、
│   │                       #   wifi_provision_probe、win32_com_probe、i18n_test、input_mapper_test
│   └── src/                # serial_transport（HostUartTransport）、host_tcp_transport、
│                           #   serial_worker、connection_manager、virtual_screen_widget、
│                           #   wifi_wizard_*、transport_config、win32_com_probe、input/ 等
├── esp32/                  # ESP-IDF v6.0.2 固件工程（project = espview_esp32）
│   ├── CMakeLists.txt      # include($ENV{IDF_PATH}/tools/cmake/project.cmake)
│   ├── sdkconfig.defaults  # 生产默认（无凭据）：UART 115200、LVGL app、4 MiB flash +
│   │                       #   custom partitions、LWIP TCP SND_BUF=24576、
│   │                       #   CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH=n、CONFIG_PM_ENABLE=y
│   ├── sdkconfig           # 本机未跟踪（.gitignore 排除），含 UART 引脚/波特率与 Wi-Fi 凭据
│   ├── partitions.csv      # nvs 0x9000 / phy_init 0xf000 / factory 0x10000 2 MiB（no OTA）
│   ├── main/               # main.cpp + Kconfig（应用选择/传输/凭据 Kconfig 键）
│   ├── components/         # display、espview、input、lvgl_port、oled、protocol、testpattern
│   └── managed_components/ # lvgl__lvgl（LVGL v8.4，idf.py 组件管理器生成，不提交）
├── scripts/                # 一键构建/烧录/验证入口 + 硬件验收 Python 脚本（见 §3/§4）
├── docs/                   # DESIGN.md（权威）+ README.md（索引）+ development.md + contributing.md
├── README.md               # 项目总览（根）
└── build/                  # host/Qt 构建输出（gitignored）；ESP32 构建在 esp32/build/<profile>/
```

要点：

- `shared/*` 每个子目录都有独立 CMakeLists 静态库目标（`espview_protocol` / `espview_input` /
  `espview_display` / `espview_transport` / `espview_oled` / `espview_wifi`），可单独被 host 与
  ESP32 编译。`shared/protocol` 是 host 测试伞工程（`ESPVIEW_BUILD_TESTS=ON` 时挂载其余子库
  与 `tests/`）。
- ESP32 侧组件直接编译同一份 `shared/` 源码（`esp32/components/{protocol,input,display,oled}`
  + `esp32/components/espview` 复用 `shared/wifi`），不复制实现。
- `pc/CMakeLists.txt` 用 `add_subdirectory(../shared/protocol ...)` 复用协议/输入/显示库；
  Qt GUI 目标可用 `-DESPVIEW_BUILD_QT_GUI=OFF` 跳过（host 回归不依赖 Qt）。

## 2. 构建环境（前置依赖）

| 依赖 | 本机路径 / 版本 | 用途 |
| --- | --- | --- |
| Windows | 10/11（x64） | 开发主机 |
| MSYS2 MinGW64 | `C:\msys64\mingw64\bin`（g++ 15/16、cmake、ctest、Qt 6.11.1） | host / Qt 构建 |
| CMake + CTest | MSYS2 自带（MinGW Makefiles 生成器） | host / Qt 构建与测试 |
| Qt 6 | MSYS2 MinGW64 的 Qt 6.11.1（Widgets + SerialPort） | 仅 PC GUI 目标 |
| ESP-IDF | v6.0.2，PowerShell profile：`C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` | ESP32 固件 |
| Python | ESP-IDF venv：`C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe`（硬件脚本默认；`py -3.10` 亦可） | 硬件验收脚本（pyserial） |
| 硬件 | 经典 ESP32-D0WDQ6（rev v1.1）+ 板载 CH340、OLED SSD1306 128×64（I2C SDA=GPIO21 SCL=GPIO22）、2.4 GHz Wi-Fi | 硬件验收（§4.4） |

环境变量覆盖（脚本已支持）：`MINGW64_BIN`（默认 `C:\msys64\mingw64\bin`）、
`ESPIDF_PROFILE`（默认上面的 ESP-IDF profile 路径）、`ESPVIEW_PYTHON`（默认 ESP-IDF venv python）。

PowerShell 手工载入 ESP-IDF（.bat 脚本内部就是这么做 preflight + 构建的）：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
idf.py --version
```
## 3. 真实构建命令

### 3.1 Host-only（无需 COM / ESP32 / Qt / ESP-IDF）

一键入口（推荐）：`scripts\verify_host.bat`。它依次做：

```bat
:: [1/4] 配置 + 构建 shared/protocol host 单测（含 scan_transaction_test）
cmake -S shared\protocol -B build\verify_host\protocol -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build\verify_host\protocol --target espview_protocol_tests scan_transaction_test -j 8
:: [2/4] ctest（协议套件，20 万+ checks）
ctest --test-dir build\verify_host\protocol --output-on-failure
:: [3/4] 配置 + 构建 pc 工具（Qt OFF）
cmake -S pc -B build\verify_host\pc -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=OFF
cmake --build build\verify_host\pc --target com3_frame_test tcp_transport_test transport_config_test -j 8
:: [4/4] 运行 transport_config_test / com3_frame_test --selftest-queue / tcp_transport_test
```

脚本会把 `%MINGW64_BIN%` 前置到 `PATH`；退出码 `0` = 全部通过，`1` = 失败。

### 3.2 Qt GUI 构建（`espview_virtual_display.exe`）

```bat
scripts\verify_qt.bat
:: 产物：build\verify_qt\espview_virtual_display.exe
```

等价手动命令：

```bat
cmake -S pc -B build\verify_qt -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=ON
cmake --build build\verify_qt --target espview_virtual_display -j 8
```

运行 GUI（先把 MSYS2 bin 加进 PATH；UART 或 TCP 二选一）：

```bat
set PATH=C:\msys64\mingw64\bin;%PATH%
build\verify_qt\espview_virtual_display.exe --transport uart --port COM4 --baud 115200
build\verify_qt\espview_virtual_display.exe --transport tcp --tcp-bind 0.0.0.0 --tcp-port 8765
```

GUI 参数：`--transport uart|tcp`、`--port <COM>`、`--baud`、`--tcp-bind`、`--tcp-port`、
`--dump-png <dir>`、`--diag-log <file>`、`--autoclose-ms N`、`--no-reset`。配置优先级：
CLI > QSettings > 默认（TCP / COM4 / 115200 / `0.0.0.0:8765`）；QSettings 只持久化白名单键
（`transport/type`、`uart/port`、`uart/baud`、`tcp/port`、`window/size`），结构性保证不保存凭据。

### 3.3 ESP32 固件构建（ESP-IDF v6.0.2）

一键：`scripts\espview_build.bat -esp32 -b <profile>`（或 `verify_lvgl.bat`，见 §4.3）。

手动等价（profile sdkconfig 隔离，M7-F F4）：

```powershell
# 1) 载入 ESP-IDF 环境
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
# 2) 进入 esp32/ 构建（-B 指定 profile 构建目录；-D SDKCONFIG 必须用"空格形式"，见 §8 坑 #1）
Set-Location esp32
idf.py -B build\uart_hw -D SDKCONFIG=C:\path\to\ESPView\esp32\build\uart_hw\sdkconfig build
```

profile 机制：每个 profile = `esp32\build\<name>` 独立构建目录 + 独立 sdkconfig（首次构建/烧录
时从本地 `esp32\sdkconfig` 引导拷贝一份，保留 UART 引脚/波特率与 Wi-Fi 凭据），此后各 profile
完全隔离，共享的 `esp32\sdkconfig` 永不被构建/烧录覆盖（详见 `scripts/README.md`「M7-F F4」与
`scripts/espview_build.bat`）。

### 3.4 一键构建 + 烧录

```bat
scripts\espview_build.bat                 :: host + Qt + ESP32（默认 profile）
scripts\espview_build.bat -host -qt       :: 只构建 host + Qt
scripts\espview_build.bat -esp32 -b uart_hw :: 只构建指定 profile 的固件
scripts\espview_build.bat --check         :: 仅 preflight（MSYS2 / ESP-IDF 探测）

scripts\espview_flash.bat                 :: 烧录 COM4 / 默认 profile
scripts\espview_flash.bat -p COM5 --no-reset
scripts\espview_flash.bat --dry-run       :: 只校验（COM 口 + bin 存在），不真烧

scripts\espview_build_flash.bat -esp32 -p COM4 --no-reset
```

退出码约定（`scripts/README.md` AH 节）：

- `espview_build.bat`：`0` 全过 / `1` 构建/测试失败 / `2` 参数错误 / `3` preflight 失败。
- `espview_flash.bat`：`0` 通过（含 dry-run）/ `2` 参数 / `3` ESP-IDF 不可用 / `4` COM 口不存在
  / `5` 固件 bin 不存在 / `6` 烧录失败。
- `--no-reset`：ESP-IDF v6.0.2 的 `idf.py flash` 无原生 `--no-reset`，脚本直接调 esptool
  （`--before default-reset --after no-reset write-flash @flash_args`，注意 esptool v5.3.1
  要求 `--after` 位于 `write-flash` 之前）。
## 4. 测试分层

| 层 | 入口 | 依赖 | 覆盖 |
| --- | --- | --- | --- |
| Host | `scripts\verify_host.bat` / `ctest` | MSYS2 MinGW64 | `shared/protocol/tests` 全套（CRC/编码/解码/帧组装/端点/流水线/输入/显示/传输/OLED/ScanTransaction，20 万+ checks）、`com3_frame_test --selftest-queue`、`tcp_transport_test`（127.0.0.1 loopback）、`transport_config_test` |
| Qt | `scripts\verify_qt.bat` | MSYS2 MinGW64 + Qt 6 | 构建 `espview_virtual_display.exe`（Qt Widgets + SerialPort）；GUI 手动运行 |
| ESP32 | `scripts\verify_lvgl.bat` | MSYS2 + ESP-IDF v6.0.2 | [1/3] host 协议+显示+wifi 单测 → [2/3] `idf.py build` → [3/3] 可选 `ESPVIEW_COM3` 硬件 sanity（`pc_com3_lvgl_sanity.py`） |
| 硬件 | 见 §4.4 | 真实 ESP32 + COM + Wi-Fi + OLED | 协议/UART/TCP/OLED 真机验收（记录在 DESIGN.md） |

### 4.1 Host（ctest / verify_host.bat）

- 单测构建在 `build\verify_host\protocol`，运行
  `ctest --test-dir build\verify_host\protocol --output-on-failure`。
- 三个独立可执行 host 工具：`com3_frame_test.exe --selftest-queue`（ByteQueue 自检）、
  `tcp_transport_test.exe`（TCP loopback，无真实 Wi-Fi）、`transport_config_test.exe`（配置校验）。
- 其他独立 host 目标（按需手动构建/运行）：`i18n_test`、`input_mapper_test`（Qt 依赖，仅
  `ESPVIEW_BUILD_QT_GUI=ON` 时构建）、`shared/oled` 的 `oled_preview_test`、`shared/wifi` 的
  `scan_transaction_test`（已并入 ctest 套件）。

### 4.2 Qt（verify_qt.bat）

只做构建检查，不连硬件。GUI 真机验证按 §3.2 手动运行（UART 或 TCP），验收结果记录在
DESIGN.md（AB.12 / AC.13 等）。

### 4.3 ESP32（verify_lvgl.bat）

```bat
scripts\verify_lvgl.bat                          :: host 测试 + ESP32 构建
set ESPVIEW_COM3=COM4 && scripts\verify_lvgl.bat :: 加跑 COM4 LVGL sanity（需先烧录固件）
```

### 4.4 硬件验收

**COM（UART 链路，115200 8N1 baseline）**

```bat
:: 真实 COM 帧管线全模式验收（C++，无需 Qt）
build\verify_host\pc\com3_frame_test.exe --mode full-small
build\verify_host\pc\com3_frame_test.exe --mode full-large
build\verify_host\pc\com3_frame_test.exe --mode partial
build\verify_host\pc\com3_frame_test.exe --mode corruption
build\verify_host\pc\com3_frame_test.exe --mode seq-gap
build\verify_host\pc\com3_frame_test.exe --mode reconnect
:: Python 探针（需 ESP-IDF venv python / pyserial）
py -3.10 scripts\pc_com3_test.py --port COM4 --baud 115200          :: M1-1 UART 字节流
py -3.10 scripts\pc_com3_session_test.py --port COM4 --baud 115200  :: M1-2 会话（HELLO/PING/SET_MODE）
py -3.10 scripts\pc_com3_lvgl_sanity.py --port COM4 --baud 115200   :: M5-A LVGL 帧流 sanity
```

**Wi-Fi / TCP**

```bat
:: host loopback（无硬件，普通回归）
build\verify_host\pc\tcp_transport_test.exe
:: 真实 UART Wi-Fi 扫描探针（PC → ESP32 → 空口 SCAN_RESULT；不收任何凭据）
cmake --build build\verify_host\pc --target wifi_provision_probe -j 8
build\verify_host\pc\wifi_provision_probe.exe --port COM4 --baud 115200
:: M7-E A/B/C 硬件实验（OLED active/suspended/disabled × Wi-Fi 扫描；默认不构建不烧录）
py -3.10 scripts\espview_e_ab_harness.py --modes A,B --iterations 3 --strict --result-file %TEMP%\abc.txt
py -3.10 scripts\espview_e_ab_harness.py --build --flash --dry-run
```

TCP 模式实机验收：PC 端跑 `espview_virtual_display.exe --transport tcp --tcp-bind 0.0.0.0
--tcp-port 8765`，ESP32 固件用 TCP profile 自动连接；断线重连 → HELLO → FULL resync。
历史实测数据在 DESIGN.md（T.10 / U / V / X / AJ.7b）。

**OLED**

```bat
:: M7-B pc_oled_monitor.py（OLED 状态行观测；M7-A/B 验收用）
py -3.10 scripts\pc_oled_monitor.py --port COM4 --baud 115200 --duration 1800
```

OLED 硬件接线与验收记录见 DESIGN.md（Y / Z / AA / AJ 节）：SSD1306 128×64，I2C SDA=GPIO21
SCL=GPIO22，probe 地址 0x3C。

**验收纪律**：硬件测试均为 manual（不进入普通 ctest）；结果必须在 DESIGN.md 对应章节记录
（含证据文件目录，如 `build/m6a_png/`、`build/m6d_png/`、`build/m6e_png/`）。

### 4.5 GitHub Actions CI（自动 gate）

仓库已接入 GitHub Actions CI，分四层：`fast-ci`（ubuntu + Windows MSYS2 host 测试）、
`windows-ci`（Qt 6 构建 + offscreen 自关闭冒烟）、`esp32-ci`（`espressif/idf:v6.0.2` 容器内
profile 构建，绝不 flash）、`docs-security`（check_docs / security_scan / check_bat_crlf /
YAML lint，始终运行）；另有 tag `v*` 触发的 `release.yml` 与手动 self-hosted 的
`hardware-smoke.yml`。**分层模型、触发矩阵、PR gate、产物与凭据策略、本地模拟与故障排查
全部见 [docs/ci.md](ci.md)**。本节只强调三点：

- **构建/验证入口不变**：CI 只是自动兜底，本地仍跑 §3 / §4 的命令（`verify_host.bat` /
  `verify_qt.bat` / `verify_lvgl.bat`）；CI 不做本地没有的事（除容器构建外）。
- **CI passed ≠ hardware passed**：`esp32-ci` 只 build 不 flash、不要 COM、不要真 Wi-Fi；
  真机验收仍是 §4.4 的 manual gate（RF-ON / CH340 阻塞见 DESIGN.md AL.3，禁止宣称已真机验证）。
- **host-only PR 不需要 ESP32 / CH340 / 真 Wi-Fi**：无硬件也能提 PR；CI 红轮先按
  docs/ci.md §10 区分代码失败与环境失败。

## 5. Profile 系统（固件构建目录）

固件行为由 ESP32 Kconfig 决定；**profile = `esp32\build\<name>` 构建目录**，用 `-b <name>`
选择（`espview_build.bat` / `espview_flash.bat` / `espview_build_flash.bat` 通用），构建与
烧录都使用 `build\<profile>\sdkconfig`（M7-F F4 隔离，见 §3.3）。

命名约定（任务书 §19）：`uart`（UART 验收）、`tcp`（TCP / 生产）、`oled` / `oled-off`
（OLED 开 / 关对照，M7-E A/B/C 实验所用配置差异）、`diagnostic`（诊断钩子）。
**以 `scripts/README.md` 与 `git HEAD` 为准**——脚本默认值、别名与 profile 表可能随里程碑
演进（当前默认 `uart_hw` 别名 → `uart`，UART 验收固件）；M7-G 硬件实验另设 `g1_a`–`g1_d`（G1
A/B/C/D 矩阵，见 DESIGN.md AL.3）。新 profile = menuconfig 配置后另建构建目录即可，脚本无需改动。

## 6. 开发工作流与提交纪律

- **章节式 commit**：每个里程碑/章节（如 M7-D、M7-E、M7-F）独立提交，前缀 `feat:` /
  `docs:` / `test:` / `fix:` + 可选 `(scope)`，如 `feat(protocol): M7-D3 wifi provisioning`、
  `fix(scripts): M7-F F4 build/flash UX`、`docs: M7-F F5 hardware evidence matrix`。每章完成后
  **独立 commit + push + clean**（提交后 `git status` 干净，再进入下一章）。
- **并行代理边界**：本仓库由多个代理并行推进；每个代理只修改自己独占的文件清单，其他文件
  只读。文档代理只写 `docs/development.md`、`docs/contributing.md`；脚本/README 由脚本代理
  维护——改动前先读最新版。
- **改动前先读**：协议/架构改动前必须先读 DESIGN.md 对应章节与 `scripts/README.md`；硬件
  改动前确认 DESIGN.md 中既有验收记录（避免重复实验）。
- 提交信息示例与分支约定详见 [docs/contributing.md](contributing.md)。

## 7. DESIGN.md 权威与 wire format 冻结

- `docs/DESIGN.md` 是协议、架构与验收记录的**唯一权威**。Packet Header、CRC32、HELLO、
  SET_MODE、Frame 语义、消息表（v0.1）等已冻结（E 节）；后续里程碑全部遵循
  「wire format 未修改 / additive」原则（AD–AK 各节）。
- **禁止在未先更新 DESIGN.md 的情况下修改 wire format 或已冻结语义**。新增消息/字段必须是
  additive 扩展，并先在 DESIGN.md 定稿（如 M7-D1 CAPABILITIES、M7-D2 PHYSICAL_PREVIEW、
  M7-D3 Wi-Fi provisioning、M7-E OLED suspend/resume 的 AJ 节）。
- 里程碑式文档修订沿「修订记录（时间戳）」块追加，不重写历史章节；实测数据/结论要标注
  日期、硬件配置与证据文件。

## 8. 已知环境坑（Windows / PowerShell）

1. **PowerShell 5.1 `@args` 拆参**：`powershell.exe -Command` 里传 `idf.py -DSDKCONFIG=...`
   （无空格）会被 PS 5.1 拆坏，**必须用空格形式** `-D SDKCONFIG=...`。仓库脚本
   （`espview_build.bat` / `espview_flash.bat`）已按空格形式书写；手动敲命令时同样注意。
2. **.bat 必须 CRLF**：`cmd.exe` 的 `goto`/标签在 LF 行尾下会解析失败、行为诡异。仓库
   `.gitattributes` 是 `* text=auto`（LF 入库、Windows 检出 CRLF）；编辑 `.bat` 时不要引入
   LF-only（`.md` 文件则用 LF，见贡献规范）。
3. **ESP32 构建串行**：`idf.py` 构建较慢且 `esp32/build/` 目录共享，并行代理同时构建会互相
   冲突；同一时间只跑一个 ESP32 构建/烧录（`espview_e_ab_harness.py` 默认不构建/不烧录即为
   此设计）。`--check` 只做 preflight，适合先探测环境。
4. **MSYS2 PATH**：cmake/ctest/g++ 必须来自 MSYS2 MinGW64——把 `C:\msys64\mingw64\bin`
   前置到 `PATH`，否则可能命中 ESP-IDF 自带工具或 Visual Studio 编译器导致 CMake 生成失败。
   `verify_*.bat` / `espview_build.bat` 会自动处理；手动构建时用
   `set PATH=C:\msys64\mingw64\bin;%PATH%`。
5. **Python 环境**：硬件 Python 脚本需要 `pyserial`，默认用 ESP-IDF venv python
   （`C:\Espressif\tools\python\v6.0.2\venv\Scripts\python.exe`）或 `py -3.10`；系统 python
   通常没有 pyserial（`py -3.10 -m pip install pyserial`）。
6. **esptool 参数顺序**（`--no-reset` 路径）：esptool v5.3.1 要求 `--after no-reset` 位于
   `write-flash` 之前（脚本已处理，见 `scripts/espview_flash.bat`）。

## 9. 凭据纪律（速览）

- Wi-Fi SSID / 密码**只存在于本机未跟踪的 `esp32/sdkconfig`**（`.gitignore` 已排除
  `esp32/sdkconfig*`，保留 `sdkconfig.defaults`）；源码、文档、日志、QSettings、PNG dump
  一律不含密码。
- 默认 **RAM-only**（断电即失，DESIGN.md AF.4）；PC 侧凭据只经对话框输入（禁 CLI 参数），
  ACK 后 secureErase，不持久化；日志只打印 SSID 长度。
- 脚本从不读取/打印 `esp32\sdkconfig`。完整纪律见 [docs/contributing.md](contributing.md)
  §7 与 DESIGN.md（T.7 / X.8 / AF.4 / AG.3）。
