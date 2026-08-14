# ESPView — ESP32 Virtual Display & Input Bridge

> 让 PC 成为真实 ESP32 的“远程显示屏 + 远程输入设备”。ESP32 始终是显示状态与输入分发的**唯一权威**：
> ESP32 上的 LVGL UI 通过自研二进制协议（Packet / Message / Frame 三层）经 UART 或 Wi-Fi TCP 实时推送到
> PC 端的 Qt Virtual Display；PC 捕获的鼠标 / 键盘事件沿反向链路发回 ESP32 驱动 LVGL。
>
> 协议核心已冻结并通过 20 万+ host checks 与真实硬件验收（见 [docs/DESIGN.md](docs/DESIGN.md)）。

## 特性

- **零协议重设计、纯 C++17、平台无关**：`shared/` 协议与传输抽象可同时编译到 ESP32（ESP-IDF v6.0.2）与 PC（MSYS2 MinGW64 / Qt 6），不依赖第三方序列化库。
- **dirty-rect 显示链路**：LVGL flush callback → `RemoteDisplay::writeRect()` → `FRAME_BEGIN/RECT/END` → Transport，PC 端 `FrameAssembler` 提交只读镜像；**ESPView 不持有第二份 framebuffer**，PC 永远只保存“最后一份完整提交帧”。
- **双传输**：UART（115200 8N1，正式 baseline）与 Wi-Fi STA + TCP（PC = Server `0.0.0.0:8765`，ESP32 = Client），运行时可在 Transport UI 安全切换（会话重置 → HELLO → FULL resync）。
- **可靠性**：20-byte Packet Header + IEEE CRC-32（参数全钉死）、Message-level CHUNKED 拆包、SEQ/frameId 分离、UART 重同步状态机、断线重连后强制 FULL resync、有界 TX 队列 + 整帧丢弃、ACK 只服务控制消息。
- **输入反向链路**：Qt 键盘/鼠标 → `INPUT_KEY / INPUT_MOUSE` → ESP32 `InputManager` → LVGL Input Adapter（HID keymap + 坐标映射）。
- **可观测性**：ESP32 每 3s 经 ERROR 文本通道上报 `trx`（RSSI/channel/reconnect/tx/rx）、heap、会话错误计数（decoder/CRC/seqGap）等诊断行。

## 系统架构

```
ESP32 (LVGL demo UI)
  └─ RemoteDisplay (writeRect / dirty-rect 汇入)
       └─ ProtocolEndpoint (MessageEncoder / StreamDecoder / FrameAssembler)
            └─ TransportManager
                 ├─ UART (UART0 / CH340)
                 └─ Wi-Fi STA + TCP client
                          │  协议字节流 (Packet/Message/Frame + CRC32)
PC (Qt VirtualScreen)
  ├─ TCP Server 0.0.0.0:8765 (或 UART COM 口)
  ├─ SerialWorker: StreamDecoder → FrameAssembler → DisplayFrame (Qt::QueuedConnection)
  ├─ VirtualScreenWidget: FULL 重建 / PARTIAL 只写目标 RECT（RGB565 LE → RGB888）
  └─ InputController: INPUT_KEY / INPUT_MOUSE → ESP32 InputManager → LVGL
```

## 硬件要求

- 经典 ESP32（本项目验收板为 **ESP32-D0WDQ6**，rev v1.1）+ 板载 **CH340** USB 转串口
- 4 MiB flash（single factory app = 2 MiB，无 OTA 分区）
- Wi-Fi 2.4GHz 局域网（TCP 模式）或 USB 串口线（UART 模式）
- PC：Windows + MSYS2 MinGW64 + Qt 6（GUI 目标）；ESP-IDF v6.0.2（固件目标）

## 快速开始

### 1. ESP32 固件（TCP 模式示例）

```powershell
# 1) 载入 ESP-IDF v6.0.2 环境（你的 profile 路径可能不同）
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
cd esp32

# 2) 配置 Wi-Fi 凭据与 PC 服务器地址（写入本机未跟踪 sdkconfig，绝不提交到 git）
#    idf.py menuconfig
#    → ESPView → Protocol transport → Wi-Fi STA + TCP
#    → ESPView → Wi-Fi SSID / Wi-Fi password / PC TCP server IPv4 (port 8765)

# 3) 编译并烧录（示例 COM4）
idf.py -p COM4 flash monitor
```

> 默认 profile（`sdkconfig.defaults`）为 UART + LVGL demo 应用、console-off；`CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH=n`
> 为生产默认，F12 切换钩子仅在测试固件（本地 `sdkconfig`）中启用。生产固件单 Transport 部署，UI 不依赖 F12。

### 2. PC Qt Virtual Display

```powershell
# 构建（需 MSYS2 MinGW64，g++ 15/16 + Qt 6）
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
scripts\verify_qt.bat        # 产出 build\verify_qt\espview_virtual_display.exe

# 运行（TCP 模式：PC 是 Server）
build\verify_qt\espview_virtual_display.exe --transport tcp --tcp-bind 0.0.0.0 --tcp-port 8765

# 或 UART 模式
build\verify_qt\espview_virtual_display.exe --transport uart --port COM4 --baud 115200
```

GUI 参数：`--transport uart|tcp`、`--port <COM>`、`--baud`、`--tcp-bind`、`--tcp-port`、
`--dump-png <dir>`（每个新 FULL commit 保存 `full_<frameId>.png`）、`--diag-log <file>`、
`--autoclose-ms N`、`--no-reset`。更多见 `espview_virtual_display.exe --help`。

## 测试截图（真实硬件验收）

以下画面由 ESP32 LVGL demo（背景 + 按钮 + 1Hz 计数器）经真实链路渲染到 PC Virtual Display，
`--dump-png` 在每次 FULL commit 时落盘（320×240 RGB565，letterbox 缩放显示）。

| 场景 | 截图 |
| --- | --- |
| 硬件 sanity（首帧 FULL commit，frame id=1） | ![sanity](docs/images/demo_hello_full.png) |
| 30 分钟 TCP 长稳（frame id=63，单会话无重连抖动） | ![longrun](docs/images/demo_longrun_full.png) |
| TCP reconnect stress 第 1 轮（10/10 OK 重连后 FULL resync） | ![reconnect](docs/images/tcp_reconnect_full.png) |

## 验证与测试

| 入口 | 内容 | 最近实测 |
| --- | --- | --- |
| `scripts\verify_host.bat` | 协议 host 套件 + ctest + `transport_config_test` + `com3_frame_test --selftest-queue` + `tcp_transport_test`（127.0.0.1 loopback，无需硬件） | 协议 **208,951 checks / 0 failures**；config **67/0**；TCP **126/0**；ctest 1/1 Passed |
| `scripts\verify_qt.bat` | Qt GUI 目标编译检查（`espview_virtual_display.exe`） | ALL PASS |
| `scripts\verify_lvgl.bat` | host 测试 + ctest + ESP32（`idf.py build`） | ALL PASS |
| 硬件 manual | 30 min TCP 长稳、TCP reconnect stress 10/10、UART↔TCP 切换 20×20、PARTIAL/输入链路过（见 DESIGN.md X/W 节） | heap 平坦 `hb/ha=231352`，会话错误全程 0 |

> 30 分钟长稳属于 manual verification，不加入普通 CI（ctest 保持快速、可重复、离线）。

## 协议摘要（详见 [docs/DESIGN.md](docs/DESIGN.md)）

- **Packet**：20-byte 头（MAGIC/VERSION/TYPE/FLAGS/RSVD/SEQ LE/LENGTH LE/CRC32 LE/RSVD2）+ payload ≤ 4096 B；
  CRC32（IEEE/zlib：poly `0xEDB88320`，init/final `0xFFFFFFFF`，reflected，LE）覆盖 `header[0..14)+payload`，不覆盖 CRC32 字段与 RSVD2。
- **Message**：1..N Packet；小消息 1 包（CHUNKED=0）；大消息（如 FRAME_RECT）按 4096 B 拆分，CHUNKED=1 表示“仍有后续”，末包清除；每包 SEQ 递增、TYPE 一致。
- **Frame**：`FRAME_BEGIN`（宽高/像素格式/帧号）→ 1..N `FRAME_RECT`（dirty rect + 像素）→ `FRAME_END`（提交）。PARTIAL 只允许应用到最近一次成功提交的 frame；无 committed frame 时 PARTIAL 不得提交，接收端继续等 FULL。
- **传输层**：UART（115200 baseline）与 TCP 均视为可靠有序字节流；`ITransport` 只上报自身状态，不理解协议；断线/重连/切换后一律会话重置 + FULL resync。

## 目录结构

```
shared/            # 平台无关核心：protocol（Packet/Message/Encoder/Decoder/FrameAssembler/
                   #   ProtocolEndpoint/RuntimeStats）、transport（TransportManager/TransportSink）、
                   #   display（RemoteDisplay）、input（InputManager/keymap/坐标映射）
pc/                # PC 侧：Qt GUI（espview_virtual_display）、HostTcpTransport/SerialTransport、
                   #   host 测试（com3_frame_test / tcp_transport_test / transport_config_test）
esp32/             # ESP-IDF v6.0.2：main + components（espview 传输、lvgl_port、display、input、
                   #   testpattern）、Kconfig、sdkconfig.defaults（生产默认，无凭据）、partitions.csv
scripts/           # verify_host / verify_qt / verify_lvgl / 硬件验收 python 脚本
docs/DESIGN.md     # 完整设计文档（协议冻结规范 + 里程碑实测记录）
```

## 当前状态

- M0（协议核心）→ M1-1（UART）→ M1-2（固件/Flash）→ M1-3A–D（协议收尾/Streaming/文档/测试 CI）→
  M2（Qt Virtual Display）→ M3（输入链路）→ M4（帧管线）→ M5-A/B（LVGL 后端/输入适配）→
  M6-A–E（Wi-Fi/TCP、性能、运行时切换 UI、配置持久化、生产 profile 与长稳）✅
- Flash footprint：`espview_esp32.bin ≈ 0x1037F0`（≈1.01 MiB），2 MiB app 分区剩余 ≈49%。

## 已知边界 / 暂不实现

- 不接物理 LCD / 触摸；`HardwareDisplay` / `MirrorDisplay` 实机未验证（架构已预留三种 DisplayMode）。
- TinyUSB、OTA（分区未改）、UDP、mDNS、WebSocket、HTTP、TLS、云服务、多设备并发均不在范围内。
- 921600 UART 为 experimental-only（大帧突发不可靠），正式 baseline 为 115200 8N1。
- AP 断电（router 掉电）场景 deferred；当前已验证 TCP 断开/退避重连/PC 重启/FULL resync。

## 安全说明

- **Wi-Fi 凭据只存在于本机未跟踪的 `esp32/sdkconfig`**（已被 `.gitignore` 排除）。源码、
  `sdkconfig.defaults`、文档、日志一律不含 SSID/密码；日志只打印 SSID 长度与错误码。
- 提交前请确认本地 `git status` 中**不包含** `esp32/sdkconfig*`（`sdkconfig.defaults` 除外）。

## 开发环境

- Windows + MSYS2 MinGW64（g++ 15/16）、CMake、ctest
- Qt 6.11.1（MSYS2 MinGW64，仅 PC GUI 目标）
- ESP-IDF v6.0.2（PowerShell profile 加载）
