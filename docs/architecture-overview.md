# 高层架构（Architecture Overview）

> 只描述**已实现**的部分（M0..M8-A7 冻结）。目录结构与设计依据：
> [docs/DESIGN.md](DESIGN.md)（E/C/D/K/L 节 + 里程碑 M 节）。

## 1. 系统图

```
ESP32（LVGL demo UI）
  └─ RemoteDisplay / DisplayRouter（writeRect / dirty-rect 汇入）
       └─ ProtocolEndpoint（MessageEncoder / StreamDecoder / FrameAssembler）
            └─ TransportManager
                 ├─ UART（UART0 / CH340）
                 └─ Wi-Fi STA + TCP client（PC = TCP Server 0.0.0.0:8765）
                          │  Packet/Message/Frame + CRC32
PC（Qt VirtualScreen）
  ├─ TCP Server（或 UART COM 口）
  ├─ SerialWorker：解码 → FrameAssembler → DisplayFrame → GUI
  ├─ VirtualScreenWidget：FULL 重建 / PARTIAL 只写 RECT（RGB565 LE → RGB888）
  ├─ DisplayRouter UI / Split Drawer / Physical Preview
  └─ InputController：INPUT_KEY / INPUT_MOUSE → ESP32 InputManager → LVGL
```

## 2. 代码布局

| 目录 | 内容（已实现） |
| --- | --- |
| `shared/protocol/` | Packet / Message / Frame 协议核心：`packet`、`encoder`、`decoder`、`message`、`frame_assembler`、`protocol_endpoint`、`runtime_stats`、`crc32` + host 单测 |
| `shared/transport/` | `transport.h`（ITransport）、`transport_manager`、`transport_sink` + host 测试 |
| `shared/display/` | `display.h` / `display_manager` / `remote_display` / `display_router` / `display_sink.h`（IDisplaySink）/ `display_capabilities` / `physical_capability_snapshot` / `physical_status` / `display_ui_state` / `split_state` + host 测试 |
| `shared/input/` | `input_event` / `input_codec` / `input_manager` / `coordinate_mapper` / `keyboard_mapper` / `hid_lvgl_keymap` / `lvgl_adapter` + host 测试 |
| `shared/oled/` | `oled_fb`（1KB 页式 fb + 8×8 字体）/ `oled_cmd` / `oled_status` / `oled_preview` / `physical_renderer` / `oled_recovery` + host 测试 |
| `shared/wifi/` | `scan_transaction`（M7-E 事务状态机）+ host 测试 |
| `pc/src/` | Qt GUI（`main.cpp`、`virtual_screen_widget`、`display_mode_widget`、`display_status_panel`、`split_drawer`、`physical_preview_widget`）、`connection_manager`、`serial_worker`、`serial_transport`、`host_tcp_transport`、`transport_config`、`wifi_wizard_dialog` + `wifi_wizard_state`、`i18n`、`input/`（`input_controller`、`qt_key_adapter`）、非 Qt 工具（`com3_frame_test`、`input_send_test`、`wifi_provision_probe`、`win32_com_probe`、`tcp_transport_test` 等） |
| `esp32/components/` | `espview`（Kconfig + `uart_transport` / `tcp_transport` / `transport_manager` / `wifi_sta` / `wifi_provisioning`）、`oled`（`oled_i2c` / `oled_controller` / `oled_display` / `physical_display_sink`）、`lvgl_port`（`lvgl_port` / `lvgl_app` / `lvgl_indev`）、`display`、`input`、`protocol`、`testpattern` |
| `esp32/main/` | 应用选择（LVGL 默认 / TestPattern）、Transport 装配、模式路由 `sceneOf(mode)`、诊断行、TEST hooks |
| `scripts/` | 构建/烧录/验收脚本（见 `scripts\README.md`） |

## 3. 协议三层：Packet / Message / Frame

- **Packet**：线路最小单位 = 固定 20B 头（MAGIC 'E''S''P''V' / VERSION 0x01 / TYPE /
  FLAGS / RSVD / SEQ LE / LENGTH LE / CRC32 LE / RSVD2）+ 载荷 ≤ **4096 B**
  （`MAX_PACKET_PAYLOAD`）。CRC32 = IEEE/zlib（poly 0xEDB88320、init/final
  0xFFFFFFFF、reflected、LE），覆盖 header[0..14)+payload，不覆盖 CRC 字段与 RSVD2。
- **Message**：一条逻辑消息（HELLO / FRAME_RECT / INPUT_MOUSE…）= 1..N 个 Packet；
  载荷 >4096B 时 CHUNKED 拆分（前 n-1 包 CHUNKED=1，末包 0，SEQ 连续、TYPE 一致）；
  拼接上限 `MAX_MESSAGE_PAYLOAD` = 1 MiB（协议级上限，不要求任何组件分配 1MiB 缓冲）。
- **Frame**：一次显示事务 = `FRAME_BEGIN` + 0..n 个 `FRAME_RECT` + `FRAME_END`；
  整帧提交/整帧丢弃；PARTIAL 只允许叠加到最近一次成功提交的帧；无 committed 帧时
  等 FULL resync。
- 控制消息（HELLO/SET_MODE/INPUT_*/PING/PONG/ERROR/ACK）不属于帧，可穿插但不得
  插入同一 CHUNKED Message 的连续 Packet 之间。

消息表（真实 TYPE 值，`shared\protocol\protocol.h`）：

| TYPE | 消息 | 方向 |
| --- | --- | --- |
| 0x01 | HELLO | ESP→PC |
| 0x02 | CAPABILITIES | 双向（可选） |
| 0x03 | SET_MODE | PC→ESP（ACK_REQ） |
| 0x06 / 0x07 | WIFI_SCAN_REQ / WIFI_SCAN_RESULT | PC→ESP / ESP→PC |
| 0x08 / 0x09 | WIFI_CONFIG / WIFI_STATUS | PC→ESP（ACK_REQ）/ ESP→PC |
| 0x10 / 0x11 / 0x12 | FRAME_BEGIN / FRAME_RECT / FRAME_END | ESP→PC |
| 0x13 | PHYSICAL_PREVIEW | ESP→PC |
| 0x20 / 0x21 | INPUT_KEY / INPUT_MOUSE | PC→ESP |
| 0x30 / 0x31 | PING / PONG | 双向 |
| 0x50 | ERROR | 双向 |

## 4. Transport 抽象

- `ITransport`（`shared/transport/transport.h`）：可靠有序字节流抽象，只上报自身
  状态、不理解协议；ESP32 与 PC 各有实现但接口同构。
- ESP32：`UartTransport`（UART0 @ 115200）与 `TcpTransport`（STA client）；
  `TransportManager` 支持运行时安全切换（会话重置 → HELLO → FULL resync）。
- PC：`SerialTransport`（UART）与 `HostTcpTransport`（server + client，WinSock2）；
  `SerialWorker` 独立线程持有 transport + ProtocolEndpoint，经 Qt::QueuedConnection
  投递 `DisplayFrame`。
- 断线/重连/切换一律 FULL resync；TCP 大帧实测 235–254ms（默认 power save）。

## 5. 显示架构：DisplayRouter 与模式

- `RemoteDisplay`：LVGL flush_cb 的汇入点（writeRect + dirty-rect，无整屏缓存）。
- `IDisplaySink`：init / capabilities / present(rect,px) / flush / setEnabled /
  isAvailable / status。`PhysicalDisplaySink`（OLED）与 `VirtualSink`（PC）都是 sink。
- `DisplayRouter`：按模式选择 sink 目标集；任一成功 → kOk 聚合；physical 不可用 →
  kDegraded（Virtual 不受影响）。
- 四种模式（M7-C1 冻结）：`0=VirtualOnly` / `1=PhysicalOnly` / `2=Mirror` /
  `3=Split`（wire additive kSplit=3）；语义见 [docs/display-modes.md](display-modes.md)。
  早期设计（F 节）只有三模式 + 编译期切换；M7-C 之后四模式运行时可用。

## 6. 输入架构

- PC：`InputController`（Qt 事件 → INPUT_*，MouseMove ≤60Hz 节流合并，autoRepeat
  忽略）→ Worker TX 队列。
- ESP32：`InputManager` → `LvglInputAdapter`（纯 C++17）→ `lvgl_indev`（POINTER /
  KEYPAD / ENCODER read_cb）→ LVGL。详见 [docs/input.md](input.md)。

## 7. 里程碑概览（只列已实现）

| 里程碑 | 内容 |
| --- | --- |
| M0 | shared/protocol 核心 + host 单测 |
| M1-1..D | UART 传输 / 会话层 / 帧错误恢复 / Streaming Message API / 文档冻结 |
| M2 | PC Qt 6 Virtual Display |
| M3 | 输入回传（INPUT_KEY / INPUT_MOUSE） |
| M4 | 运行态统计层（RuntimeStats / DiagnosticsRing） |
| M5-A/B | LVGL Display Backend + LVGL Input Device |
| M6-A..E | Wi-Fi/TCP Transport、flash 分区、运行时传输选择、配置/持久化硬化、生产 profile |
| M7-A/B | OLED 诊断显示 + 生产语义收尾 |
| M7-C1..C4 | DisplayRouter / 物理显示后端 / Qt 多显示 UI + Split Drawer + 双语 |
| M7-D1..D6 | CAPABILITIES / PHYSICAL_PREVIEW / Wi-Fi provisioning 协议 / Wizard UX / Build-Flash UX / UART 真实验收 |
| M7-E | Power-aware provisioning（OLED suspend + ScanTransaction + A/B/C harness） |
| M7-F | 硬件证据矩阵（F1–F4）+ provisioning 生产化硬化 |

逐条提交历史见 [docs/changelog.md](changelog.md)。

## 7. 依赖规则（什么不能依赖什么）

| 模块 | 不得依赖 | 现状（M8-A7 核查） |
| --- | --- | --- |
| `shared/protocol` | Qt、ESP-IDF、具体 Transport 实现 | ✅ 纯 C++17；仅包含 `transport.h` **接口头**（M8-A3 明确豁免，见 DESIGN §35.2） |
| `shared/display` | Qt、LVGL、ESP-IDF | ✅ 纯 C++17（依赖 protocol/transport/oled 接口头，方向允许） |
| `shared/input`（InputManager 等） | Qt、LVGL | ✅ 纯 C++17；LVGL 接线只在 `esp32/components/lvgl_port/src/lvgl_indev.cpp` |
| `shared/transport` | 协议实现 | ✅ 独立；TransportManager 无 UART/TCP 细节 |
| `shared/oled`、`shared/wifi` | 平台 | ✅ 纯 C++17 + host 测试 |
| `pc/` | ESP-IDF | ✅ Qt 只依赖 shared 头 |
| `esp32/` | Qt | ✅ IDF 组件复用 shared 源码 |

**S3 边界（M8-A7）**：`ITransport` 已预留 `TransportType::kUsb=2`（CDC 语义 paced=true），
未来 UsbTransport 只需实现 ITransport 并接入 TransportManager，不改 shared 核心（DESIGN
AQ.16）。当前 Classic ESP32 无 native USB；S3 的 USB CDC / LCD / touch 均为 planned、
not implemented（红线，DESIGN §四十一 对应章节）。

