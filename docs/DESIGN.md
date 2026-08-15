# ESPView — 第一阶段设计文档 (v0.1)

> ESP32 Virtual Display & Input Bridge
> 目标：让 PC 成为真实 ESP32 的“远程显示屏 + 远程输入设备”，ESP32 始终是显示状态的唯一权威。

> **修订记录（Pre-M0 Review, 2026-08-12）**：ESPView 不持有长期完整 framebuffer（所有权归 Application/LVGL）；RemoteDisplay 采用 writeRect()/dirty-rect 汇入模型；协议改为 Packet/Message/Frame 三层并新增 FRAME_BEGIN、删除独立 FRAME_FULL；MAX_PACKET_PAYLOAD=4096；CRC32 全参数钉死；UART 重同步状态机与重连整帧重同步；ACK 只服务控制消息；TX 背压整帧丢弃；ITransport 增加状态回调；v0.1 只实现编译期 DisplayMode。详见 E/F/H/J 节。
> **修订记录（M1-3C, 2026-08-13）**：新增 Streaming Message API（`IMessagePayloadSource` + `MessageEncoder::encodeStreaming`）——payload 不要求驻留内存，逐 4096B Packet 编码，与完整 payload 编码逐位等价；正式硬件 baseline 定为 115200（921600 = experimental / unreliable for large burst）；经典 ESP32 可流式发送 153608B 级 FRAME_RECT，避免 153KB 连续堆分配。wire format 不变。
> **修订记录（M1-3D, 2026-08-13）**：文档同步 + 测试工具 CI 收尾 + M2 前架构冻结检查。正式写入 115200 硬件 baseline 与 921600 experimental 的准确表述；带宽表改用真实测量（153600B 整帧 ≈ 13.5 s，有效 payload ≈ 11.1 KB/s）；E 节新增 Streaming Message API 正式设计；里程碑更新至 M1-3C 完成；P 节移除已过时建议；新增 host-only 验证入口 `scripts/verify_host.bat`（含 `com3_frame_test --selftest-queue`，无需 COM3/ESP32）。协议 wire format 与 M0–M1-3C 全部不变。
> **修订记录（M2, 2026-08-13）**：PC Qt 6 Virtual Display 完成。`pc/` 新增 Qt 6.11.1 (MSYS2 MinGW64) 目标 `espview_virtual_display`：SerialWorker（独立线程）持有 HostUartTransport + ProtocolEndpoint（StreamDecoder + FrameAssembler 在内），以 DisplayFrame（frameId/frameType/width/height/pixelFormat/rectCount/byteCount/rects）经 Qt::QueuedConnection 投递 GUI；VirtualScreenWidget 仅消费 DisplayFrame（RGB565 LE → QImage::Format_RGB888 显式转换；FULL 重建 QImage，PARTIAL 只写目标 RECT，letterbox 等比缩放，断线清空/重连等新 FULL）。被动 HELLO + 7s 主动重试 + 断线 1.5s 自动重连（复位 ESP32 重同步）。真实硬件验收：COM3 @ 115200，HELLO / FULL small / FULL large(153600B) / PARTIAL / disconnect / reconnect / FULL resync 全部通过，PNG 像素逐点校验与 TestPattern 公式一致。新增 `scripts/verify_qt.bat`（Qt 工程 host build）与 `scripts/verify_png_pixels.ps1`（像素校验）。协议 wire format 与 M0–M1-3D 全部不变。
> **修订记录（M6-C, 2026-08-14）**：Runtime Transport Selection + Transport-aware TX Pacing。
> TransportManager（shared/transport）统一 UART/TCP 运行时创建/切换/状态转发；TransportCapabilities
> （paced/unpaced）决定发送与 LVGL flush 预算（UART 3000ms / TCP 250ms），上层不再感知具体 Transport；
> 有界 TX 队列 + 整帧丢弃 + 控制消息单次尽力（trySink）保公平；切换 = safe switch（disconnect → 会话重置
> → 新 Transport → HELLO → FULL resync），非热插拔。实测：UART baseline ≈13.2s/FULL 无回归；TCP FULL
> 235–254ms、queue peak=2、0 drop；TCP→UART / UART→TCP 双向运行时切换 + 断线重连 + FULL resync 全部零
> 协议错误；新发现并修复 PC HostTcpTransport 对端断开未通知 Worker（pumpLoop 卡死）问题。wire format 不变。
> **修订记录（M6-E, 2026-08-14）**：Wi-Fi/TCP 工程化收尾 + 稳定性 + 配置 + 生产 profile。
> 审计结论：无 wire format 冲突、无 CRITICAL/HIGH；完成 3 个 MEDIUM 修复（HostTcpTransport
> send 致命路径 → Disconnected、attach() socket 所有权、TransportManager stale-state 清理）。
> 新增 ITransport 只读统计（reconnectCount/txBytes/rxBytes/wifiApInfo）与 ESP32 `trx` 诊断行
> （RSSI/channel）；Kconfig 生产 profile 默认禁用 F12 test hook（SWITCH=n，本地 sdkconfig 仍 =y
> 供测试固件）；配置优先级 CLI>QSettings>默认 + 持久化键白名单（无凭据）；tcp_transport_test
> +3（126 checks）、transport_config_test 67 checks、协议套件 208,951 checks 全 0 failure；
> 30 分钟 TCP 长稳 heap 平坦（hb/ha=231352）、sess2 全 0、输入/PARTIAL 正常；TCP reconnect
> stress 10/10 OK；AP outage 仍 deferred。详见 X 节。wire format 不变。
> **修订记录（M7-A, 2026-08-15）**：独立 OLED 状态显示（128×64 I2C SSD1306/SH1106，
> SDA=GPIO21 SCL=GPIO22）。OLED 组件协议无关（零依赖 protocol/display/lvgl/transport），
> 低优先级任务 1s 刷新状态页（transport/session/IP/RSSI/FRM/ERR/HEAP/UP），1KB 页式
> framebuffer 按 ≤32B 分段上传；有界错误恢复（3 轮 re-init + 指数退避 + 冷却，禁无限重置）；
> 实机探测 0x3C/SSD1306，15 分钟 298 条诊断行 err=0 ok=1，0 CRC/bad_magic，OLED 不污染
> 协议链路；host 210,504 checks / 0 failures。wire format 不变。详见 Y 节。
> **修订记录（M7-B, 2026-08-15）**：OLED 生产语义收尾（M7-A 架构审计 3 个 Mandatory 修复）。
> OLED/I2C 生命周期（M#1）：stop() 只置停止标志 + 有界等待，I2C 资源释放仅发生在任务
> 退出路径，消除“任务内 transmit 与释放竞争”UAF；Transport 快照（M#2）：statsLoop 不再
> 锁外解引用 transport 裸指针（新增 TransportManager::diagSnapshot() 值语义 +
> capabilities() 按值返回 + reconnectCount 原子化）；OledStatus 可观测性（M#3）：新增
> state 枚举（kDisabled…kStopping）与 lastFlushDurationMs；statsLoop 新增 `mem` 堆诊断行
> （h/lg/mn）；pc_oled_monitor.py 重写（HELLO 握手、主动 1s PING、ERROR 负载头剥离、
> 堆趋势 5% 泄漏判定）。实测：30 分钟 UART 长跑 heap first=160096/last=168568（+8472）、
> 0 CRC；TCP reconnect stress 10/10 OK（~2.0s/轮）；host 218,625 checks / 0 failures；
> 生产固件 1,092,448 B（app 余量 48%）。wire format 不变。详见 Z 节。

---

## A. 对需求的理解

1. **ESP32 是唯一权威**：所有 UI 状态、framebuffer、输入分发都以 ESP32 为准。PC 只负责“显示 ESP32 给出的像素”和“把输入事件发回 ESP32”，绝不建立第二份 UI 状态。
2. **业务代码与显示后端完全解耦**：Application 只面向一个统一的 `IDisplay` 抽象，不出现 `if (pc_mode)` / `if (lcd_mode)` 这类代码。显示模式由 `DisplayManager` 在运行时决定（WINDOW / DEVICE / MIRROR）。
3. **通信协议与传输层解耦**：协议是纯字节流二进制协议（Header + Payload + CRC），Transport 只是“可靠的字节管道”。第一阶段在本开发板上实际走 **UART-over-USB（CH340 虚拟串口）**，未来可平滑换到 TinyUSB（ESP32-S2/S3）或 Wi-Fi TCP。
4. **ESPView 是一个可复用的 ESP-IDF component**，不是把整个项目变成“虚拟屏幕程序”；PC 端是独立的 Qt 6 程序，不包含任何 ESP32 业务逻辑。
5. **第一阶段聚焦 MVP**：320x240 RGB565、FRAME_BEGIN/RECT/END（dirty rectangle）、PING/PONG、CRC、版本号、基本鼠标/键盘输入、连接状态显示。

### 硬件现实（重要）

当前开发板是 **ESP32-D0WDQ6（经典 ESP32，rev v1.1）**，通过板载 **CH340 在 COM3** 上以虚拟串口形式出现在 PC。

- 经典 ESP32 **没有原生 USB 外设**（USB-OTG 只在 ESP32-S2/S3 上）。
- 因此第一阶段“USB 连接”实际是 **UART-over-USB**：ESP32 UART ↔ CH340 ↔ PC COM 口。这对用户来说仍是“USB 线连 PC”，协议层无需感知。
- 原生 USB（TinyUSB CDC/ACM）属于 `UsbTransport`，是未来 S3 开发板的选项，架构中预留接口即可。

---

## B. 推荐的总体架构

```
┌───────────────────────────── PC (Qt 6, Windows) ─────────────────────────────┐
│  Qt GUI 线程                 Worker 线程（QSerialPort + 协议解码）             │
│  ┌──────────────────┐  信号/槽  ┌────────────────────────────┐              │
│  │ VirtualScreenWidget│ ───────► │ SerialTransport            │              │
│  │  · 绘制 framebuffer │         │ ProtocolDecoder            │              │
│  │  · 捕获鼠标/键盘    │ ◄─────── │ FrameAssembler             │              │
│  └──────────────────┘           └────────────────────────────┘              │
│        ▲                                 │                                   │
│        │ QImage(共享 framebuffer)        │ 输入事件队列                        │
│        │                                 ▼                                   │
│  ConnectionManager ── HELLO/CAPS/心跳/状态统计 ──► ProtocolEncoder → Transport│
└───────────────────────────────────────────────────────────────────────────────┘
                                    │  USB(COM) / 未来: TCP
┌──────────────────────────────── ESP32 ────────────────────────────────────────┐
│  Application (LVGL 或自定义 UI)                                                │
│        │ UI 调用                                                              │
│        ▼                                                                      │
│  DisplayManager ──► IDisplay                                                   │
│        ├── HardwareDisplay (真实 LCD, 阶段1为 stub)                             │
│        ├── RemoteDisplay  (编码→协议→Transport)                                │
│        └── MirrorDisplay  (扇出到多个 IDisplay)                                 │
│        ▲                                                                      │
│  InputManager ◄── 统一 InputEvent（来自 PC 或未来物理触摸）                      │
│        ▲                                                                      │
│  Protocol 层（编码/解码/封包/CRC/心跳）                                         │
│        ▲                                                                      │
│  Transport 层：UartTransport（阶段1）/ UsbTransport(TinyUSB, 未来)              │
└───────────────────────────────────────────────────────────────────────────────┘
```

## C. 三层架构图

```
┌────────────┐    ┌──────────────────────────┐    ┌──────────────────┐
│ ESP32 App  │    │ Protocol v0.1            │    │ PC Qt GUI        │
│  UI/LVGL   │◄──►│  Header+Payload+CRC      │◄──►│  虚拟屏幕窗口     │
│ DisplayMgr │    │  FRAME_RECT/INPUT/...    │    │  输入捕获/统计    │
│ InputMgr   │    └──────────────────────────┘    └──────────────────┘
└─────┬──────┘             │ 传输无关                ──────┬──────
      │                    │                              │
      ▼                    ▼                              ▼
 Transport 抽象      UartTransport (COM3, 阶段1)     QSerialPort Transport
 (ITransport)        UsbTransport (S3, 未来)         TcpTransport (未来)
```

依赖规则（单向）：
`Application → Display/Input API → Protocol → Transport`
任何一层都不得反向依赖。PC/ESP32 共享 `shared/protocol` 纯 C++ 头文件，不含任何平台依赖。

---

## D. 关键 C++ 接口设计

### 1. IDisplay（ESP32 侧）

```cpp
namespace espview {

struct DisplayInfo {
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::RGB565;
    bool supportsDirtyRect = true;
};

class IDisplay {
public:
    virtual ~IDisplay() = default;
    virtual esp_err_t init(const DisplayConfig& cfg) = 0;
    virtual const DisplayInfo& info() const = 0;
    virtual esp_err_t writeRect(int x, int y, int w, int h,
                                const uint8_t* pixels) = 0;   // 内存中已有数据
    virtual esp_err_t flush() = 0;                             // 提交到后端
    virtual esp_err_t setEnabled(bool enabled) = 0;
};

} // namespace espview
```

### 2. DisplayManager / 模式切换

```cpp
enum class DisplayMode { Window, Device, Mirror };

class DisplayManager {
public:
    esp_err_t setMode(DisplayMode mode);   // 只改组合，不动 Application
    IDisplay& active();                     // 对 Application 透明
    void addBackend(std::shared_ptr<IDisplay> backend);
};
```

实现要点：
- `Window`  → 只挂 `RemoteDisplay`
- `Device`  → 只挂 `HardwareDisplay`
- `Mirror`  → 挂 `MirrorDisplay`，内部扇出到 Remote + Hardware
- 运行时切模式 = 重建 `active()` 指向的 display 链；Application 无需重编译。
- **v0.1 只实现编译期模式**（`ESPVIEW_DEFAULT_MODE` 配置）；运行时 `setMode()` 接口保留、M6 实现。
- **LVGL flush_cb 固定经 `DisplayManager::active()` 转发**（不直接绑后端）；模式切换后需全屏置脏触发一次重绘。这样 Application 与 LVGL 配置零改动。

### 3. ITransport（ESP32 与 PC 各有一份实现，接口同构）

```cpp
class ITransport {
public:
    enum class State { Disconnected, Connecting, Connected, Error };
    using DataCallback = std::function<void(const uint8_t* data, size_t len)>;
    using StateCallback = std::function<void(State state)>;
    virtual esp_err_t open(const TransportConfig& cfg) = 0;
    virtual void close() = 0;
    virtual bool isConnected() const = 0;
    virtual esp_err_t send(const uint8_t* data, size_t len) = 0;  // 完整发送
    virtual void setDataCallback(DataCallback cb) = 0;            // 收包回调
    virtual void setStateCallback(StateCallback cb) = 0;          // 连接状态回调
    virtual size_t mtu() const = 0;
};
```

> M0 不实现任何 Transport；UartTransport / UsbTransport / TcpTransport 全部留到 M1 之后。状态回调驱动 E 节连接状态机。

### 4. InputManager（ESP32 侧）

```cpp
struct InputEvent {
    enum class Type { MouseMove, MouseDown, MouseUp, MouseWheel,
                      KeyDown, KeyUp, TouchDown, TouchMove, TouchUp };
    Type type;
    uint16_t x, y;          // 显示坐标系
    uint8_t  buttons;       // 位掩码
    int8_t   wheelDelta;
    uint32_t keycode;       // USB HID usage code（OS/传输无关）
    uint16_t modifiers;
    uint64_t timestampMs;
};

class IInputListener {
public:
    virtual void onInputEvent(const InputEvent& e) = 0;  // 由 InputManager 回调
};

class InputManager {
public:
    void registerListener(IInputListener* l);
    void feed(const InputEvent& e);   // PC 输入与物理触摸统一入口
};
```

### 5. Protocol 层

```cpp
// 编码（ESP32 / PC 共用）
class ProtocolEncoder {
public:
    // 帧：BEGIN → 0..n 个 RECT → END；大矩形自动拆多包（CHUNKED），见 E 节
    void encodeFrameBegin(uint16_t frameId, FrameType type, uint16_t w, uint16_t h,
                          std::function<void(const uint8_t*, size_t)> emit);
    void encodeRect(const Rect& r, const uint8_t* pixels,
                    std::function<void(const uint8_t*, size_t)> emit);
    void encodeFrameEnd(uint16_t frameId, uint16_t rectCount, uint32_t byteCount, bool aborted,
                        std::function<void(const uint8_t*, size_t)> emit);
    void encodeInput(const InputEvent& e, ...);
    // HELLO / SET_MODE / PING / PONG / ERROR / ACK ...
};

// 解码（PC 侧，ESP32 侧同理）
class ProtocolDecoder {
public:
    // 流式喂入字节；内部处理粘包/拆包/CRC/长度校验
    void feed(const uint8_t* data, size_t len);
    void setHandler(IMessageHandler* h);
};
```

---

## E. Protocol v0.1 设计

### 三层概念：Packet / Message / Frame

- **Packet（包）**：线路上的最小单位 = 固定 20 字节头 + 载荷片段（0..4096 B）。接收端逐包校验 CRC 后消费。
- **Message（消息）**：一条逻辑协议消息（HELLO、FRAME_RECT、INPUT_MOUSE…），对应一个 TYPE。载荷 ≤ 4096 B 时 = 1 个包；> 4096 B 时拆为连续 n 个包（前 n-1 个 `CHUNKED=1`，末包 `CHUNKED=0`）。完整 payload（所有 CHUNKED Packet 拼接后）≤ `MAX_MESSAGE_PAYLOAD`（1 MiB）。**拆包是“消息→包”层行为**，与帧边界无关。
- **Frame（帧）**：一次显示事务 = `FRAME_BEGIN` 消息 + 0..n 个 `FRAME_RECT` 消息 + `FRAME_END` 消息。帧是“整帧提交 / 整帧丢弃”的唯一载体。
- 控制消息（HELLO/SET_MODE/INPUT_*/PING/PONG/ERROR/ACK）**不属于任何帧**，可以穿插在不同 Message 之间，但**不得插入同一个 CHUNKED Message 的连续 Packet 之间**；帧状态机只响应 BEGIN/END。
- 对于一个 CHUNKED Message：Packet[0] … Packet[n-2] 的 CHUNKED=1，Packet[n-1] 的 CHUNKED=0；期间 TYPE 必须保持一致；Packet 必须连续（SEQ 连续）；不允许其他 Message 的 Packet 插入。
- **实现（M1-3C）**：`MessageEncoder` 同时支持完整 payload 编码（`encode`）与流式编码（`encodeStreaming(header, source, sink)`：payload 由 `IMessagePayloadSource::read()` 按需产生，先聚到 4096B packet 级 staging 再编码，逐包交给 PacketSink）。两条路径对同一逻辑 payload 产生**逐位一致**的 Packet（同一拆分 / CHUNKED / SEQ / CRC 规则）。**ESPView 不要求完整 Message payload 常驻内存**：经典 ESP32 用流式 RECT 发送 153608B 级 FRAME_RECT，峰值额外内存 ≈ 4096B staging + 4116B 单包缓冲（≈ 8.2KB），避免 153KB 连续堆分配。

### Streaming Message API（正式设计，M1-3C 冻结）

逻辑 Message 的 **wire format 完全不变**，变化的只是「payload 在内存中的产生方式」：

- **两种等价 API**（`shared/protocol/encoder.h`）：
  - `MessageEncoder::encode(const Message&, ...)`：完整 payload 驻留内存（小消息 / 控制消息路径）；
  - `MessageEncoder::encodeStreaming(const MessageHeader&, IMessagePayloadSource&, IPacketSink&)`：payload 由 source 按需产生，不要求整段驻留内存（大消息 / FRAME_RECT 路径）。
- **Streaming 模型**：
  `IMessagePayloadSource::read(dst, maxBytes)` → packet 级 staging（≤ 4096B）→ CRC32 → `IPacketSink::writePacket(data, len)` → Transport。
  同一逻辑 payload 下两条路径产生**逐位一致**的 Packet 序列（同一拆分 / CHUNKED / SEQ / CRC 规则），host 单测逐字节比对。
- **内存语义**：
  - `MAX_MESSAGE_PAYLOAD = 1 MiB` 只是**协议级上限**（wire 上一条逻辑 Message 拼接后的 payload 上限），**不要求任何组件分配 1 MiB 缓冲**；
  - 320×240 RGB565 = 153600 bytes 的 FRAME_RECT **不要求 153600B 连续缓冲**：流式发送时峰值额外内存 ≈ 4096B staging + 4116B 单包缓冲（≈ 8.2KB）；
  - 经典 ESP32 已实测：153608B 单 RECT 以 38 个 CHUNKED Packet 流式发送，`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 前/中/后无 153KB 连续堆分配。

### 最终 Packet Header（固定 20 字节）

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 4 | MAGIC | 固定字节序 | `0x45 0x53 0x50 0x56`（'E''S''P''V'） | 帧同步标识 |
| 4 | 1 | VERSION | — | 0x01 | 协议版本；不兼容变更时递增 |
| 5 | 1 | TYPE | — | 0x01..0x51 | 消息类型（见消息表） |
| 6 | 1 | FLAGS | — | bit0=CHUNKED, bit1=ACK_REQ, bit2..7=0 | CHUNKED：本包是消息的续片；ACK_REQ：请求对端回 ACK |
| 7 | 1 | RSVD | — | 0 | 保留，接收方必须忽略 |
| 8 | 2 | SEQ | 小端 (LE) | 0..65535，回绕 | 包序号：每方向独立、每发一包 +1；用于丢包/乱序检测，v0.1 不做重传 |
| 10 | 4 | LENGTH | LE | 0..4096（= MAX_PACKET_PAYLOAD） | 本包 Payload 字节数 |
| 14 | 4 | CRC32 | LE | — | CRC32，见下方规范 |
| 18 | 2 | RSVD2 | — | 0 | 保留对齐字段，接收方忽略 |

> `MAX_PACKET_PAYLOAD = 4096`。115200 baud（正式 baseline）下单包（4116B）耗时约 356 ms；921600 下约 45 ms。足够小以保证 PING/INPUT 等控制消息穿插，足够大以装下绝大多数 dirty rect。

> **`MAX_MESSAGE_PAYLOAD = 1048576 (1 MiB)`（M0-C 正式冻结）**：
> - 一个逻辑 Message 的完整 payload（所有 CHUNKED Packet 拼接后）不得超过 1 MiB；
> - Packet payload 上限仍为 `MAX_PACKET_PAYLOAD = 4096` bytes，不变；
> - **Encoder 超过上限必须拒绝**；**Decoder 超过上限必须拒绝**；
> - FrameAssembler 与两端使用同一协议常量；
> - 1 MiB 仅是 wire-level 最大值，**不要求任何组件分配固定 1 MiB 缓冲**；
> - ESPView 必须允许流式/分段处理，**不得因该上限产生固定 1 MiB framebuffer**；
> - 实现（M1-3C）：`MessageEncoder` 提供 Streaming Message API（`IMessagePayloadSource` + `encodeStreaming`），完整 payload 可不驻留内存。

### CRC32 规范（全参数固定）

- **覆盖**：Header 字节 [0,14)（MAGIC、VERSION、TYPE、FLAGS、RSVD、SEQ、LENGTH）+ 完整 Payload。
- **不覆盖**：CRC32 字段本身（字节 14..17）与 RSVD2（字节 18..19）。
- **算法**：IEEE 802.3 / zlib CRC-32。
- **polynomial**：0xEDB88320（反射形式；普通形式 0x04C11DB7）。
- **初始值**：0xFFFFFFFF。
- **reflected input / output**：是 / 是。
- **final XOR**：0xFFFFFFFF。
- **wire byte order**：小端（低字节在前）。
- **交叉验证向量**：`CRC32("123456789") == 0xCBF43926`（M0 单测必须含此用例）。
- 实现约束：ESP32 侧不得假设 `esp_rom_crc32_le` 与 zlib 语义等价，必须过同一向量测试；PC 侧用 zlib/标准实现。

### 消息表（v0.1）

| TYPE | 方向 | 消息 | 载荷简述 | ACK | MVP |
|------|------|------|----------|-----|-----|
| 0x01 | ESP→PC | HELLO | 版本/分辨率/像素格式/模式掩码/设备名 | 由对端 HELLO 隐式确认 | ✔ |
| 0x02 | 双向 | CAPABILITIES | 能力协商扩展（可并入 HELLO） | 可选 | 可选 |
| 0x03 | PC→ESP | SET_MODE | mode（0=WINDOW,1=DEVICE,2=MIRROR） | 必须 ACK_REQ | ✔ |
| 0x04 | PC→ESP | SET_RESOLUTION | 运行时改分辨率 | 必须 | 未来 |
| 0x05 | PC→ESP | SET_PIXEL_FORMAT | 运行时改像素格式 | 必须 | 未来 |
| 0x10 | ESP→PC | FRAME_BEGIN | frameId/type/fmt/w/h/byteHint | 否 | ✔ |
| 0x11 | ESP→PC | FRAME_RECT | x/y/w/h + 像素（可跨包） | 否 | ✔ |
| 0x12 | ESP→PC | FRAME_END | frameId/rectCount/byteCount/flags | 否 | ✔ |
| 0x20 | PC→ESP | INPUT_KEY | HID usage + modifiers + down | 否 | ✔ |
| 0x21 | PC→ESP | INPUT_MOUSE | buttons + x + y + wheel + flags | 否 | ✔ |
| 0x22 | PC→ESP | INPUT_TOUCH | — | 否 | 未来 |
| 0x30 | 双向 | PING | timestampMs | 以 PONG 响应 | ✔ |
| 0x31 | 双向 | PONG | timestampMs | 否 | ✔ |
| 0x40 | 双向 | RESET | 保留类型号，v0.1 不实现 | 必须 | 未来 |
| 0x50 | 双向 | ERROR | errorCode + 文本 | 否 | ✔ |
| 0x51 | 双向 | ACK | ackSeq + status + errorCode | 仅响应，不再次 ACK | ✔ |

### 帧消息 Payload Layout

**FRAME_BEGIN (0x10)**

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 2 | frameId | LE | 0..65535，回绕 | 帧序号，每帧 +1；接收端据此检测丢帧 |
| 2 | 1 | frameType | — | 0=FULL, 1=PARTIAL | 0=整帧（握手/重连重同步用），1=局部更新 |
| 3 | 1 | pixelFormat | — | 0=RGB565 | 本帧像素格式，一帧内唯一 |
| 4 | 2 | width | LE | 1..4096 | 逻辑显示宽度 |
| 6 | 2 | height | LE | 1..4096 | 逻辑显示高度 |
| 8 | 4 | byteHint | LE | 0..0xFFFFFFFF（0=未知） | 本帧预计像素总字节数，仅提示 |

> **PARTIAL 提交语义（M0 收尾正式冻结）**：
> - PARTIAL frame 只允许应用到**最近一次成功提交的 frame**（FULL 或 PARTIAL 提交后即为新基准）；
> - 若当前**没有任何已提交 frame 作为基准**（如刚连接、断线重连后、或此前从未提交过任何帧）：
>   PARTIAL frame **不得提交**（接收端丢弃）；
> - 接收端继续等待 **FULL frame 进行重同步**；FULL frame 提交后即重新建立基准。
> - 该规则是接收端提交语义，不改变 wire format（FrameAssembler 在 FRAME_END 校验通过后追加判断）。

**FRAME_RECT (0x11)**'

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 2 | x | LE | 0..width-1 | 矩形左上角 x |
| 2 | 2 | y | LE | 0..height-1 | 矩形左上角 y |
| 4 | 2 | w | LE | 1..width-x | 矩形宽 |
| 6 | 2 | h | LE | 1..height-y | 矩形高 |
| 8 | n | pixels | LE | n = w×h×bpp（RGB565: bpp=2） | 像素数据；n > 4088（= 4096−8）时拆包 |

> 像素格式由 FRAME_BEGIN 声明，FRAME_RECT 不重复携带；同一帧内格式唯一。

**FRAME_END (0x12)**

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 2 | frameId | LE | 0..65535 | 必须与 BEGIN 的 frameId 一致 |
| 2 | 2 | rectCount | LE | 0..65535 | 本帧实际 FRAME_RECT 消息数 |
| 4 | 4 | byteCount | LE | — | 本帧实际像素总字节数 |
| 8 | 1 | flags | — | bit0=ABORTED, bit1..7=0 | ABORTED=发送端主动作废本帧，接收端丢弃帧内全部数据 |

### 控制消息 Payload Layout（v0.1）

**HELLO (0x01)**

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 1 | protocolVersion | — | 1 | 与 Header.VERSION 一致 |
| 1 | 1 | deviceClass | — | 0=generic | 保留分类 |
| 2 | 2 | width | LE | 1..4096 | 默认输出分辨率宽 |
| 4 | 2 | height | LE | 1..4096 | 默认输出分辨率高 |
| 6 | 1 | pixelFormat | — | 0=RGB565 | 默认像素格式 |
| 7 | 1 | modeMask | — | bit0=WINDOW, bit1=DEVICE, bit2=MIRROR | 支持的显示模式 |
| 8 | 1 | nameLen | — | 0..32 | 设备名长度 |
| 9 | n | deviceName | — | UTF-8 | 设备名，n=nameLen |

**SET_MODE (0x03)**：`[0] mode`（0=WINDOW, 1=DEVICE, 2=MIRROR）。单包，必须 ACK_REQ。

**ACK (0x51)**

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 2 | ackSeq | LE | 0..65535 | 被确认包的 SEQ |
| 2 | 1 | status | — | 0=OK, 1=ERR | 处理结果 |
| 3 | 2 | errorCode | LE | 0=无; 1=UNSUPPORTED_MODE; 2=INVALID_PARAM; 3=BUSY; 4=INTERNAL | status=ERR 时的错误码 |

**ERROR (0x50)**

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 2 | errorCode | LE | 1..65535 | 错误码（复用 ACK 错误码表） |
| 2 | 1 | msgLen | — | 0..64 | 文本长度 |
| 3 | n | message | — | UTF-8 | 人类可读描述 |

**PING / PONG (0x30 / 0x31)**：`[0..7] timestampMs`（LE，发送方单调毫秒时间，用于延迟测量与超时判定）。

**INPUT_KEY (0x20)**

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 4 | keycode | LE | USB HID usage code | 键码（0..0xFFFF 有效） |
| 4 | 2 | modifiers | LE | bit0=Ctrl, bit1=Shift, bit2=Alt, bit3=GUI | 修饰键位掩码 |
| 6 | 1 | down | — | 0/1 | 1=按下, 0=抬起 |
| 7 | 1 | rsvd | — | 0 | 保留 |

**INPUT_MOUSE (0x21)**

| 偏移 | 宽度 | 字段 | 大小端 | 取值范围 | 语义 |
|------|------|------|--------|----------|------|
| 0 | 1 | buttons | — | bit0=LEFT, bit1=RIGHT, bit2=MIDDLE | 按键位掩码 |
| 1 | 2 | x | LE | 0..width-1 | 显示坐标 x |
| 3 | 2 | y | LE | 0..height-1 | 显示坐标 y |
| 5 | 1 | wheel | — | -128..127 | 滚轮增量（有符号） |
| 6 | 1 | flags | — | bit0=ABS（v0.1 恒为 1） | 坐标类型：绝对 |
| 7 | 1 | rsvd | — | 0 | 保留 |

### ACK 语义（只服务控制消息）

- 仅**单包控制消息**可置 ACK_REQ（v0.1：SET_MODE；未来：SET_RESOLUTION/SET_PIXEL_FORMAT/RESET）。
- 接收方处理完后回 `ACK{ackSeq=该包SEQ, status, errorCode}`；无 ACK_REQ 绝不主动回 ACK。
- 发送方置 ACK_REQ 后启动 500 ms 超时，最多重试 2 次，仍失败则上报 ERROR/UI。
- 显示数据（FRAME_*）与输入（INPUT_*）一律 fire-and-forget；可靠性由帧级语义（BEGIN/END + 丢帧丢弃）保证；HELLO 的确认 = 对端 HELLO；PING/PONG 独立机制。

### 字节流解码状态机（两端共用）

```
        ┌────────────────────────────────────────────────────┐
        │  SYNC：逐字节扫描 MAGIC('E''S''P''V')               │
        │  伪 MAGIC 由 HEADER 合法性 + CRC 双重淘汰            │
        └───────────────┬────────────────────────────────────┘
                        │ 命中 MAGIC
                        ▼
                      HEADER（20B）
         校验：VERSION==1？LENGTH≤4096？TYPE 合法？  ──非法──► 回到 SYNC（从头第 2 字节继续扫）
                        │ 合法
                        ▼
                      PAYLOAD（读满 LENGTH 字节）
                        ▼
                      VERIFY：CRC32(header[0..14) + payload) == header.CRC32
              ┌─────────┴─────────┐
              │ 通过              │ 失败：丢弃整包，回 SYNC
              ▼                   ▼
       按 CHUNKED 组装 Message     （从失败包 payload 之后继续扫）
              │
              └──► 消息完成（末包 CHUNKED=0）→ dispatch → 回 SYNC
```

- **粘包**：缓冲内循环消费所有完整包。
- **半包**：残留部分留在缓冲等待；**超过 500 ms 未收齐 → 强制回 SYNC**（防串口残留字节毒化）。
- **重同步性质**：CRC 失败/seq 跳变只作废当前帧，下一个 FRAME_BEGIN 即恢复，最多损失一帧延迟。

### 帧级错误处理

| 情况 | 处理 |
|------|------|
| seq 跳变（收到非 last+1） | 当前帧作废：丢弃本帧全部已收 RECT，直到下一个 FRAME_BEGIN |
| CRC 错误 | 丢弃该包；该包所属消息作废；若在帧内 → 当前帧作废，继续重同步 |
| 半包 | 缓冲等待；> 500 ms 未收齐回 SYNC |
| 粘包 | 缓冲内循环消费 |
| 未 END 又收到 FRAME_BEGIN | 上一帧作废，开始新帧 |
| FRAME_END.frameId ≠ BEGIN.frameId | 本帧作废（丢弃帧内数据） |
| FRAME_END.flags.ABORTED=1 | 本帧作废 |
| PARTIAL 无已提交基准帧 | PARTIAL 不提交（丢弃），继续等待 FULL frame 重同步（见 FRAME_BEGIN 表后说明） |
| 断线重连 | 见连接状态机；PC 清黑 QImage，收到完整 FULL 帧才绘制 |

### 连接状态机（双方）

```
DISCONNECTED → CONNECTING（打开传输）
   → HANDSHAKE（HELLO 互换；双方 packet.seq 清零；ESP32 立即发 FULL 帧：
               BEGIN(FULL) + 全屏 RECTs + END）
   → CONNECTED（PING 每 2 s；对端 5 s 无响应判超时）
   → 超时/错误 → DISCONNECTED → PC 自动退避重连 / ESP32 等待对端 HELLO
```

- **每次进入 CONNECTED 都强制 FULL 帧重同步**（见 H 节“PC 只保留最后一份完整提交帧”）。

### 典型交互序列

```
连接建立
 ESP32 → PC : HELLO {ver=1, 320x240, RGB565, modeMask=0b101}
 PC   → ESP : SET_MODE(WINDOW) [ACK_REQ]   →  ESP32 → PC : ACK{ackSeq, OK}
 ESP32 → PC : BEGIN{FULL, frameId=1} → RECT{0,0,320,240} → END{1, 1, 153600}

显示刷新
 ESP32 → PC : BEGIN{PARTIAL, frameId=42}
              RECT{x=8,y=16,w=64,h=32}            （≤4096B，单包）
              RECT{x=0,y=0,w=320,h=240}           （大矩形，拆为 n 个 CHUNKED 包 + 末包）
              END{42, 2, 153600}

输入（穿插在帧流之间，不属于帧）
 PC   → ESP : INPUT_MOUSE{LEFT, 120, 80, 0}
 PC   → ESP : INPUT_KEY{hid=0x29 (A), mods=0, down=1}

保活
 双向周期 : PING / PONG（ESP32 每 2 s，PC 超时 5 s 判定断线）
```

### 设计取舍（保持）

- 不需要每包 ACK（ACK 只服务控制消息）；不需要 RESET 进 v0.1（重连 + 重新 HELLO 解决）。
- 帧数据不做压缩（阶段 1）；未来在 FLAGS 加 COMPRESSED 位 + RLE/LZ4（不影响本版字段布局）。

---

## F. 三种 DisplayMode 的实现方式

| 模式 | active() 返回 | RemoteDisplay | HardwareDisplay |
|------|--------------|---------------|-----------------|
| Window | RemoteDisplay | 开启 | stub（不初始化） |
| Device | HardwareDisplay | 关闭 | 开启 |
| Mirror | MirrorDisplay | 开启 | 开启（同一份 writeRect 扇出） |

- `MirrorDisplay::writeRect()` = 依次调用两个后端，若任一失败标记 degraded 并记录 ERROR，不阻塞 UI。
- 模式切换在 DisplayManager 内原子完成（互斥锁 + 重建 display 链），Application 只持有 `IDisplay&`，无感知。
- 阶段 1 无真实 LCD，`HardwareDisplay` 提供 `LCD_STUB`（模拟计时/占位），保证 MIRROR 路径代码真实可跑。
- **v0.1 只实现编译期模式**（`ESPVIEW_DEFAULT_MODE` 配置）；`DisplayManager::setMode()` 运行时接口保留，M6 实现。
- 运行时切换前提（M6）：flush_cb 固定经 `DisplayManager::active()` 转发；切换后 LVGL 全屏置脏重绘；后端初始化惰性完成。Application 零改动。

---

## G. 输入事件架构

```
PC 鼠标/键盘 ──► Qt VirtualScreenWidget ──► ProtocolEncoder(INPUT_*) ──► Transport
                                                                          │
ESP32 物理触摸(未来) ─────────────────────────────► InputManager ◄────────┘
                                                        │ 统一 InputEvent
                                                        ▼
                                             Application 的 IInputListener
                                             （LVGL input_read_cb 或自定义）
```

- 键码统一用 **USB HID usage table**（PC 端 Qt::Key → HID 映射，ESP32 端直接消费），彻底摆脱 Windows 虚拟键码依赖。
- 鼠标坐标使用**显示坐标系**（0..width-1, 0..height-1），由 PC 按当前分辨率换算后发送。
- 物理触摸与 PC 触摸最终都变成同一个 `InputEvent`，Application 无感知来源。

### M3 实现语义（冻结；wire format 未修改）

- **autoRepeat（键盘）**：PC 端对带 Qt `autoRepeat` 标志的 `KeyDown` **直接忽略**（不重复发送 KeyDown）；`KeyUp` **始终发送**。ESP32 端不依赖该抑制，Wire 上只有 KeyDown / KeyUp 两种键盘事件。
- **MouseMove 频率**：PC 端 MouseMove **≤ 60 Hz 节流 + 合并（coalescing）**，只发送最新坐标；MouseDown / MouseUp / MouseWheel **实时发送**，不节流。节流/合并放在 InputController / SerialWorker 发送路径，GUI 线程不直接操作串口。
- **MouseDown / MouseUp 推导**：`INPUT_MOUSE` 在 wire 上**没有事件类型字段**（只有坐标 + buttons 掩码 + wheelDelta）。ESP32 `InputManager` 按 buttons 掩码相对上一状态的**变化位**推导 MouseDown / MouseUp；Move / Wheel 只更新位置/滚轮，不改变按钮状态。
- **reconnect / resetState()**：Transport 断开或会话重置时，ESP32 侧调用 `InputManager::resetState()` —— 对当前按下中的键补发**本地 KeyUp**、对按下中的鼠标按钮补发 **MouseUp(buttons=0)**，**绝不把 release 回发给 PC**；HELLO 重连完成即状态清空（与显示 FULL resync 相互独立）。PC 侧断线即丢弃未发送的输入队列，重连握手后从全新状态继续。
- **输入线程模型**：Qt 事件 → InputEvent → queued request → SerialWorker → `ProtocolEndpoint::sendMessage(INPUT_*)` → HostUartTransport → COM3；GUI 线程不得直接操作串口。显示路径（ESP32→PC）在 M3 中完全不变，输入是独立反向通道。

---

## H. Framebuffer 与 Dirty Rectangle

### 所有权模型（Pre-M0 修正）

- **ESPView 不持有长期完整 framebuffer**。权威像素状态归 **Application / UI 框架（LVGL）** 所有；ESPView 只是显示后端的“汇入点”。
- `RemoteDisplay` 采用 **writeRect() / dirty-rect 汇入模型**：只接收“矩形 + 像素指针”，立即编码入 TX 队列，**绝不分配/缓存整屏**。
- 两种受支持形态：
  - **流式模式（默认）**：UI 框架（LVGL flush_cb）逐矩形调用 `writeRect(x,y,w,h,pixels)`，ESPView 无整屏缓冲；矩形像素经 Streaming Message API 逐 4096B Packet 编码送出，不复制整矩形到堆（M1-3C 已验证 153608B 单 RECT）；
  - **透传模式**：Application 自持 framebuffer（自有渲染循环），变化时调用 `writeRect` 指向其内存，ESPView 同样不复制整屏。
- “ESP32 是唯一权威”的含义修正：权威 = **决定像素内容的实体**（App/LVGL），不是“ESPView 缓存了整屏”。

### 帧策略（可选 RectAccumulator，非整屏缓存）

- 可选组件只维护**矩形元数据**（不含像素）：`markDirty(x,y,w,h)` → 合并相邻/重叠矩形 → 按帧率上限（20–30 fps）发一帧 `BEGIN/RECTs/END`。
- 变化面积 > 60% 整屏 → 退化为一帧 FULL（`BEGIN(FULL)` + 全屏 RECT），避免小矩形洪泛。
- 该策略属于“帧调度”，与协议无关，M0 不实现。

### PC 端：只保留最后一份完整提交帧

- PC 端 QImage 镜像 = **最后一份完整提交帧的显示缓冲**（收到 FRAME_END 才提交），**不是 ESP32 的第二份权威 framebuffer**。PARTIAL 帧只叠加到最近一次成功提交的帧；无提交基准时 PARTIAL 不提交，等待 FULL 帧重同步（见 E 节 PARTIAL 提交语义）。
- 断线即清黑 QImage、复位帧组装器；**只有收到第一帧完整 FULL 帧后才开始绘制**（见 E 节连接状态机）。
- 帧内部分数据（BEGIN 后未 END）随时可丢弃，不产生半成品画面。

### 像素格式

- 阶段 1：RGB565（原生 16bpp，LVGL 默认支持；字节序低字节在前）。
- 未来扩展点：`PixelFormat{RGB565, RGB888, Gray8, Mono1}` + 每格式的 stride/字节序描述，禁止把 320x240 RGB565 写死在协议/显示层。

---

## I. 线程模型

### ESP32（FreeRTOS）
```
[UI/App Task] ──DisplayManager──► [RemoteDisplay] ──(锁/队列)──► [TX Task] ──► UART
[RX Task] ◄── UART ──► [Protocol 解码] ──► [InputManager] ──► App/UI
```
- 职责划分：UI 任务只写“内存中的矩形”；TX 任务负责编码 + 发送；RX 任务负责解码 + 输入分发。
- 同步：`writeRect` 与 TX 之间用**无锁环形缓冲 + 信号量**；DisplayManager 模式切换用短临界区。
- **背压（TX 队列满）**：以帧为粒度丢弃——不阻塞 UI/flush_cb；丢弃整帧并置 coalesce，队列排空后补发 FULL 帧；已发 BEGIN 后队列再次填满 → 发 `FRAME_END(ABORTED)` 作废本帧。详见 E 节。
- 优先级：TX/RX > UI；UART 中断直接进 RX 队列，不在中断里做协议处理。

### PC（Qt）
- Qt 主线程：GUI、绘制、输入捕获。
- 一个 Worker 线程：`QSerialPort` 读写 + 协议解码 + 帧重组。
- 通信：framebuffer 用 `QImage` 由 Worker 更新后以 `Qt::QueuedConnection` 发到主线程；输入事件从主线程投递到 Worker 的 TX 队列。
- 断线重连：Worker 状态机（Disconnected→Handshaking→Connected→Error→重连），超时由 PING/PONG 驱动；重连成功先清黑 QImage，收到完整 FULL 帧才开始绘制。

### M4 运行态统计层（实现语义；**不改变协议**）

M4 不新增任何 wire 字段，不修改 Packet/Message/Frame 布局；下列全部是**实现层语义**，
供 GUI 状态栏、诊断日志与验收工具使用。计数器不回绕（uint64）。

- **分域统计**（spec §三）：Connection（Transport/Protocol 状态分离）、Display（帧）、
  Protocol（Packet/字节/错误）、Heartbeat（PING/PONG/RTT）四域 + Input（反向通道）。
  不合并成几百字段的巨型 struct。
- **RTT 语义**（spec §六）：`RttAggregate.lastMs` 为 `std::optional<uint32_t>`；
  `nullopt` = **无测量**（会话未建立 / 大帧流式发送期间 PONG 被 best-effort 放弃 /
  会话已结束），GUI 显示 `N/A`，**禁止用 0 表示无测量**。`avg/min/max/samples` 按会话
  累计；断线/重连时 reset()，不保留跨会话旧值。
- **Heartbeat best-effort**（spec §七）：PING 每 2s 由会话 tick 经 `tryTransmit` 发送；
  长流式消息（153KB FRAME_RECT，持有 sendMutex_ 十余秒）期间 PING/PONG 会被放弃
  （锁忙或 TX 缓冲满），不阻塞会话状态机，**绝不以心跳延迟误判断开**；对端 5s 无任何
  CRC 通过的包才触发 peer timeout（`heartbeatTimeouts`）。实测：115200 大帧 13.4s
  期间 PC 发 7 个 PING，`timeouts=0`，无误断开。
- **非阻塞控制发送（trySink，M4 可靠性修复）**：`tryTransmit`（PONG/ACK 回复、心跳
  PING、ACK 重试）使用独立的非阻塞 sink（单次尝试，缓冲满立即返回背压）。原因：RX
  任务/会话 tick 若进入 paced sink 的重试循环，会阻塞 UART RX 读取（大帧期间输入
  丢包）并长时间持有 sendMutex_（帧流停滞 → 对端误判超时）。主 sink（paced）仅用于
  应用层消息（帧流）。
- **错误分级与诊断**（spec §十九/二十）：`Severity` = Info/Warning/Error/Critical；
  `DiagnosticsRing` 保留最近 50 条（timestamp / severity / source / message），与
  Packet parser 解耦；GUI 不刷屏——累计计数 + 最近 N 条。
- **帧统计**（spec §八/九）：FrameStats 分 `commitsFull / commitsPartial /
  discardsTotal / discardByReason[11]`；丢弃分类：crc / sequence / aborted /
  invalidRect / partialWithoutBase / transport / reset 等（见 `FrameDiscardReason`）。
- **Packet 统计**（spec §十）：`packetsRx` = **通过 CRC 的包数**（含被 seq 规则丢弃的
  包，仍证明对端活着）；错误计数与丢弃计数分离：`crcErrors / seqGaps / chunkErrors /
  badMagic / badVersion / badHeader`。
- **吞吐 / FPS**（spec §十一/十二）：`rxBytesPerSec / txBytesPerSec` 基于真实
  bytes/elapsed（非理论波特率）；FPS = 最近统计窗口内提交帧数 / 窗口时长，区分
  FULL/PARTIAL 速率；只作诊断，不改变发送策略。
- **重连**（spec §十五）：`reconnectCount / disconnectCount / lastDisconnectReason /
  lastReconnectDurationMs`；重连后 RTT 重置、清空旧画面、等待新 FULL commit 才重新显示。
- **统计通道**：ESP32 输入/会话统计经 `ERROR` 消息文本上报（≤64B/条，拆多行
  `inp / inp2 / sess / sess2`），PC 验收工具 `sscanf` 严格解析；**wire format 未修改**。
- **GUI 状态栏**（spec §十八）：4 域网格：Connection / Display / Protocol / Heartbeat
  + Input 行；RTT 显示 `N/A` 而非 0ms；错误按级别着色（INFO/WARNING/ERROR/CRITICAL）。

---

## J. 内存与带宽估算

### 内存（ESP32 经典，约 320KB 可用 DRAM；Pre-M0 修正后无 ESPView 整屏缓存）

| 项 | 大小 | 归属 |
|----|------|------|
| LVGL draw buffer（1/10 屏，320x240 RGB565） | ~16–32 KB | UI 框架（LVGL）持有 |
| RemoteDisplay TX 队列 + 可选 RectAccumulator（无整屏缓存） | 16–32 KB | ESPView |
| 协议 RX/TX 缓冲（4096 B 包，双方向） | ~8–16 KB | ESPView |
| 合计（不含 Application 自身） | ~40–80 KB | — |

- 结论：相比旧设计（150 KB 整屏缓存 + 三缓冲），修正后 ESP32 侧典型占用 **~40–80 KB**，余量充足；后续加 LVGL + 更大分辨率仍建议 PSRAM 或 ESP32-S3。
- 隐藏双/三缓冲已消除：LVGL draw buffer → flush_cb 拷贝入 TX 队列 → UART 驱动缓冲（原版 ESP32 UART 无 DMA，此拷贝不可避免）。**全程单一像素拷贝**，无 ESPView 整屏缓存。

### 带宽（真实测量，M1-3D 修订）

**当前硬件 baseline 的参考实测**（320×240 RGB565 整帧 = 153600 bytes，ESP32→UART→CH340→COM3 @ 115200 8N1）：

- 单帧耗时 ≈ **13.5 s**；有效 payload throughput ≈ **11.1 KB/s**。
- 这是**当前板级链路的参考实测，不是协议上限**；协议本身不受该速率约束，未来可在更好的 Transport（TinyUSB / Wi-Fi TCP）上提高吞吐，无需改动协议。

| 通道 | 有效 payload 吞吐 | 4096B 满包（4116B）耗时 | 整帧耗时（153600B） | 状态 |
|------|------------------|------------------------|---------------------|------|
| UART 115200 | ~11.1 KB/s（实测） | ~356 ms | ~13.5 s（实测） | **正式 baseline，可靠验收速率** |
| UART 921600 | ~92 KB/s（理论上限） | ~45 ms | ~1.7 s（理论） | **experimental only**：当前 经典 ESP32 + 板载 CH340 + 当前 Windows/驱动 组合下大帧突发实测不可靠（短包/控制面可用），不作为大帧验收速率 |
| UART 2M (CH340 上限) | ~200 KB/s | ~20 ms | ~0.8 s | 部分 CH340 版本不稳 |
| USB CDC (ESP32-S3, 高速) | 数十 MB/s | <1 ms | <10 ms | 未来 |

- **现实带宽限制（115200 baseline）**：整帧 ≥ 13.5 s，**高频整帧推送不可能**；常态目标是 **dirty rectangle + PARTIAL + streaming + 帧调度** 的部分更新（小 dirty rect 时负载 < 5%）。FULL 帧只用于握手/重同步/大范围变化。
- 帧率上限由 TX 侧帧调度控制；UART 背压时按 E 节策略**整帧丢弃**。
- 输入/心跳报文极小（<64B），带宽可忽略；PC 端 QImage 镜像在 PC 内存，不计入 ESP32 预算。

---
## K. ESP-IDF component 工程结构

```
ESPView/
├── docs/DESIGN.md
├── shared/protocol/                  # PC/ESP32 共用，纯 C++，零平台依赖（M0 全部）
│   ├── protocol.h                    # 常量、枚举、消息结构、CRC32 规范
│   ├── packet.h / packet.cpp         # 20B Header 编解码（LE + CRC32）
│   ├── encoder.h / encoder.cpp       # Message 编码（消息级拆包 CHUNKED + Streaming API）
│   ├── decoder.h / decoder.cpp       # 字节流解码状态机（SYNC/HEADER/PAYLOAD/VERIFY）
│   ├── frame_assembler.h / .cpp      # 帧组装/丢帧/提交语义（纯逻辑，M0 单测）
│   └── tests/                        # 主机侧单测 + in-memory transport 模拟（M0）
│       ├── crc_test.cpp              # CRC 向量（"123456789"→0xCBF43926）
│       ├── decoder_test.cpp          # 粘包/拆包/伪 MAGIC/CRC 失败重同步
│       └── frame_assembler_test.cpp  # 丢包/丢帧/重连重同步/ABORTED
├── esp32/
│   ├── CMakeLists.txt
│   ├── sdkconfig.defaults
│   └── main/
│       ├── CMakeLists.txt
│       ├── main.cpp                  # 演示应用（初始化 + LVGL/简单 UI）
│       └── app.cpp / app.hpp
│   └── components/
│       └── espview/
│           ├── CMakeLists.txt
│           ├── include/espview/
│           │   ├── idisplay.hpp      # IDisplay / DisplayInfo / PixelFormat
│           │   ├── display_manager.hpp
│           │   ├── input_manager.hpp # InputEvent / IInputListener
│           │   ├── rect_accumulator.hpp  # 可选帧策略（矩形元数据，非整屏缓存）
│           │   └── transport.hpp     # ITransport / TransportConfig
│           ├── src/
│           │   ├── remote_display.cpp
│           │   ├── hardware_display.cpp
│           │   ├── mirror_display.cpp
│           │   ├── display_manager.cpp
│           │   ├── input_manager.cpp
│           │   ├── rect_accumulator.cpp  # 可选帧策略（M0 不实现）
│           │   ├── transport_uart.cpp
│           │   ├── transport_usb.cpp  # (S2/S3, 预留)
│           │   ├── protocol_endpoint.cpp  # 集成 encoder/decoder + 心跳
│           │   └── espview_register.cpp   # 统一初始化入口
│           └── CMakeLists.txt
├── pc/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── main.cpp
│   │   ├── connection_manager.{h,cpp}
│   │   ├── serial_transport.{h,cpp}
│   │   ├── protocol_peer.{h,cpp}     # 复用 shared/protocol
│   │   ├── frame_assembler.{h,cpp}
│   │   ├── virtual_screen_widget.{h,cpp}
│   │   ├── input_forwarder.{h,cpp}   # Qt 事件 → InputEvent
│   │   └── status_model.{h,cpp}      # 连接/分辨率/FPS 统计
│   └── resources/
└── README.md
```

## L. Qt 6 工程结构（pc/）

- `VirtualScreenWidget`：继承 `QWidget`，`paintEvent` 绘制 QImage（缩放 + 等比），捕获 `mousePress/Move/Release/Wheel`、`keyPress/Release`。
- `ConnectionManager`：管理 Worker 线程生命周期、串口打开/关闭、断线重连、HELLO 握手。
- `SerialTransport`：封装 `QSerialPort`，暴露 `ITransport` 同构接口，便于未来换 TcpTransport。
- `FrameAssembler`：按 Message（CHUNKED 续片）重组、按 Frame（BEGIN/RECT/END）组装；`frameId` 落后/跳变 → 丢弃整帧；只在收到完整 FRAME_END 后提交一次重绘。
- `StatusModel`：QAbstractListModel 或简单 struct，展示 COM 口、波特率、分辨率、像素格式、FPS、RX/TX 字节、延迟。
- CMake 需 `find_package(Qt6 COMPONENTS Widgets SerialPort)`；本机用 MSYS2 MinGW Qt 6.11.1：`-DCMAKE_PREFIX_PATH=C:/msys64/mingw64`，运行时把 `C:\msys64\mingw64\bin` 加入 PATH。

## M. MVP 开发阶段划分（当前进度，M6-B 更新）

| 阶段 | 内容 | 状态 |
|------|------|------|
| M0 | `shared/protocol`：Packet / CRC32 / Message / Encoder / StreamDecoder / FrameAssembler / ProtocolEndpoint + host 单测 + in-memory pipeline | ✅ 完成（207900 checks / 0 failures，ctest 1/1） |
| M1-1 | ESP32 UartTransport + TestPattern 固定字节流 + PC COM3 字节级验收 | ✅ 完成（真实硬件） |
| M1-2 | ProtocolEndpoint 会话层（HELLO 握手 / PING-PONG / SET_MODE+ACK / 超时重连） | ✅ 完成（真实硬件） |
| M1-3A | FrameAssembler 帧级语义 + 错误恢复（CRC 损坏 / seq 跳变 / 重同步） | ✅ 完成 |
| M1-3B | 真实帧管线全模式验收：FULL / PARTIAL / corruption / seq-gap / reconnect / partial read / sticky packet | ✅ 完成（115200 baseline） |
| M1-3C | Streaming Message API + 经典 ESP32 无 153KB 连续分配 | ✅ 完成（153600B 单 RECT @ 115200 实测；修复测试工具 ByteQueue livelock 并新增 `--selftest-queue`） |
| M1-3D | 文档同步 + host-only 验证入口 + M2 前架构冻结检查 | ✅ 本阶段 |
| M2 | PC Qt 6 Virtual Display：串口 → 解码 → FrameAssembler → Qt VirtualScreenWidget | ✅ 完成（真实硬件：COM3 @ 115200，320×240 虚拟屏幕；HELLO / FULL small / FULL large / PARTIAL / disconnect / reconnect / FULL resync / 像素校验 全部通过） |
| M3 | 输入回传：鼠标/键盘 → INPUT_* → InputManager → 日志/UI 反馈 | ✅ 完成（真实硬件：COM3 @ 115200；Qt 鼠标/键盘 → HID usage → INPUT_KEY / INPUT_MOUSE → ESP32 InputManager → IInputListener 日志+统计；reconnect 卡键/卡鼠标恢复验证通过） |
| M4 | 运行态统计层（RuntimeStats / DiagnosticsRing / RTT optional / 丢帧分类 / 吞吐-FPS / 重连计数 / 4 域 GUI 状态）+ 大帧心跳与输入压力修复（非阻塞 trySink） | ✅ 完成（真实硬件 COM3 @ 115200；host 208432 checks / 0 failures；M2 六模式回归全过；大帧 heartbeat timeouts=0；大帧输入 4/4 到达；reconnect stuck-input 恢复 PASS） |
| M5-A | LVGL Display Backend：LVGL flush_cb → IDisplay/DisplayManager/RemoteDisplay → Streaming Encoder → UART → Qt 窗口（FULL→PARTIAL 真实 dirty rect） | ✅ 完成（M5-A：真实硬件 COM3 @ 115200；首次 FULL=153600B/10 rects/≈13.2s，后续 PARTIAL≈26KB/3 rects/≈2.3s，dirty≈16.9%，0 CRC/0 mismatch；Qt `--dump-png` FULL 帧像素 = LVGL UI；断线重连 FULL resync 通过；free heap before/after=304312/304312B，min=256348B，largest=110592B；host 208594 checks / 0 failures，ctest 1/1） |
| M5-B | LVGL Input Device：Qt 输入 → INPUT_* → InputManager → LvglInputAdapter → LVGL v8.4 POINTER/KEYPAD/ENCODER indev → demo UI（Button A/B 点击/Enter、Counter 滚轮/方向键、Keyboard/Mouse 标签） | ✅ 完成（真实硬件 COM3 @ 115200：鼠标 move/left/right/middle、wheel ±1、A/B/Enter/方向键/Ctrl+A/Shift+A 27 事件全收 0 dropped 0 invalid；FULL 后 LVGL 消费 k=4；reconnect stuck-input r=1 sk=1 sb=1 PASS；display 回归 FULL=153600B/PARTIAL=6392B dirty=4.16% 0 CRC；host 208777 checks / 0 failures，ctest 1/1） |
| M6-A | Wi-Fi STA + TCP Transport（PC = TCP Server 8765，ESP32 = STA Client；Protocol ≠ Transport，wire format 未修改；TCP byte-stream 语义；reconnect + FULL resync；host loopback 测试） | ✅ 完成（host TCP loopback 78 checks / 0 failures；ESP32 TCP/UART 双构建通过；真实硬件验收见 T.10） |
| M6-B | Flash 分区扩容（4 MiB flash + custom partition table + single factory app 2 MiB、no OTA）+ TCP 性能 5 轮实测（FULL/PARTIAL/RTT）+ power-save 对比实验 + host/ESP32 回归 （wire format 未修改） | ✅ 完成（2026-08-14 真实硬件 COM4：固件 1,027,488 B、app 余量 51%；详见 U 章） |

| M6-C | Runtime Transport Selection + Transport-aware TX Pacing（TransportManager + capabilities 模型；UART 保持 115200 pacing、TCP 取消 UART 固定节流；有界 TX 队列 + 整帧丢弃 + 控制公平；safe switch + 会话重置 + FULL resync；host transport tests；真实硬件双向运行时切换与断线重连验收；wire format 未修改） | ✅ 完成（2026-08-14 真实硬件 COM4：UART 13.2s/FULL 无回归，TCP 235–254ms/FULL、qp=2、0 drop；TCP↔UART 运行时切换零协议错误；断线重连 FULL resync；host 208910 checks / 0 failures；详见 V 章） |
| M7-A | 独立 OLED 状态显示（128×64 I2C SSD1306/SH1106，SDA=GPIO21 SCL=GPIO22；状态页 = transport/session/IP/RSSI/FRM/ERR/HEAP/UP；1KB 页式 fb + ≤32B 分段上传；有界错误恢复；wire format 未修改） | ✅ 完成（2026-08-15 真实硬件：probe 0x3C/SSD1306、15min 298 行 err=0 ok=1、0 CRC/bad_magic；host 210,504 checks / 0 failures；详见 Y 节） |
| M7-B | OLED 生产语义收尾（M7-A 审计 Mandatory 修复：OLED/I2C 生命周期、Transport 快照、OledStatus 可观测性；statsLoop `mem` 堆诊断行；pc_oled_monitor.py 重写；wire format 未修改） | ✅ 完成（2026-08-15 真实硬件：30 分钟 UART 长跑 heap +8472、0 CRC；TCP reconnect stress 10/10 OK；host 218,625 checks / 0 failures；详见 Z 节） |
| M6(未来) | 真实 LCD (DEVICE/MIRROR)、触摸（INPUT_TOUCH）、TinyUSB、运行时 DisplayMode | 未开始 |

### M2 前置架构冻结（M1-3D 检查）

M2 不重新设计以下链路（已冻结并实测）：

- ESP32：Application / TestPattern → `ProtocolEndpoint` → `MessageEncoder::encodeStreaming` → `IPacketSink` → `UartTransport` → UART；
- PC：`HostUartTransport` → `ProtocolEndpoint` → `StreamDecoder` → `FrameAssembler` → `CommittedFrame`（→ 未来 Qt VirtualDisplay）。

M2 的 Qt 层约束（冻结）：

- Qt 只接收 **CommittedFrame / DisplayFrame**（frameId / frameType / width / height / pixelFormat / rectCount / byteCount / rect 像素回调），**不得直接操作 StreamDecoder / Packet / CRC / CHUNKED / Packet SEQ**；
- 线程边界：Serial Worker（ReadFile + decoder.feed + ACK/PING 超时）→ CommittedFrame → Queued delivery → Qt GUI Thread（只做 QImage / paintEvent / resize / input event）；GUI 线程不得做串口阻塞 I/O 与协议解码；
- framebuffer 语义：PC 可持有「最后一次提交画面」（QImage 镜像），但 **PC framebuffer ≠ ESP32 权威 framebuffer**；FULL 替换整个画面；PARTIAL 只应用到最近一次 committed frame；断线清空/隐藏旧画面；重连后等待新 FULL。

M2 正式目标（只做）：PC Qt 6 Virtual Display —— 真实 ESP32 → 真实 COM3 → PC HostUartTransport → ProtocolEndpoint → FrameAssembler → Qt VirtualScreenWidget，在 Windows 上出现 320×240 ESP32 实际画面。
M2 不负责：LVGL、真实 LCD、Mirror、触摸、Wi-Fi、高 FPS、复杂 UI。

## N. 可能的技术风险

1. **经典 ESP32 无原生 USB**：阶段 1 只能 UART-over-USB；需向用户明确，UsbTransport 留待 S3。
2. **UART 带宽瓶颈**：整帧 @115200 实测 ≈ 13.5 s（有效 payload ≈ 11.1 KB/s）；921600 大帧突发在当前板级链路实测不可靠（experimental only），必须依赖 dirty rect + PARTIAL + 小矩形合并；UI 动画密集时可能掉帧。
3. **CH340 驱动/波特率兼容**：正式 baseline = 115200（M1-3C 冻结）；921600 = experimental，仅短包/控制面可用；2M 波特率部分 CH340 版本不稳。
4. **协议健壮性**：串口残留字节、半包、CRC 失败 → 必须实现重同步与状态机复位，否则 PC 端画面花屏。
5. **Qt/MSYS2 运行时**：Qt DLL 路径、windeployqt、串口权限；需要 `C:\msys64\mingw64\bin` 在 PATH。
6. **内存（Pre-M0 修正后）**：ESPView 无整屏缓存后典型占用 ~40–80 KB，余量充足；但 UI 框架自身（LVGL + draw buffer）仍需实测（`heap_caps_get_free_size`）。
7. **输入延迟**：Qt 事件 → 串口 → ESP32 的延迟累积，需用 PING/PONG 量化。
8. **ESP-IDF v6.0.2 新 API**：USB/UART 驱动 API 变化，以 v6.0.2 文档为准（不要照抄 v5.x 示例）。

## O. 参考项目与取舍

| 项目 | 借鉴 | 不复制 |
|------|------|--------|
| LVGL | flush callback 解耦显示后端；input device 抽象；draw buffer 模式 | 不把 LVGL 当通信层；不用其 monolithic 配置包办一切 |
| SDL | 事件模型（Mouse/Key/Touch 统一 Event）；窗口↔framebuffer 映射 | SDL 本身的渲染后端 |
| Qt | QImage 绘制、信号槽线程安全投递、QSerialPort | 不把业务逻辑放进 GUI 类 |
| QEMU virtio-gpu | “命令流 + scanout + dirty tracking”思想 → 对应我们的 FRAME_BEGIN/RECT/END | 其设备模型本身 |
| Espressif USB examples | TinyUSB CDC 配置、usb_device 组件结构 | 无 |
| 经典串口协议 (SLIP/MODBUS 思想) | 帧同步 + 长度 + CRC 的错误恢复范式 | MODBUS 的寄存器模型 |

核心取舍：**不引入“PC 是第二权威”的同步方案（如双向帧同步/回滚）**；ESP32 权威 + 单向像素流 + 输入回传 足够且简单。

## P. 第一阶段实施状态（文件清单 + 进度）

已完成（M0–M3，详见 M 节）：

1. `docs/DESIGN.md` — 架构定稿（本文件）。
2. `shared/protocol/`（packet / crc32 / message / encoder(+streaming) / decoder / frame_assembler / protocol_endpoint）+ `tests/` — 纯 C++17 host 单测，in-memory pipeline 模拟。
3. `esp32/components/espview`（UartTransport，M1-1）+ `esp32/components/testpattern` + `esp32/main`（流式测试图案发送，M1-3C）。
4. `pc/` — `HostUartTransport` + `com3_frame_test`（真实 COM3 全模式验收工具，含 `--selftest-queue`） + Qt 6 Virtual Display `espview_virtual_display`（`display_frame` / `serial_worker` / `virtual_screen_widget` / `connection_manager` / `main.cpp`，M2）。
5. `shared/input/`（InputEvent / InputManager / IInputListener / KeyboardMapper / CoordinateMapper / InputCodec / InputPolicy）+ `pc/src/input/`（InputController / QtKeyAdapter）+ `esp32/components/input/`（M3）——纯 C++17 输入桥：Qt 鼠标/键盘 → HID usage → INPUT_KEY / INPUT_MOUSE → ESP32 InputManager → IInputListener 日志与统计；验收工具 `pc/src/input_send_test.cpp`（真实 COM3 输入链路）、`pc/src/input_mapper_test.cpp`（host 映射单测）。
6. `scripts/` — 硬件验收脚本（`pc_com3_test.py` M1-1、`pc_com3_session_test.py` M1-2）+ host-only 验证入口 `verify_host.bat`（M1-3D，无需 COM3/ESP32） + Qt host 构建检查 `verify_qt.bat`（M2）+ PNG 像素校验 `verify_png_pixels.ps1`（M2）。
7. M4（运行态统计层，不改变协议）：`shared/protocol/runtime_stats.{h,cpp}`（Severity / DiagnosticEntry / DiagnosticsRing 50 / RttAggregate）+ `frame_assembler` FrameStats 分类统计 + `protocol_endpoint` SessionStats 心跳/Packet 错误分类 + **非阻塞 trySink**（tryTransmit 专用，大帧期间控制回复不阻塞 RX 任务）；`pc/src/serial_worker`（WorkerStats 4 域 + DiagnosticsRing + diagAdded 信号）、`connection_manager` 转发、`main.cpp` 4 域状态网格 + 诊断列表；ESP32 `reportInputStats()` 扩展 `sess/sess2` 统计行；验收工具 `input_send_test`（大帧输入压力 + reconnect 恢复）、`com3_frame_test`（心跳可观察输出）+ host 单测 `runtime_stats_test` / `protocol_endpoint_test.try_sink_used_for_control_replies`。

8. M5-A（LVGL Display Backend，wire format 未修改）：`shared/display/`（`IDisplay` / `DisplayManager` / `RemoteDisplay` + 帧统计，纯 C++17 零平台依赖，PC 与 ESP32 复用同一份源码）+ `esp32/components/display`（IDF 组件直接编译 shared/display 源码）+ `esp32/components/lvgl_port`（LVGL v8.4 display driver + flush_cb + TX 任务 + demo UI + 堆统计）+ `esp32/main/Kconfig` 应用选择（LVGL 默认 / TestPattern 可选）+ host 单测 `shared/display/tests/remote_display_test.cpp`（13 例：bounds / RGB565 / streaming / FULL 首帧 / PARTIAL / disconnect-FULL / backpressure / flush 生命周期 / DisplayManager 编译期模式 / in-memory pipeline 提交）+ `scripts/verify_lvgl.bat`（host 测试 + ESP32 构建 + 可选 COM3 sanity）+ `scripts/pc_com3_lvgl_sanity.py`。

9. M5-B（LVGL Input Device，wire format 未修改）：`shared/input/lvgl_adapter.{h,cpp}`（`LvglInputAdapter`，纯 C++17 无 LVGL 依赖：pointer 状态缓存 + 点击保持窗口 + key ring buffer + wheel accumulator + 统计）+ `shared/input/hid_lvgl_keymap.{h,cpp}`（HID usage → LVGL key code）+ `esp32/components/lvgl_port/src/lvgl_indev.cpp`（POINTER / KEYPAD / ENCODER 三个 `lv_indev_drv_t` read_cb）+ `esp32/components/lvgl_port/src/lvgl_app.cpp`（交互 demo UI：Button A/B + Counter + Keyboard/Mouse 标签 + group 导航）+ `esp32/main`（InputManager → LvglInputAdapter 接线 + `inp3` 统计行）+ host 单测 `shared/protocol/tests/lvgl_adapter_test.cpp`（§19 十五项 + 点击保持/集成扩展）。

10. M6-A（Wi-Fi STA + TCP Transport，wire format 未修改）：`esp32/components/espview/include/espview/wifi_sta.hpp` + `src/wifi_sta.cpp`（STA 初始化/连接/GOT_IP/自动重连，凭据仅经 Kconfig 注入）+ `include/espview/tcp_transport.hpp` + `src/tcp_transport.cpp`（link 任务 + RX 任务 + sendAll，ITransport 同构）+ `esp32/components/espview/Kconfig`（ESPVIEW_TRANSPORT choice + TCP/Wi-Fi 配置，Wi-Fi 凭据默认空、只在本机未跟踪 sdkconfig 填写）+ `esp32/main/main.cpp`（编译期 Transport 选择，上层完全透明）+ PC 侧 `pc/src/pc_transport.h`（IPcTransport 抽象）+ `pc/src/host_tcp_transport.{h,cpp}`（HostTcpTransport 客户端 + TcpListener 服务端，WinSock2）+ `pc/src/serial_transport.{h,cpp}`（HostUartTransport 收敛到 IPcTransport）+ `pc/src/serial_worker.{h,cpp}`（TransportKind kUart/kTcp，TCP Server 模式：bindListen → acceptOne → pump → re-accept）+ `pc/src/connection_manager` / `main.cpp`（--transport uart|tcp --tcp-bind --tcp-port CLI）+ host loopback 测试 `pc/src/tcp_transport_test.cpp`（§二十八 1-19）。

11. M7-A（独立 OLED 状态显示，wire format 未修改）：`shared/oled/`（`oled_fb` 1KB 页式 fb + 8×8 字体、`oled_cmd` SSD1306/SH1106 命令序列与上传分段生成，纯 C++17 零平台依赖）+ `esp32/components/oled/`（`oled_i2c` v6.0.2 新 I2C 驱动 `driver/i2c_master.h` 封装、`oled_controller` 控制器解析、`status_ui` 状态页渲染、`oled_display` 低优先级任务 + 有界错误恢复、Kconfig 默认 n）+ `esp32/main`（`oledStatusSnapshot` provider + 启停接线 + `oled` 诊断行，全部 `#if CONFIG_ESPVIEW_OLED_ENABLE` 保护）+ host 单测 `shared/oled/tests/oled_test.cpp`（1,553 checks）+ `scripts/pc_oled_monitor.py`（被动串口监控 `oled` 诊断行）。

下一步（M6）：真实 LCD（DEVICE/MIRROR）、触摸（INPUT_TOUCH）、TinyUSB、运行时 DisplayMode；输入侧扩展项：GUI 层 MouseMove 节流/合并在发端调优、ESP32 输入防抖、输入宏/录制/回放、游戏手柄、修饰键在 LVGL 侧的合成（当前为 capability limitation，见 S.7）。ESP32 侧 `IDisplay` / `RemoteDisplay` 属于 M2/M5 范围，本设计已为其预留接口（D 节）。

---

## Q. 测试分层与 Host Verification（M1-3D）

- **Host-only（普通 ctest / `verify_host.bat` / `verify_lvgl.bat`，无需 COM3 与 ESP32）**：
  - CRC / Packet / Encoder / Streaming Encoder / Decoder / FrameAssembler / ProtocolEndpoint / In-Memory Pipeline 单测（`shared/protocol/tests`）；
  - M5-A RemoteDisplay 单测（`shared/display/tests/remote_display_test.cpp`：writeRect bounds / RGB565 / streaming / FULL 首帧 / PARTIAL / disconnect-FULL / backpressure / flush 生命周期 / DisplayManager 编译期模式 / 真实 Encoder→Decoder→FrameAssembler pipeline 提交）；
  - M5-B LvglInputAdapter 单测（`shared/protocol/tests/lvgl_adapter_test.cpp`：mouse move / left press / left release / wheel / key down/up / rapid down-up / multiple events / reset / reconnect / invalid coordinates / pending event consumption / HID→LVGL mapping / no stuck key / no stuck mouse / RIGHT-MIDDLE recorded-not-consumed / InputManager→Adapter 集成 / 点击保持窗口语义 —— LVGL 无关，host 直接编译，不引入 lvgl.h）；
  - 测试工具自检 `com3_frame_test --selftest-queue`：ByteQueue 粘滞死循环回归（out 非空 + 并发 push + 连续 popAll，旧实现稳定失败、当前实现稳定通过）。
  - M6-A TCP loopback 测试（`pc/src/tcp_transport_test.cpp`，127.0.0.1，无真实 Wi-Fi）：Transport 语义（connect / disconnect / reconnect / partial recv / sticky recv / sendAll short write / remote close / timeout / invalid address / 多连接 BUSY 拒绝）+ Protocol integration（HELLO / PING-PONG / FULL 153600B / CHUNKED FULL / PARTIAL / CRC corruption / seq gap / FULL resync / reconnect resync，像素逐字节校验）。
- **Hardware（需真实 ESP32 + COM3，不作为普通 ctest 的硬依赖）**：
  - `pc/build/com3_frame_test --mode full-small|full-large|partial|corruption|seq-gap|reconnect`（C++ 真实帧管线验收，115200 baseline）；
  - `scripts/pc_com3_test.py`（M1-1 UART 字节流）、`scripts/pc_com3_session_test.py`（M1-2 会话）。
- **入口**：`scripts/verify_host.bat` = host-only 一键验证（构建 host 单测 + ctest + `--selftest-queue`）；`scripts/verify_lvgl.bat` = M5-A 验证（host 测试 + ESP32 idf.py 构建 + 可选 `ESPVIEW_COM3` 硬件 sanity）；硬件验收按需手动执行，不进入普通 CI。


---

## R. M5-A LVGL Display Backend 实现语义（冻结；wire format 未修改）

M5-A 只新增实现层，**不修改 Packet / Message / Frame 的 wire format**（含 CHUNKED / SEQ / CRC / 帧语义）。

### R.1 LVGL flush_cb 生命周期

- LVGL 一个 rendering cycle（`lv_timer_handler()` → render → `flush_cb(area, px_map)` 1..N 次）对应一个 ESPView Frame：`FRAME_BEGIN → FRAME_RECT×N → FRAME_END`。
- `px_map` 只在 flush_cb 内有效 → flush_cb 内**同步**把像素 staging 到 RemoteDisplay 队列槽（有界拷贝），之后不得再访问 px_map。
- 帧边界判定使用 LVGL v8.4 `lv_disp_flush_is_last(drv)`：同一 cycle 多次 flush 合成一帧；最后一次 flush 后调用 `RemoteDisplay::flush()`（TX 在队列排空后发 FRAME_END）。
- **无论 send 成功 / 失败 / 背压 / 整帧丢弃，都必须调用 `lv_disp_flush_ready()`**，否则 LVGL 永久停在当前 cycle。

### R.2 Streaming payload source

- RemoteDisplay 经 `IFrameSink::sendStreaming(MessageHeader, IMessagePayloadSource&)` 走 Streaming Encoder：FRAME_RECT 的 8 字节头 + 像素由 PayloadSource 按需产出，不构造整条 RECT 消息缓冲区。
- 协议一致性约束：Streaming Encoder 与普通 Encoder 同一 payload 时 wire bytes 等价；LVGL 只能充当 PayloadSource，不能改变 Message 语义。

### R.3 初次 FULL / 后续 PARTIAL

- Transport 首次 connected 后的第一帧必须是 FULL（PC 建立 committed base）；FULL 只是语义自包含，其 RECT 仍可拆为多个小矩形，不重建完整 framebuffer（连接后 `lv_obj_invalidate(lv_scr_act())` 全屏置脏触发）。
- 之后 LVGL dirty rect 经 `RemoteDisplay::writeRect()` 汇入，帧类型为 PARTIAL；不把每次刷新都发成整屏 FULL。

### R.4 Backpressure / 掉帧策略

- TX 队列固定 2 槽 × 15360B（1/10 屏 RGB565），`init()` 预分配；`writeRect()` 先非阻塞入队（满 → `kQueueFull`）。
- 队列满 / 上一帧未结束：flush_cb 有界等待（FULL 与 PARTIAL 帧预算均为 3000ms = 115200 节流，实测冻结值；PARTIAL 若按 100ms 预算，1Hz 小矩形（~26KB ≈ 2.3s wire 时间）会被误判背压丢弃 → 强制 FULL 雪崩，每次 FULL 又需 ~13s 传输，导致任何帧都无法提交），绝不无限阻塞 LVGL。
- 超时 → `dropPendingFrame()` 丢弃整帧 + 标记下一帧 FULL resync；TX 侧发 `FRAME_END(ABORTED)`，**不允许半帧被 PC 当成完整画面**。

### R.5 Reconnect FULL

- CONNECTED → `RemoteDisplay::onConnected()` + 全屏置脏（下一帧 FULL）；DISCONNECTED → `onDisconnected()`（清队列 + 复位帧状态）；重连后第一帧必须 FULL，与 M3/M4 语义一致。

### R.6 Draw buffer 内存模型

- LVGL draw buffer = 1/10 屏（320×24 RGB565 = 15360B），静态 `.bss`（不占堆，避免经典 ESP32 大连续堆分配）；单缓冲：LVGL 等 flush_ready → 自然节流。
- RemoteDisplay TX 队列 = 2 槽 × 15360B（≈30KB，预分配）；packet staging = 4096B（encoder 内部）；**无整屏 framebuffer**（Pre-M0 所有权模型：framebuffer 归 Application/LVGL，ESPView 不持有）。
- 堆验收：free heap before/after LVGL init、运行期最低 free heap、最大空闲块，经 `disp3` 统计行上报。

### R.7 Dirty rectangle 统计

- `DisplayStats`：framesFull / framesPartial / framesDropped、rects、fullPixelBytes / partialPixelBytes、queueFullEvents、lastFrame{Id,Type,RectCount,Bytes,ElapsedMs}。
- 经 ERROR 文本通道（≤64B/行，不污染协议 UART）上报（值 clamp ≤999999 保证行宽）：
  - `disp id= frameType= rects= bytes= elapsedMs= fps= d=drop q=queueFull`
  - `disp2 full= part= dirty%`（dirty ratio = partialBytes / (320×240×2)）
  - `disp3 hb= ha= hm= lb= dw= qb=`（heap / draw buffer / TX 队列字节）

### R.8 LVGL 版本与配置

- component manager 解析 `lvgl/lvgl: ^8.3.0`，实际获取 **LVGL v8.4.0**（v8 API：`lv_disp_drv_t` / `lv_disp_flush_ready` / `lv_disp_flush_is_last`，与本设计假设的 v8.3 API 兼容）。
- `LV_CONF_SKIP=y`（Kconfig 配置）：`LV_COLOR_DEPTH=16`（RGB565）、`LV_FONT_MONTSERRAT_14` 默认启用、`LV_MEM_SIZE_KILOBYTES=32`。
- tick：UI 任务实测 delta 调 `lv_tick_inc()`（不使用 `LV_TICK_CUSTOM`，避免 ESP-IDF 端 sys-time-expr 配置缺口）。

### R.9 线程模型（ESP32）

- `lvgl_ui`（prio 4）：`lv_timer_handler()` + tick 推进 + 全屏置脏 + 运行期最低堆采样；
- `lvgl_tx`（prio 4）：`RemoteDisplay::pump()` 阻塞发送（每 RECT 持有 endpoint sendMutex ≈1.4s @115200），发送完成释放槽位 → 唤醒 flush_cb 等待者；
- 会话状态（sessionLoop / RX 任务）→ `onSessionState()` 只置原子标志 + RemoteDisplay 状态，绝不在 LVGL 任务之外触碰 `lv_obj`（线程安全）。

### R.10 应用选择（编译期）

- `esp32/main/Kconfig`：`ESPVIEW_APP_LVGL`（默认，M5-A）/ `ESPVIEW_APP_TESTPATTERN`（M1-3C/M2/M3/M4 硬件回归工具依赖）；**不引入 runtime DisplayMode**（v0.1 保持编译期模式，M6 实现运行时切换）。

### R.11 验收工具

- `scripts/verify_lvgl.bat`：host 测试 + ESP32 `idf.py build` + 可选 COM3 sanity（`ESPVIEW_COM3` 环境变量）；普通 ctest 不要求 ESP32 / COM3 / Qt。
- `scripts/pc_com3_lvgl_sanity.py`：COM3 解码 HELLO + FRAME_BEGIN/RECT/END（含 CHUNKED 重组），断言首帧 FULL、存在 PARTIAL、CRC/帧一致性，输出 dirty ratio。
- **心跳要求（实测根因）**：PC 必须主动周期发 PING（脚本 `--ping-interval` 默认 1s）。长流式帧（首次 FULL ≈13s）期间 ESP32 的 PING 会因 sendMutex 被流式发送持续占用而被放弃（M4 已知行为），若 PC 仅被动回 PONG，ESP32 5s peer timeout 会在 FULL 中途断开会话 → 帧流停止；PC 自发的 PING 维持会话（DESIGN.md M4：大帧期间 PC 发 PING，timeouts=0）。

## S. M5-B LVGL Input Device 实现语义（冻结；wire format 未修改）

M5-B 只新增实现层，**不修改 INPUT_KEY / INPUT_MOUSE wire format、Packet Header、CRC、CHUNKED、帧语义**。完整输入链：

```
Qt VirtualScreenWidget → InputController → INPUT_* → HostUartTransport → COM3
  → ESP32 UartTransport → ProtocolEndpoint → InputManager → LvglInputAdapter（LVGL 无关状态）
  → lv_indev read_cb（POINTER / KEYPAD / ENCODER，LVGL 任务轮询消费）→ LVGL Application
```

### S.1 架构与依赖方向

- `InputManager`（shared/input）**不依赖 LVGL**；LVGL 适配器由两块组成：
  - `shared/input/lvgl_adapter.{h,cpp}`：`LvglInputAdapter`（纯 C++17，**不包含 lvgl.h**），实现 `IInputListener`，只维护可轮询状态（pointer x/y/pressed、key ring、wheel accumulator）与统计；
  - `esp32/components/lvgl_port/src/lvgl_indev.cpp`：薄胶水，把 adapter 状态接到 LVGL v8.4 `lv_indev_drv_t` 三个 read_cb（POINTER / KEYPAD / ENCODER）。
- 依赖方向唯一：**LVGL Adapter（lvgl_indev）依赖 shared/input**，shared/input 永远不反向依赖 LVGL。
- 驱动结构必须长期存活（`static lv_indev_drv_t`）：`lv_indev_drv_register()` 之后 indev->driver 指向调用者结构，栈局部会在首个 read timer 周期读到悬垂指针（v8 常见误区，实测崩溃点）。

### S.2 Pointer 轮询模型

- RX 任务收到 MouseMove / MouseDown / MouseUp → `onInputEvent()` 只更新 adapter 的 x/y/leftPressed（内部互斥）；LVGL 任务 `pointerReadCb` 调 `pointerState()` 拿瞬时快照 → `data->point` + `data->state`。
- **点击保持窗口**（`kPointerClickHoldReads = 2`）：LeftUp 后不立即释放，冻结点击点并保持 PRESSED 若干次 read_cb，再真正 RELEASED —— 保证 LVGL ~30ms 轮询至少观察到一次 PRESSED，Down+Up 落在两次轮询之间也不会漏点击（LV_EVENT_CLICKED 稳定触发）。保持期间 Move 只更新坐标，返回冻结点击点。
- 坐标由 InputManager 在协议层完成换算/校验；adapter 做第二层防御校验（越界 → invalidEvents 计数并忽略，不 crash LVGL）。

### S.3 Keyboard 轮询模型

- HID usage → `HidToLvglKeyMapper` → LVGL key code，进入 32 项有界 ring buffer；`keypadReadCb` 每次消费一项并置 `continue_reading`（有剩余时 LVGL 在同一 read timer cycle 内立即再次调用 read_cb，Down/Up 快速序列按序处理）。
- ring 满时丢**最新**项：丢 KeyDown 不会造成 stuck key（后续 KeyUp 到达时 LVGL prev_state 为 RELEASED 会被跳过）。
- 字母/数字/标点映射到 ASCII；Enter/Esc/Backspace/Tab/Space/Home/End/Delete/方向键/Keypad 映射到 LVGL `LV_KEY_*` 或 ASCII。

### S.4 Wheel → ENCODER 语义

- 滚轮格数在 adapter 累加（`wheelAccum_`，int32 防溢出），`encoderReadCb` 调 `consumeWheelDiff()` 取走并 clamp 到 int16 → `data->enc_diff`，`state` 恒 RELEASED —— LVGL v8.4 `indev_encoder_proc` 只在 RELEASED 时处理 enc_diff（PRESSED 时强制 0）。
- 取走的步数从累计中扣除，余量保留给下一次 read_cb（不丢步数）；+1/-1/+2/-2 原样到达。
- ENCODER 与 KEYPAD 都绑定 demo group（enc_diff 需要 group 才能进入编辑/导航语义；demo UI 开启 `lv_group_set_editing`，使 Counter 聚焦时滚轮增减计数）。

### S.5 线程边界（关键）

- **RX 任务**（InputManager::feed 在自身锁内调用 listener）→ `onInputEvent()`：只在本 adapter 内部互斥下更新状态，**绝不调用任何 lv_\* API**；
- **LVGL 任务** → `pointerState()` / `nextKeyEvent()` / `consumeWheelDiff()`：同样互斥、非阻塞，供 read_cb 轮询消费；
- 两侧短临界区，不持有协议锁，不阻塞 LVGL / TX / Display；断线/重连状态由会话任务置标志，adapter 清空操作自带互斥。

### S.6 LEFT / RIGHT / MIDDLE 映射策略

- LEFT → LVGL pointer PRESSED/RELEASED（唯一 primary 语义）；
- RIGHT / MIDDLE → adapter 记录（ignoredButtons 统计）但**不消费**：LVGL v8.4 pointer indev 只有单一 primary pressed 状态，无法表达第二/第三按钮 —— 记录为 capability limitation，不改协议。

### S.7 HID → LVGL key 映射与能力边界

- 已映射：A–Z、0–9、空格、标点、Enter/Esc/Backspace/Tab/方向键/Home/End/Delete/Keypad（`shared/input/hid_lvgl_keymap.cpp` 线性表，LVGL v8.4 `LV_KEY_*` 常量硬编码为 plain 常量，host 测试不引入 lvgl.h）。
- **不映射（unmappedKeys 计数，不 crash）**：修饰键 0xE0..0xE7（Ctrl/Shift/Alt/GUI —— 修饰状态已表达在 modifiers 掩码，LVGL 侧不合成修饰键事件）、F1–F12、PageUp/PageDown、Insert、CapsLock、NumLock、ScrollLock 等 LVGL v8 无对应概念的能力。
- 组合键（Ctrl+A / Shift+A）在 PC 侧是独立 InputEvent 序列（Ctrl Down → A Down → A Up → Ctrl Up）；到 LVGL 只有 'A' 键事件，修饰状态被 adapter 丢弃 —— 记录为 capability limitation。

### S.8 事件消费语义与重置

- 消费即取走：key ring 逐项、wheel 累计取走并保留余量、pointer 状态为最新值；`peekPointer()` 供 UI 标签只读（不推进点击保持窗口）。
- 断线/会话重置：main 先调 `InputManager::resetState()`（本地补发 KeyUp/MouseUp，**绝不回发 PC**，spec §18），再调 `LvglPort::onSessionState(kDisconnected)` → `inputAdapter_->reset()`（清 pointer/key/wheel）；两者顺序无关，最终无 stuck key / stuck mouse。

### S.9 可观察性与延迟

- `LvglAdapterStats` 经 ERROR 文本通道上报（≤64B/行，wire format 未修改）：`inp3 w= wheelEvents s=wheelSteps(累计带符号) k=consumedKeys d=keyQueueDropped u=unmappedKeys b=ignoredButtons r=resets`。
- 延迟语义：ESP32 侧记录的是 **ESP32 local scheduler latency**（input 收到 → LVGL read_cb 消费），不是网络 RTT；实测 FULL 帧（153600B ≈13.2s @115200）传输期间，LVGL 任务阻塞在 flush_cb 有界等待，read_cb 被同样背压延迟 —— 输入在 InputManager/adapter 层**不丢失**（实测 27/27 事件到达、0 dropped），消费延后到帧传输完成后（实测 FULL 完成后 k=4 按键被消费）。这是 M5-A 既有 backpressure 模型的自然延伸，不是协议缺陷，不在 M5-B 重新设计。

### S.10 M5-B 实测（真实硬件 COM3 @ 115200）

- 输入链路：鼠标 move/left down/up/right click/middle click、wheel +1/-1、A/B/Enter/方向键、Ctrl+A、Shift+A —— 27 事件全收（rx=27, inv=0, u=0, dropped=0），无 stuck state；
- LVGL 消费：FULL 帧完成后 consumedKeys=4（A down/up + Enter down/up），wheel 事件 w=2 入账；
- reconnect：按住 Ctrl + Left 断开 → 重连后 `inp2 r=1 sk=1 sb=1`，pressed keys=0 / buttons=0（PASS）；
- display 回归：FULL=153600B/10 rects，PARTIAL=6392B/1 rect（dirty=4.16%），0 CRC / 0 mismatch；
- host：208777 checks / 0 failures，ctest 1/1；`verify_host` / `verify_qt` / `verify_lvgl` 全部 PASS。

---

## T. M6-A Wi-Fi/TCP implementation semantics（冻结；wire format 未修改）

规范来源：M6-A 任务书 §一~§四十。目标不是"加 Wi-Fi 功能"，而是验证 **Protocol ≠ Transport**：
UART 与 TCP 使用同一套 `ProtocolEndpoint / MessageEncoder / StreamDecoder / FrameAssembler /
InputManager / RemoteDisplay`；TCP 不新增任何 Message / Packet / CRC 变体，wire format 完全不变。

### T.1 拓扑与 Transport 选择

- PC = TCP Server（监听 `0.0.0.0:8765`，CLI `--tcp-bind` / `--tcp-port` 可改）；ESP32 = Wi-Fi STA + TCP Client（`CONFIG_ESPVIEW_TCP_SERVER_IP` / `CONFIG_ESPVIEW_TCP_SERVER_PORT`）。
- 编译期 Transport 选择（Kconfig `ESPVIEW_TRANSPORT` choice，默认 UART）：`ESPVIEW_TRANSPORT_UART` / `ESPVIEW_TRANSPORT_TCP`；`esp32/main` 只面对 `ITransport`，应用（LVGL / TestPattern）完全不知道 Transport 类型。
- 接口为未来 runtime switching 预留（ESP32 `ITransport` 与 PC `IPcTransport` 同构），M6-A 不实现运行时切换。

### T.2 状态分离（任务书 §四）

- **Wi-Fi connected ≠ TCP connected ≠ Protocol connected**，三套状态严格分离：
  - `WifiSta`：DISCONNECTED → WIFI_CONNECTING（STA_START）→ WIFI_CONNECTED（STA_CONNECTED）→ GOT_IP（IP_EVENT_STA_GOT_IP）；STA_DISCONNECTED 事件驱动自动重连（有界退避由 link task 侧超时兜底）。
  - `TcpTransport`：TCP_CONNECTING（非阻塞 connect + select 超时）→ TCP_CONNECTED（可写 + SO_ERROR=0）→ TCP_DISCONNECTED（recv=0 / 错误 / 发送失败）。
  - `ProtocolEndpoint`：HELLO 互换 → HANDSHAKE → CONNECTED；每次会话建立/重连，seq / decoder / frame / ACK / input state 全部清零。

### T.3 ESP32 侧组件

- `esp32/components/espview`：
  - `WifiSta`（wifi_sta.hpp/cpp）：`esp_netif_init` / `esp_event_loop_create_default` / `esp_netif_create_default_wifi_sta` / `esp_wifi_init`（`WIFI_INIT_CONFIG_DEFAULT`）/ `esp_wifi_set_mode(STA)` / `esp_wifi_set_config` / `esp_wifi_start`；事件 handler 只更新标志 + 唤醒等待者；日志只打印 SSID 长度，**绝不打印 SSID/密码**。
  - `TcpTransport`（tcp_transport.hpp/cpp，`ITransport` 同构）：link 任务（Wi-Fi 等待 → connect → 启动 RX 任务 → 断开退避重连）+ RX 任务（select 200ms 轮询 → recv → dataCallback）+ `send()` sendAll（处理 short write；SO_SNDTIMEO 限时；EAGAIN→超时，致命错误→断开并唤醒 link）+ `close()`（置停止 → 唤醒 link → join → 关 fd）；所有 fd 关闭在 `sockMutex_` 内（shutdown + close），无 double close / use-after-close / deadlock（§二十二）。
- lwIP 兼容调用（`lwip_socket` / `lwip_connect` / `lwip_select` / `lwip_send` / `lwip_recv` / `lwip_fcntl` / `lwip_inet_pton`），符合 ESP-IDF v6.0.2 当前 API。
- 依赖（组件 REQUIRES）：`esp_wifi esp_netif esp_event nvs_flash lwip` 等；Wi-Fi 凭据经 Kconfig 注入（见 T.7）。

### T.4 PC 侧组件

- `pc/src/pc_transport.h`：`IPcTransport` 抽象（open / close / send / isConnected / dataCallback / stateCallback / mtu / rxBytes / txBytes）；`HostUartTransport`（M1-3B）与 `HostTcpTransport`（M6-A）为同构实现，`WorkerStats` / `DisplayFrame` / 输入队列对 Transport 类型透明。
- `pc/src/host_tcp_transport.{h,cpp}`：WinSock2（`WSAStartup` 一次性引用计数初始化）：
  - `HostTcpTransport`：客户端（getaddrinfo → 非阻塞 connect + select 超时 → 阻塞模式 + `SO_SNDTIMEO` + `TCP_NODELAY`；RX 线程 select(100ms) → recv → dataCallback；`send()` sendAll；`close()` shutdown 唤醒 + join）。
  - `TcpListener`：服务端（socket / bind / listen / acceptOne）；**单客户端**（§九）：已有活跃连接时 `acceptOne` 返回 BUSY；前一个客户端断开后自动允许重新 accept（§十一 PC 重连路径）；`cancel()` 只置标志（worker 线程随后 close），避免跨线程 closesocket/select 竞态；bind 失败报告 WinSock error（§二十七），不自动修改 Windows Firewall。
- `pc/src/serial_worker.{h,cpp}`：`TransportKind { kUart, kTcp }`；`startTcp(port, bind)` 进入 TCP Server 模式（`bindListen` → `acceptOne` → `pumpLoop` → 断开 → re-accept）；握手策略沿用 UART 的被动 HELLO（等 ESP32 boot HELLO，7s 超时主动发起）；`stop()` 先 `listener_.cancel()` 再 join。
- GUI（§三十三）：`ConnectionManager::startTcp`；CLI `--transport uart|tcp`、`--tcp-bind <ip>`、`--tcp-port <port>`；状态面板显示 `TCP <bind>:<port>`（UART 显示 `COMx` / baud）；`VirtualScreenWidget` / `InputController` 完全不变。

### T.5 TCP 是 BYTE STREAM（§七 / §二十四）

- 一次 `recv()` 可能得到**半个 Packet** 或**多个 Packet**：Transport 只原样转发 `dataCallback`，**不在 Transport 内切 Packet**；半包 / 粘包 / CRC / 重同步全部由 `StreamDecoder` 处理（与 UART 共用同一套代码，M0-C 冻结）。
- 接收 chunk 固定 4096B，不无限增长；不缓存/拼接（上层 decoder 自带缓冲）。
- `sendAll`（§二十三）：Transport 层循环处理 short write，直到全部发送或 error / timeout / disconnect；`ProtocolEndpoint` 不感知 TCP short write。

### T.6 重连与 FULL resync（§十一 / §三十二）

- ESP32：TCP disconnected → 退避（`CONFIG_ESPVIEW_TCP_RECONNECT_DELAY_MS`，默认 3000ms）→ 重新 connect → 会话层 HELLO → 全状态清零 → 等新 FULL。
- PC：client disconnect → 等待 → `acceptOne` 接受新 ESP32 → HELLO → FULL；Qt 无需重启。
- 每次 Protocol session 建立：decoder reset / frame reset / ACK reset / seq reset / input state reset（`InputManager::resetState()` 本地补发 release，**绝不回发 PC**）。

### T.7 安全与凭据（§三十七）

- Wi-Fi 凭据只存在于**本机未跟踪配置** `esp32/sdkconfig`（`.gitignore` 已覆盖）；`sdkconfig.defaults` 不含凭据；`CONFIG_ESPVIEW_WIFI_SSID/PASSWORD` 默认空。
- 代码 / 日志 / 错误信息**绝不打印 SSID/密码**（只打印 SSID 长度）；日志通道与协议 Transport 隔离。
- 当前测试网络为本地开发环境（"development LAN credentials supplied locally"），不得把真实凭据写入 source / DESIGN.md / git / 日志。

### T.8 性能对照（§三十五）

- 115200 UART 为 M6-A 对照 baseline；Wi-Fi TCP 下记录 FULL 153600B 的 elapsed / payload throughput / RTT / 帧 FPS / input responsiveness，与 UART 对照**找新瓶颈**（不追求理论最大值，不硬编码 MB/s 验收线）。

### T.9 Wi-Fi power save（§二十五）

- 默认保持 ESP-IDF power save（baseline）；`CONFIG_ESPVIEW_WIFI_PS_NONE`（默认 n）为实验性延迟优化，不作为生产默认；实测数据区分 baseline vs experimental。

### T.10 真实硬件验收记录（M6-A 实测，2026-08-14，COM4）

硬件与链路：

- ESP32-D0WDQ6（4MB flash；M6-A 时 app 分区 1MB，M6-B 扩为 2MB，LVGL 应用固件）；板载 USB-SERIAL CH340 接 **COM4**（console 115200 8N1，DTR/RTS 关闭只读）；PC 有线网 192.168.3.15。
- Wi-Fi：本地开发网络（凭据仅存于未跟踪本地 sdkconfig，T.7）。STA 扫描实测 17 个 AP（含 5GHz）；目标网络存在（2.4GHz ch6，RSSI ≈ -29 dBm，WPA2-PSK），连接成功并经 DHCP 取得 GOT_IP 192.168.3.128。
- TCP：ESP32 STA client → PC TCP server `0.0.0.0:8765`；Windows 防火墙入站规则仅放行 `build\verify_qt\espview_virtual_display.exe`（Private/Public），GUI 必须从该路径启动（否则 0xC0000139 / 连接被拦）。

验收结果：

1. Wi-Fi connect / DHCP GOT_IP ✅（扫描 → 认证 → GOT_IP 全链路日志确认）。
2. TCP connect / HELLO ✅（`session CONNECTED (HELLO done)`，双向 HELLO，`errors=0 decErr=0`）。
3. LVGL FULL ✅（320x240 RGB565，10 rects，153600B；PC 端 1:1 像素还原，PNG 证据 `build/m6a_png/dump7/full_4.png` 等）。
4. LVGL PARTIAL ✅（输入驱动 dirty rect 局部刷新；点击/按键/计数器区域均按 rect 增量提交）。
5. 输入键盘 / 鼠标 / 滚轮 ✅（SendInput 实测：点击 Button A 变橙 #215194→#F74131；键盘 'A' 更新 Keyboard 标签；Counter 点击聚焦 + RIGHT 方向键数值变化；断线重连后会话状态保持——会话权威在 ESP32 侧）。
6. TCP 断开重连 + FULL resync ✅（kill GUI → ESP32 退避重连（3s）→ 新会话 HELLO → 新 FULL；PC 无需重启，GUI 自动 re-accept）。
7. 153600B FULL throughput：elapsed ≈ 337–350ms（ESP32 帧统计 `disp e=`，两轮实测），payload ≈ 440–460 KB/s；RTT：ESP32 侧 9ms；PC 侧 min 11–48ms / avg 40–91ms / max ≈ 125ms（默认 power save 下逐会话波动，T.9）。
8. UART vs TCP 对照：115200 UART 理论 ≈ 13.3s/FULL（11.5 KB/s）→ TCP ≈ 39× 更快；FULL 期间 `q=26–100`（TX 队列满事件），当前瓶颈在 ESP32 编码/发送管线（LVGL flush 节流为 UART 设计），不在 Wi-Fi 空中接口。
9. AP 丢失重连：STA_DISCONNECTED → `esp_wifi_connect()` 自动重连 + link 任务 phase-1 等待已实现；**真实 AP 断电重连未做硬件验证**（需操作路由器；本机仅验证 TCP 断线路径）。
10. 修复记录：
    - `TcpTransport::open()` 的 `xTaskCreate(link)` 返回值检查错误（`esp_err_t/ESP_OK` vs `BaseType_t/pdPASS`）→ open 误报 `ESP_ERR_NO_MEM` 且 `sessionLoop/statsLoop` 不启动（症状：TCP 已连接但 Session 卡 CONNECTING）→ 已改为 `terr != pdPASS` 并回归。
    - `WifiSta::init()` 曾缺 `nvs_flash_init()` → `esp_wifi_init: ESP_ERR_NVS_NOT_INITIALIZED` → 已补（含 erase-retry）。
    - NVS 异常（非干净复位/写坏 PHY 校准）可致 `esp_wifi_init/start` 挂起 → 擦除 NVS 分区后恢复。
11. 固件体积：TCP 固件 `espview_esp32.bin` = 1,027,488 B（app 分区 1MB，仅剩 ≈2%）；UART 固件 ≈ 406,064 B。Wi-Fi/TCP 栈显著增大镜像；M6-B 已通过 2 MiB factory 分区解决容量问题（见 U 章）。

## U. M6-B Flash / Partition / TCP 性能收尾（2026-08-14 实测；wire format 未修改）

### U.1 Flash 容量与分区表（最终）

- 开发板 Flash：**4 MiB**（esptool `Detected flash size: 4MB`；M6-A 文档曾误写 2MB，已在本章更正）。
- 使用 **custom partition table**（`esp32/partitions.csv`，ESP-IDF 6.0.2），**single factory app、no OTA**：

| Name     | Type | SubType | Offset  | Size            | 说明 |
|----------|------|---------|---------|-----------------|------|
| nvs      | data | nvs     | 0x9000  | 0x6000（24 KiB）| 历史位置保留，Wi-Fi/NVS 校准数据不迁移 |
| phy_init | data | phy     | 0xf000  | 0x1000（4 KiB） | 历史位置保留 |
| factory  | app  | factory | 0x10000 | 0x200000（2 MiB）| 生产 App（原 1 MiB 扩大） |

- Bootloader 0x1000–0x8000、partition table 0x8000–0x9000（与 ESP-IDF 默认对齐不变）。
- **未分配 data 空间**：0x210000–0x400000 ≈ 0x1F0000 ≈ **1.94 MiB 保留**（供后续算法数据使用；本阶段不预切 FATFS/SPIFFS）。
- 未来若需要 OTA：必须重新设计 partition table（4 MiB 下典型为双 1.7 MiB OTA slot + ota_data 布局），**不得假设 2 MiB app + 双 OTA 可无条件共存**。

### U.2 固件体积与余量（实测）

- `espview_esp32.bin` = **1,027,488 B**（0xFADA0）。
- App 分区 2 MiB（0x200000），剩余 **0x105260 ≈ 1,069,664 B ≈ 1.02 MiB（51%）**。
- 镜像体积与 M6-A 基本一致：Wi-Fi power-save 选项只影响运行时行为，几乎不增加镜像大小。

### U.3 烧录与启动（实测）

- CH340 在 460800 下不可靠（esptool `Failed to read target memory`）；**烧录统一使用 `idf.py -p COM4 -b 115200 flash`**（1,027,488 B / 59.8 s，写后 hash 校验通过）。
- Boot 日志：`SPI Flash Size : 4MB`、`factory factory app 00 00 00010000 00200000`；app 完整落在 2 MiB 分区内，正常启动。
- 分区修改后全链路复验：Session CONNECTED（HELLO）✅、FULL ✅、PARTIAL ✅、输入点击 ✅、TCP 断线重连 + FULL resync ✅。

### U.4 TCP 性能 5 轮实测（默认 power save；320×240 RGB565 FULL = 153600 B）

| run | FULL elapsed (ms) | RTT avg (min/max) ms | TX queue full q= |
|-----|-------------------|----------------------|------------------|
| 1   | 373               | 90 (46/127)          | 218              |
| 2   | 466               | 63 (15/104)          | 256              |
| 3   | 716               | 107 (45/159)         | 313              |
| 4   | 628               | 86 (33/128)          | 362              |
| 5   | 689               | 84 (49/125)          | 423              |

- FULL elapsed：**min 373 / avg 574 / max 716 ms**；payload 吞吐：**min 214.5 / avg 267.4 / max 411.8 KB/s**。
- PARTIAL 5 次（默认 power save，点击 Button A）：e=70/70/123/26/26 → **min 26 / avg 63 / max 123 ms**。
- 连续 FULL 时 `q=`（TX 队列满事件）逐次累积（218→423）：TX 队列仍按 UART 节流设计，TCP 下 FULL 帧期间背压成为新瓶颈（见 U.6）。
- 波动来源：默认 Wi-Fi power save 的 RTT 波动（min 15 / max 159 ms）传导到 FULL elapsed。

### U.5 Power-save 对比实验（experimental，非生产默认）

| 指标 | default power save | WIFI_PS_NONE（experimental） |
|------|--------------------|------------------------------|
| FULL elapsed min/avg/max | 373 / 574 / 716 ms | 369 / 398 / 438 ms |
| FULL 吞吐 min/avg/max | 214.5 / 267.4 / 411.8 KB/s | 350.7 / 386.3 / 416.3 KB/s |
| RTT min/avg/max | 15 / 86 / 159 ms | 11 / 18 / 34 ms |
| PARTIAL min/avg/max | 26 / 63 / 123 ms | 32 / 32 / 32 ms（3 次有效样本） |
| q= 累积（5 轮） | 218 → 423 | 63 → 193 |

- `CONFIG_ESPVIEW_WIFI_PS_NONE`（Kconfig，默认 n）为实验性延迟优化：关闭 power save 后 RTT 明显更稳（avg 86 → 18 ms）、FULL elapsed 波动大幅收窄；**不作为生产默认**（任务书 §二十五 / T.9 延续）。
- 不因 RTT 波动修改协议；wire format 零改动。

### U.6 新发现：TX 队列背压（q= 累积）

- TCP 下每轮 FULL 期间 TX 队列满事件逐次累积（默认 218→423，PS_NONE 63→193），说明当前发送 pacing 仍按 115200 UART 节流设计，TCP 链路未被充分利用。
- 结论：M6-C 需要 runtime transport selection 与 TCP 适配的 TX pacing（不改变 wire format）。

### U.7 对照：UART vs TCP（全部实测）

| Transport | FULL 153600 B | 吞吐 |
|-----------|---------------|------|
| UART 115200（历史 baseline） | ≈ 13.5 s | ≈ 11.4 KB/s |
| TCP（默认 power save，5 轮） | min 373 / avg 574 / max 716 ms | min 214.5 / avg 267.4 / max 411.8 KB/s |

- TCP 比 UART 快约 **19–36×**（按 min/avg/max elapsed 对照，不使用理论速度）。

### U.8 边界声明（本阶段不做 / deferred）

- **AP 断电测试 intentionally deferred**：不列入 M6-B 验收红线；保留并回归已实现的 TCP disconnect / ESP32 退避重连 / PC restart / FULL resync 路径（M6-A 已验）。
- 不接 LCD / 触摸；不实现 HardwareDisplay / Mirror 实机验证；不增加复杂 Wi-Fi 状态机 / mDNS / watchdog workaround。
- **不修改 wire format**：Packet Header / CRC / Message / CHUNKED / Frame 语义 / INPUT_KEY / INPUT_MOUSE / MAX_PACKET_PAYLOAD 全部不变。
- Wi-Fi 凭据：只存在于未跟踪本地 `esp32/sdkconfig`，不写入本文件 / source / git / 日志（T.7 延续）。

### U.9 证据文件（build/m6a_png/）

- `perf_results.txt`（默认 power save：5 轮 FULL + RTT）
- `partial_results.txt`（默认 power save：PARTIAL 5 次）
- `psnone_results.txt` / `psnone_partial_results.txt`（PS_NONE：5 轮 FULL + RTT + PARTIAL）
- `perf1-5/full_*.png`（FULL 帧像素证据）、`gui_m6b.png`（GUI 内容区）、`input/step0-2*.png`（输入链路证据）


---

## V. M6-C Runtime Transport Selection + Transport-aware TX Pacing（2026-08-14 实测；wire format 未修改）

### V.1 架构目标与 TransportManager

- 目标（任务书 §一）：Application / LVGL / RemoteDisplay / ProtocolEndpoint 完全不感知当前是 UART 还是 TCP；
  Transport 决定自身能力，Protocol 不决定 Transport。禁止 `if (uart) / if (tcp)` 泄漏到 Protocol / RemoteDisplay。
- 新增 `shared/transport`：`ITransport`（open/close/isConnected/send/mtu + data/state 回调）、`TransportCapabilities`
  （`paced` 布尔：UART=true / TCP=false）、`TransportManager`（factory 创建、open/close/switchTo/reopen、
  switch 窗口内状态缓冲后重放、`lockTransport()/tryLockTransport()` 发送门串行化）、`TransportSink`
  （paced → 背压重试至 30s 预算；unpaced → 单次尝试，send 内部 sendAll 已按 socket 背压）。
- ESP32 侧适配：`esp32/components/espview/src/{uart_transport,tcp_transport,transport_manager}.cpp`
  （Esp32UartAdapter / Esp32TcpAdapter + uartCaps/tcpCaps/mapState/mapSend）；`main.cpp` 编译期 initial 选择
  （menuconfig `ESPVIEW_TRANSPORT_TCP`），运行时经 `g_mgr.switchTo()` 切换。

### V.2 Runtime Switch 语义（safe switch，非热插拔）

- `switchTo(type)` 按 Disconnected → Connecting/Connected 顺序重放状态回调：上层完成会话重置
  （decoder / FrameAssembler / pendingACK / seq / InputManager / partial base 全部清零），随后 HELLO 握手，
  CONNECTED 后 FULL resync（§五/§六）。PARTIAL 无 committed base 仍按既有语义拒绝。
- 旧会话的 seq / frame / ACK / partial base / input state 不跨 Transport 携带。
- test-only 验收钩子：F12（HID 0x45）经输入通道触发 `g_debugSwitchPending` → sessionLoop 执行
  `debugTransportSwitch()`，切换后经 ERROR 文本通道上报 `trx sw=<type> ok=<0|1> n=<switchCount>`。
- PC GUI：状态面板显示 Transport 类型（UART/TCP）、Session、Reconnects 计数与 TCP Peer 地址；
  `--no-reset`（test-only）跳过 UART DTR/RTS 复位脉冲，供运行时切换验收（避免打开 COM4 复位 ESP32）。

### V.3 Transport-aware TX Pacing（LVGL flush 预算）

- `onTransportState(kConnected)` → `updateFlushWaitFromTransport()`：paced（UART 115200）→
  `CONFIG_ESPVIEW_LVGL_FLUSH_WAIT_MS`（3000ms，M5-A 基线）；unpaced（TCP）→ 250ms。
- 效果：UART 继续保持 115200 安全节流（~13.2s/FULL）；TCP 不再继承 UART 固定节流（~250ms/FULL），
  LVGL flush_cb 背压等待超时后仍走既有整帧丢弃 + FULL resync。

### V.4 有界 TX 队列与掉帧策略

- RemoteDisplay 队列槽位有界（M5-A 既定）；背压事件 `q=`（queueFull）与队列峰值 `qp=`（queuePeak）经
  ERROR 文本通道上报（disp/disp3 行）。
- 实测：UART 全帧 pacing 期间 `q≈1055`（预期背压）、`qp=2`、`d=0`（无掉帧）；TCP 全帧 `q=16→206`
  （跨 run 的累积计数样式，非队列无限增长）、`qp=2`、`d=0`。队列有界，不随运行时间无限增长。
- 超时仍丢弃整帧（dropPendingFrame → ABORTED END → 下一次 FULL resync），wire format 未修改。

### V.5 控制流量公平性

- PONG / ACK / PING / ACK 重试走 `trySink`：单次尽力（门忙/缓冲满立即返回背压），绝不进入 UART 式重试循环，
  避免阻塞 RX 线程 / 会话 tick 饿死（M4 既有约束延续）；统计上报在独立低优先级任务（statsLoop，3s 周期）。

### V.6 Host tests（新增，全部并入 host 套件）

- `shared/transport/tests/transport_manager_test.cpp`：select UART / select TCP / switch UART→TCP /
  switch TCP→UART / switch while disconnected / switch failure / session reset / FULL resync / input state reset。
- `transport_sink_test.cpp`：UART policy / TCP policy / bounded queue / queue full / frame drop /
  control traffic during display load。
- `transport_pipeline_test.cpp`：in-memory byte pipe 管线（Encoder → Pipe → Decoder → FrameAssembler）。
- 回归基线：host 总计 **208910 checks / 0 failures**（ctest 1/1），`verify_host.bat` / `verify_qt.bat` /
  `verify_lvgl.bat` 全部通过，clean build 无 warning。

### V.7 实测：UART baseline（M6-C §26，COM4 CH340 @ 115200，LVGL app）

- FULL 153600B × 3：e=13182 / 13182 / 13260 ms（min/avg/max = 13.182 / 13.208 / 13.260 s）；
  有效吞吐 ≈ 11.6 KB/s（含 header 开销 93.1 kbit/s）；`q≈1055`、`qp=2`、`d=0`。
- 结论：transport 抽象未引入 UART pacing 回归（M6-B baseline ≈13.5s 同量级）。

### V.8 实测：TCP FULL（M6-C §25/§二十九，默认 power save）

- 5 轮 FULL 153600B：e=235–254 ms（UI 侧测得）；`qp=2`、`d=0`；queue 有界；
  RTT min 29–61 / avg 55–93 / max 92–144 ms（默认 power save 实测范围）。

### V.9 实测：运行时双向切换（M6-C §二十/§二十一，console-off 固件）

- **TCP→UART**：TCP GUI 会话 FULL → F12 → 会话断开 → UART GUI（`--no-reset`）连接 → FULL 重同步，
  零 decode/CRC/seq/session 错误；帧号连续（如 13→16）证明为活体切换而非重启。
- **UART→TCP**：UART 会话 F12 → TCP GUI 连接 → FULL 重同步，零错误（帧号 16→19）。
- 同一 GUI 进程经断线重连后重新 accept（`Reconnects 1`）+ FULL resync，零错误。

### V.10 新发现并修复：PC HostTcpTransport 断开传播缺陷

- 现象：ESP32 在对端关闭 TCP 后，PC `HostTcpTransport::rxLoop` 仅置 `connected_=false`，
  未调用 `setState(Disconnected)` → Worker `pumpLoop` 永不返回 → 不再重新 accept，
  会话状态机在 7s 被动 HELLO 重试循环中空转（session 错误计数持续增长）。
- 修复（`pc/src/host_tcp_transport.cpp`，最小改动）：rxLoop 的 select 错误路径与 recv==0/错误路径
  在锁外调用 `setState(State::Disconnected)`，与 HostUartTransport 的 ReadFile 错误传播对齐。
- 验证：修复后同一 TCP GUI 进程可完整经历 断开 → `transport disconnected` → 重新 accept →
  HELLO → CONNECTED → FULL resync，`Reconnects 1`，decode/CRC/seq/session 全 0。
- 附带观察（未改）：GUI 诊断时间戳以 steady_clock ms-since-boot 渲染为 HH:mm:ss，属既有外观问题，非协议问题。

### V.11 边界声明（延续）

- AP outage 断电测试 deferred（沿用既有 TCP disconnect / 退避重连 / PC restart / FULL resync 路径，本阶段已回归）。
- 不接 LCD / 触摸；不实现 HardwareDisplay / Mirror 实机、TinyUSB、UDP、mDNS、OTA、HTTP/WebSocket/Cloud、TLS。
- 不修改 wire format：Packet Header / CRC / Message / CHUNKED / Frame 语义 / INPUT_KEY / INPUT_MOUSE /
  MAX_PACKET_PAYLOAD 全部不变。
- Wi-Fi 凭据：只存在于未跟踪本地 `esp32/sdkconfig`，不写入本文件 / source / git / 日志（T.7 延续）。

### V.12 证据文件（build/m6a_png/）

- `uart_baseline_results.txt`（UART 3 轮 baseline）、`m6c_tcp_results.txt` / `m6c_tcp_qp_results.txt`
  （TCP 5 轮 FULL + qp）、`m6c_partial_results.txt` / `m6c_partial_v2_results.txt` / `m6c_input_results.txt`
  （TCP PARTIAL / INPUT 压力）、`m6c_switch{1..7}_{tcp,uart}*`（双向运行时切换 FULL 帧 PNG 证据，
  含 `full_{3,4,6,10,12,13,16,19}.png`）。


## W. M6-D Transport UI / Runtime Configuration / Switch Stress（2026-08-14 实测；wire format 未修改）

> 本阶段把 UART/TCP 运行时切换从「F12 开发者调试能力」提升为「正式用户可见 Transport 能力」：
> 正式 Transport 选择 UI、状态显示、切换流程、QSettings 持久化、20× 双向切换压测、断线重连回归。
> 全程零 wire format 修改；所有诊断经 ERROR 文本通道（`dbg` / `dbg2` 行）与 GUI 诊断面板。

### W.1 正式 Transport 选择 UI（任务书 §二/§二十）

- GUI 顶部 Transport 面板：`Transport [TCP ▼] [Apply]`，选择 TCP（Server）或 UART（Client）。
- TCP 配置：`Local server: 0.0.0.0` + `Port: 8765`；不显示 server 地址为 peer。
- UART 配置：`Port: COM4` + `Baud: 115200`（默认值来自 QSettings，可编辑）。
- `Apply` 流程（§五）：防重复点击（`switching_` 门）→ 本地校验（`validateTransportConfig`）→
  `TRANSPORT SWITCHING ...` → 停旧 Worker → 启动新 Worker → 等新 Transport 的 FULL commit →
  `CONNECTED (FULL resync done)`；30s 看门狗兜底（`onSwitchWatchdog`），超时 → `Switch failed: ...`。
- 切换期间旧 `CONNECTED` 不驻留：`beginSwitch` 立即 `clearDisplay()` + 状态标签切到 SWITCHING，
  新 FULL 到达前画面保持空（不显示旧 Transport 的 stale 画面）。

### W.2 Transport / Protocol 状态分离（任务书 §三）

- GUI 状态区严格区分两层：`Transport ✓ / Session CONNECTED`（标签 16）与
  `Transport TCP · Session Connected · Reconnects 0 · Peer <ip>:<port>`（标签 18）。
- TCP socket 已连接 ≠ 协议已连接：`transport connected` 只说明 TCP accept 完成；
  `CONNECTED (HELLO done)` 才是 HELLO 互换完成（由 Worker `ep_->onSessionState` 触发）。
- Peer 显示 ESP32 实际对端 IP:port；Server 绑定地址单独显示（§六，不混淆）。

### W.3 配置语义与持久化（任务书 §六/§七/§二十一）

- 语义与当前架构一致：PC 是 TCP server（`0.0.0.0:8765`），ESP32 是 TCP client；UART 为 COM/baud。
- `--transport` / `--port` / `--baud` / `--tcp-bind` / `--tcp-port` CLI 显式覆盖 QSettings；
  QSettings 保存：transport type、uart port、uart baud、tcp port、窗口大小。
- 绝不保存 Wi-Fi 密码 / ESP32 凭据（与 T.7 / V.11 一致）。
- M6-D 新增 `--diag-log <file>`：peer/session 诊断行完整追加到文件（不受 GUI 50 条 ring 限制，
  供压测取证；生产可不用）。

### W.4 Safe switch 语义（会话重置 + FULL resync + input reset，任务书 §四-§六/§十二）

- 切换 = close old → session reset → open new → HELLO → FULL resync → CONNECTED。
- 会话重置（PC 侧 `SerialWorker::switchTransport`）：stop+join 旧 Worker（ProtocolEndpoint 随线程
  销毁重建）→ 清 input 队列与 RX 残留 → 会话统计清零 → 启动新 Worker → 等 FULL。
- 会话重置（ESP32 侧 `TransportManager::switchTo`）：`switching_` 窗口内 Transport 状态缓冲，
  锁外重放 `Disconnected → Connected` → `ProtocolEndpoint::onTransportDisconnected/Connected` →
  seq/decoder/FrameAssembler/ACK/PARTIAL base/InputManager 全部清零 → HELLO（seq=0）→ FULL resync。
- Input reset：断线/切换时 ESP32 `InputManager::resetState()` 补发 stuck release + 清空状态；
  PC 切换时 `clearInputQueue()`；LVGL `inputAdapter_->reset()`（M5-B 语义延续）。

### W.5 新发现并修复：UART→TCP 切换失败根因（M6-D 最重要修复；wire format 未修改）

- **现象**：20× 压测基线（修复前）UART→TCP 失败 4/20（本轮）与 4/12、1/6（前序会话）——
  `Switch failed: timeout: no FULL commit within 30s`；TCP→UART 从未失败。
- **取证**（ERROR 文本通道 + GUI diag）：切换后 `transport connected` → `CONNECTED (HELLO done)`
  均成功，随后 **`frame discarded: kAborted`**，之后 `dbg f/p` 冻结、RemoteDisplay 空闲（q0 b0 e0
  a0 w0 c1）、再无新帧 → PC 30s 看门狗。即握手成功但 **FULL resync 帧被 ABORTED 且不再重绘**。
- **根因（两级）**：
  1. **切换与在途流式消息竞态**：F12 到达时 TX pump 正持有 `sendMutex_` 发 UART FULL（~13s/帧）。
     `switchTo` 的会话重置（`onTransportDisconnected`）只清 RemoteDisplay 状态，**pump 不知道**——
     它仍卡在旧 `sendStreaming`（等 `txMutex_` / TCP 未连接重试），不消费新 FULL resync 的槽位；
     `flush_cb` 排队等待预算（TCP 250ms）超时 → `dropPendingFrame()` → ABORTED END → `needFull_=true`。
  2. **静态 UI 不再重绘**：demo UI 无输入不产生 dirty rect；FULL 被丢弃后 `fullInvalidatePending_`
     已消费，LVGL 不再渲染 → 重同步永远无法完成（即使 transport 已 Connected）。
- **修复（3 处，均为控制流/时序，非 wire 格式）**：
  1. `debugTransportSwitch()`（sessionLoop 上下文，M6-D §九钩子）在 `switchTo()` 前先调用
     `g_endpoint.onTransportDisconnected()`：endpoint 立即置 Disconnected → `g_sink.send` 的
     alive 检查（`state != kDisconnected`）让在途流式消息**马上中止**（旧消息残包不会跨到新
     Transport；pump 快速释放 `sendMutex_`，新 FULL resync 不再排队超时）。
  2. `updateFlushWaitFromTransport()`：TCP FULL 预算 250ms → **2000ms**（实测 TCP FULL
     ~235–254ms，250ms 过紧）；PARTIAL 保持 250ms（丢弃即触发 FULL resync）。
  3. `LvglPort::flushCb`：`dropPendingFrame()` 后置 `fullInvalidatePending_=true` —— 任何整帧
     丢弃（FULL 或 PARTIAL）都会在下一 UI cycle 全屏置脏，静态 UI 也能自发重发 FULL resync。
- **验证**：修复后 20× + 20× 双向切换压测全部通过（见 W.6），`kAborted` 零出现。

### W.6 切换压测结果（真实硬件 COM4 CH340 @ 115200 + TCP LAN，console-off 固件）

- **20×（修复后第一轮，repro4）**：`rounds=20 ok=20 fail=0`；decode/CRC/seqGap/session 全 0。
  - TCP 切换（UART→TCP）：min 4631ms / avg 4851ms / max 5066ms（10 次，含 Apply→FULL commit 轮询）。
  - UART 切换（TCP→UART）：min 16513ms / avg 22773ms / max 23522ms（10 次，UART FULL ≈20.6s）。
- **20×（修复后第二轮，repro5）**：`rounds=20 ok=20 fail=0`；零错误（见 W.12 证据文件）。
- **修复前对照（repro3，同固件路径）**：`rounds=20 ok=16 fail=4`，4 次失败全部是 UART→TCP；
  失败窗口 diag 完整记录（`frame discarded: kAborted` 序列）作为根因证据。
- 每轮均验证 FULL commit（`full_<frameId>.png` 新增）+ 会话错误全 0。

### W.7 断线重连回归（任务书 §十一/§十九 M/N）

- **TCP kill → 重新 accept → HELLO → FULL**：GUI 重启 TCP server（同一进程）后
  `transport disconnected` → `TCP listening` → accept 新 client → HELLO → CONNECTED → FULL resync，
  `Reconnects` 计数递增，decode/CRC/seq/session 全 0（V.10 修复延续验证）。
- **UART close → reopen → HELLO → FULL**：GUI `--no-reset` 重新打开 COM4，UART 会话重建 +
  FULL resync，零错误（20× 压测中每轮 UART 切换即等价覆盖）。

### W.8 HostTcpTransport disconnect 回归（V.10 延续，任务书 §十七）

- 保留 V.10 修复（rxLoop 错误路径锁外 `setState(Disconnected)`），M6-D 压测中 TCP 断线/
  重连路径反复覆盖：`transport disconnected` → 重新 accept → HELLO → CONNECTED → FULL resync，
  无空转、无 session 错误累积。

### W.9 single-client policy（任务书 §十六）

- PC TCP server 单客户端策略：`TcpListener::acceptOne` 每次只 accept 一个 client
  （`acceptedOnce_` 门），GUI 显示 `Client: 1 / 1 (single)`；对端断开后重新 accept。
- 不实现多设备并发（M6-D 边界延续）。

### W.10 F12 debug hook 的去留（任务书 §九/§二十）

- ESP32 `CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH=y`（测试固件）：F12（HID 0x45）作为远程切换协助
  —— GUI Apply 在切换前经旧 Transport 发送 F12，命令 ESP32 同步切换；**UI 不依赖 F12**
  （生产固件无该钩子时按键被忽略，PC 侧切换照常进行）。
- 正式用户界面是 GUI Transport 面板（W.1）；F12 保留为压测/调试钩子，不进入用户交互路径。
- 生产固件建议 `CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH=n`（单 Transport 部署无需运行时切换）。

### W.11 边界声明（延续 V.11）

- AP outage 断电测试仍 deferred（沿用 TCP disconnect / 退避重连 / PC restart / FULL resync 路径）。
- 不接 LCD / 触摸；不实现 HardwareDisplay / Mirror 实机、TinyUSB、UDP、mDNS、OTA、
  HTTP/WebSocket/Cloud、TLS、多设备。
- 不修改 wire format：Packet Header / CRC / Message / CHUNKED / Frame 语义 / INPUT 消息 /
  MAX_PACKET_PAYLOAD 全部不变（M0–M6-C 冻结协议无任何改动）。
- Wi-Fi 凭据只存在于未跟踪本地 `esp32/sdkconfig`，不写入本文件 / source / git / 日志。
- M6-D 不改 flash 分区（4 MiB / 2 MiB app / ≈49% 余量，U 节延续）。

### W.12 证据文件（build/m6d_png/）

- `repro4/stress2_log.txt` + `stress2_rows.txt`：修复后 20× 压测（20 OK / 0 FAIL）。
- `repro4_diag.txt`：完整 ERROR 文本通道（含 `dbg` / `dbg2` / `sess` / `disp` 行）。
- `repro4/full_*.png`：每轮 FULL commit 画面证据。
- `repro5/` + `repro5_diag.txt`：第二轮 20× 压测（20 OK / 0 FAIL）。
- `repro3/` + `repro3_diag.txt`：修复前 20× 对照（16 OK / 4 FAIL，全部 UART→TCP；
  `frame discarded: kAborted` 根因取证）。
- 诊断行格式：`dbg u=<report秒> f=<flush_cb入口秒> p=<pump返回秒> b<c> i<flushCbInProgress>
  tu<UI任务态> tt<TX任务态>`；`dbg2 s<sinkActive> <sinkAgeMs>s w<switchActive> <switchAgeMs>s
  ts<session任务态> tr<stats任务态> q<queued> x<flushCbExit秒> y<pumpEntry秒>`
  （FreeRTOS 任务态：0=running 1=ready 2=blocked）。

## X. M6-E Wi-Fi/TCP Engineering Hardening + Stability + Config + Production Profile（2026-08-14 实测；wire format 未修改）

> 范围：Wi-Fi STA 状态机审计、TCP reconnect/backoff、listener 生命周期、single-client
> 策略、Transport UI 配置、power-save 配置检查、TCP 性能遥测、长时间 TCP 稳定性、
> reconnect stress、stale session cleanup、socket shutdown/close race、HostTcpTransport /
> ESP32 TcpTransport 回归、RuntimeStats、生产/测试 Kconfig profile 分离、错误分类、日志整理、
> 配置验证、host tests、hardware sanity。
> 禁止：LCD/Touch/Mirror/HardwareDisplay 实机/TinyUSB/OTA/UDP/mDNS/WebSocket/HTTP/TLS/
> Cloud/Multi-device/协议重设计/新 Packet/新 Message/新 wire flag/AP 断电强制验收。
> 结论：无 wire format 冲突；无 CRITICAL/HIGH 架构问题；完成 3 个 MEDIUM 修复
> （HostTcpTransport send 致命路径、attach 所有权、TransportManager stale-state 清理）。

### X.1 Wi-Fi STA / TCP 状态机审计（任务书 §十一）

- ESP32 Wi-Fi STA（WifiSta）+ TCP client（TcpTransport）+ PC TCP server 状态机整体一致：
  - WifiSta: DISCONNECTED → CONNECTING（WIFI_EVENT_STA_START）→ CONNECTED
    （WIFI_EVENT_STA_CONNECTED，记录 rssi/ch）→ 断线/错误回 DISCONNECTED
    （WIFI_EVENT_STA_DISCONNECTED），由 reconnect/backoff 策略驱动重连。
  - TcpTransport: Closed → Connecting（socket + connect 阻塞）→ Connected（连接建立）→
    Disconnected（对端关闭/超时/错误）；open() 失败时 running_=false + wifi_.deinit()
    （M6-E 新增：失败路径不再残留 Wi-Fi 占用，允许上层切换/重试）。
  - PC HostTcpTransport: Disconnected → Listening（listener accept）→ Connected
    （accept 完成）→ Disconnected（对端断开/超时/错误）。
- 无状态机结构性缺陷：所有状态迁移都有明确触发源与清理路径；断线后 ESP32 走退避重连，
  PC listener 重新 accept（single-client，acceptedOnce_ 门），重连后会话重置 + HELLO +
  FULL resync。

### X.2 TcpTransport（ESP32）审计（任务书 §十三/§二十六）

- socket 生命周期：open → socket(AF_INET, SOCK_STREAM) → connect（带超时）→ send/recv；
  close → shutdown(WR) → closesocket；RX 线程 select 超时定期检查关闭标志，不忙等。
- wifiApInfo()（M6-E 新增）：经 esp_wifi_sta_get_ap_info 读取 rssi/primary channel，由
  TcpTransport 转发、Esp32TcpAdapter 覆写 ITransport::wifiApInfo，仅供诊断行（非 wire）。
- reconnect/backoff：对端断开后指数退避重连（上限截断），期间保持 Wi-Fi STA 连接；
  PC server 恢复后自动重连 → HELLO → FULL resync，无手工干预。
- 已知边界：AP 断电（router 掉电）场景 deferred（见 X.15）；当前验证覆盖 TCP 断开/重连/
  PC restart/FULL resync 路径。

### X.3 HostTcpTransport（PC）审计与修复（任务书 §十七/§二十四）

- 修复 1（MEDIUM，CS-1）：send() 致命错误路径（send 失败/socket 关闭）原来只设置
  connected_=false，不通知 Worker 状态回调 → pumpLoop 可能空转卡死。现在在锁内置
  connected_=false，锁外 setState(Disconnected)（与 RX 路径修复一致，M6-D V.10 延续）。
- 修复 2（MEDIUM，CS-2）：attach() 现在拥有传入 socket 的所有权（重复 attach 拒绝时关闭
  新 socket，避免泄漏）；acceptOne 失败分支删除冗余 closesocket（避免 double-close 误关
  错误 socket 描述符）。
- listener 生命周期：bind(0.0.0.0:8765) → listen → acceptOne（single-client）→ 对端断开后
  重新 accept；close 时 shutdown + closesocket + join RX 线程，幂等。
- 对端断开（recv=0）/ select 错误 / shutdown 后 recv 唤醒均统一走 setState(Disconnected)，
  无泄漏、无双重关闭、无线程泄漏。

### X.4 TransportManager stale-state 清理（任务书 §五/§二十）

- 审计发现（LOW）：切换窗口（switching_=true）内旧 Transport 迟到的 Connecting/Connected
  状态会进入 pending 队列，可能在新会话建立后重放 → 上层误以为旧传输仍在线。
- 修复（CS-3）：switchTo()/reopen() 在 closeLocked() 之后、createAndOpenLocked() 之前调用
  clearPending()：丢弃旧 Transport 的 stale Connecting/Connected，只保留会话结束的
  Disconnected（上层依赖它做 input/session 清理：InputManager.resetState）。
  - 偏差说明（已校验）：clearPending() 保留一个 Disconnected 而非清空全部 pending——
    完全清空会破坏 transport_pipeline_test.cpp:306-307 依赖的会话结束 input-reset 语义。
  - 新增异步测试：旧 Transport 迟到的 Connected 被丢弃、Disconnected 保留、新 Transport
    状态按序上报。
- TransportManager 本身无 generation id：切换/重开期间回调先经 pending 缓冲再 flush，
  switching_ + detach + join 路径下 nominal-safe（auditor 结论）。

### X.5 ITransport 只读统计快照（任务书 §22/§24）

- ITransport 新增 4 个非纯虚默认访问器（非 wire 字段，纯诊断，默认 0/false）：
  reconnectCount() / txBytes() / rxBytes() / wifiApInfo(rssi*, channel*)。
- ESP32 侧：WifiSta::apInfo（esp_wifi_sta_get_ap_info）+ TcpTransport::wifiApInfo 转发 +
  Esp32TcpAdapter 覆写。PC HostTcpTransport 统计（tx/rx bytes、reconnect count）由 M6-E
  扩展测试覆盖（SO_LINGER 立即 RST 后 send 失败路径等）。

### X.6 ESP32 诊断行 trx（任务书 §22/§二十三；非 wire 格式）

- statsLoop 每 3s 经 ERROR 文本通道发送一行（≤64B，计数器 clamp ≤9999，本地 96B buffer）：
  trx tr=<0|1> st=<0..3> sw=<switchCount> rc=<reconnectCount> tx=<txBytes> rx=<rxBytes>
  rssi=<rssi|-128> ch=<channel|0>
  - tr：0=UART 1=TCP；st：ITransport::State（0=Disconnected 1=Connecting 2=Connected
    3=Error）；rssi=-128 表示当前无 AP 信息；ch=0 表示无 AP 信息。
- 不记录任何 Wi-Fi 凭据；Wi-Fi 错误只记录 error code / reason（现有 WifiSta 日志规范延续）。

### X.7 生产/测试 Kconfig profile（任务书 §19/§二十七/§二十八）

- Kconfig：CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH 默认 y → n（生产 profile 默认不编译 F12
  transport-switch test hook）；sdkconfig.defaults 追加 =n。
- 本地未跟踪 esp32/sdkconfig 仍为 =y：测试固件（用于 M6-D/M6-E 运行时切换压测）。生产固件
  （默认 sdkconfig.defaults 构建）为单 Transport 部署，无 F12 钩子，UI 不依赖它。
- 两种 profile 的唯一差异是编译期测试钩子；协议/传输/UI 代码路径相同。

### X.8 运行时配置：优先级与持久化白名单（任务书 §18/§24.11-12）

- 配置优先级：CLI > QSettings > 默认值（默认：TCP / COM4 / 115200 / 0.0.0.0:8765）。
- pc/src/transport_config.h 新增 applyCliOverrides(cfg, args, err)（语义与 main.cpp 原解析块
  一致：--transport/--port/--baud/--tcp-port/--tcp-bind；未知 --transport 值报错返回 false；
  其余参数忽略）与 persistedSettingsKeys()（白名单 5 键：transport/type、uart/port、
  uart/baud、tcp/port、window/size）。
- 凭据安全：TransportConfig 结构本身不含任何 Wi-Fi 字段；持久化键白名单结构性保证绝不保存
  SSID/密码。Wi-Fi 凭据只存在于 ESP32 本地未跟踪 sdkconfig，不写入源码/git/日志。
- transport_config_test.cpp 19 → 67 checks（含无凭据白名单结构测试）。

### X.9 Host tests 扩展（任务书 §24）

- pc/src/tcp_transport_test.cpp +3 测试（94 → 126 checks）：#13 RST（SO_LINGER 0）后 send
  致命 → Disconnected（3s 内）；#14 send-after-close；#15 3 轮 reconnect 循环。
- 全套 host：协议套件 208,951 checks / 0 failures（M6-E 前 208,934）；transport_config_test
  67 / 0；tcp_transport_test 126 / 0；ctest 1/1 Passed。
- 30 分钟长稳作为 manual verification，不加入普通 CI（ctest 保持快速/可重复/离线）。

### X.10 长时间 TCP 稳定性（任务书 §二十五；manual，30 min）

- 方法：单 GUI TCP 会话连续 30 分钟；GUI 每 8s 向 VirtualScreenWidget 发送 wheel + mouse
  move（UIA 定位控件本体，非窗口中心——窗口中心落在状态面板上，输入不会到达 ESP32）；
  PING/PONG 每 2s 自动流动；diag-log 每 3s 捕获 ESP32 统计。
- 结果（见 X.17 证据）：
  - GUI 全程存活；PING/PONG 健康（sess h=/p= 持续递增，rxPong 无缺口或短暂追赶后收敛）。
  - heap 完全平坦（无泄漏）：disp3 hb/ha=231352/231352 全程唯一二元组；min-watermark hm
    起始 121236，首帧 FULL 附近一次性下探到 97356（~24KB 瞬态分配，此后 10+ 分钟恒定），
    无持续下降——非泄漏。
  - 会话错误全程 0：sess2 ... e=0 c=0 s=0（decoder/crc/seqGap 全 0）。
  - PARTIAL + 输入链路验证：inp3 w>0 s>0（wheel/move 被 ESP32 消费）、disp2 part= 持续增长
    （6 位 clamp 到 999999）、LVGL 1Hz counter 正常（FULL id 递增）。
  - FULL/PARTIAL 均正常提交（longrun/full_*.png 画面证据）。

### X.11 FULL / PARTIAL / 输入 stress（任务书 §25-§26 相关）

- FULL：连接建立后首帧 FULL 提交（full_*.png 逐张生成）；断线重连后强制 FULL resync
  （会话重置语义，M6-D W.4 延续）。
- PARTIAL：长稳期间 LVGL 1Hz counter + 输入驱动 dirty rect 持续产生 PARTIAL，disp2 part=
  累计，帧提交无卡死、无错误。
- 输入：UIA 直击 VirtualScreenWidget（class espview::pc::VirtualScreenWidget，bounding
  rect x=593 y=242 w=733 h=240）→ GUI Input sent 非 0；ESP32 inp3 wheel/keys/rects 计数
  递增；GUI Input sent=N / 0 dropped。
  - 修正：输入定位必须用控件 UIA rect，不能用窗口中心（状态面板遮挡问题已修复脚本）。

### X.12 TCP reconnect stress（任务书 §19/§25）

- 方法：reconnect_stress.ps1 -Rounds 10：每轮杀 GUI（= TCP server 下线）→ ESP32 reconnect
  loop → GUI 重启 → accept → HELLO → CONNECTED → FULL resync，记录每轮 elapsedMs 与
  conn/err 行；随后短输入 churn 验证新会话输入通路。
- 结果：见 X.17 证据行文件（rounds/ok/fail）；每轮完整 transport disconnected → reconnect
  → CONNECTED (HELLO done) → FULL resync，无 CRC/seqGap/session 错误累积。

### X.13 Heap / 内存行为（任务书 §25）

- 长稳全程 hb/ha=231352/231352 唯一二元组（before/after 完全平坦，无增长、无碎片波动）；
  hm min-watermark 起始 121236，首帧 FULL 附近一次性降至 97356 后恒定，无持续下降（非泄漏）。
- LVGL draw buffer 15360B（dw=）、TX 队列峰值 qp=2（有界 TX 队列，M6-C V.4 延续），
  无整帧丢弃增长（d=0、q= 队列满事件 0）。

### X.14 Power-save 说明（任务书 §二十）

- 默认保持 Wi-Fi power save ON（生产默认）；实测（M6-B U 节）：默认 power save 下 TCP FULL
  ≈373–716ms，PS_NONE ≈369–438ms——差距在噪声范围内，不构成生产默认切换理由。
- PS_NONE 实验仅存在于 U 节记录，不改变任何默认配置；M6-E 无功率配置变更。

### X.15 AP outage deferred（任务书 §八）

- AP 断电（router 掉电）作为强制验收项目仍然 deferred：当前验证覆盖 TCP 断开 → 指数退避
  重连 → PC server 重启 → 重新 accept → HELLO → FULL resync；AP 掉电期间 Wi-Fi STA
  DISCONNECTED → WifiSta 重连（含 rssi/ch 重新获取）路径未纳入本轮硬件验收（需 router
  可控断电环境）。不修改协议/状态机以支持 AP outage。

### X.16 边界声明（延续 W.11/V.11）

- 不接 LCD / 触摸；不实现 HardwareDisplay / Mirror 实机、TinyUSB、UDP、mDNS、OTA、
  HTTP/WebSocket/Cloud、TLS、多设备。
- 不修改 wire format：Packet Header / CRC / Message / CHUNKED / Frame 语义 / INPUT /
  MAX_PACKET_PAYLOAD 全部不变（M0–M6-E 冻结协议无任何改动）。
- 不修改 flash 分区（4 MiB / 2 MiB app / 无 OTA）；不实现 OTA。
- Wi-Fi 凭据只存在于未跟踪本地 esp32/sdkconfig，不写入本文件 / source / git / 日志。
- 921600 维持 experimental-only；正式 baseline 仍为 115200 8N1（M1-3C 冻结）。

### X.17 证据文件（build/m6e_png/）

- sanity_diag.txt：硬件 sanity（FULL 首帧、trx 行、heap、sess2 全 0）。
- longrun_diag.txt + longrun_stdout.txt + longrun/full_*.png：30 分钟长稳（GUI PID、first
  FULL、输入流、heap 唯一二元组、sess2 e=0 c=0 s=0、final full_count/diag_lines）。
- fresh_input_diag.txt：UIA 直击 VirtualScreenWidget 后的输入验证（Input sent、ESP32 inp3
  w/k/r 递增、PARTIAL 流动）。
- tcp_reconnect/reconnect_log.txt + reconnect_rows.txt + tcp_reconnect_diag.txt：TCP
  reconnect stress（rounds/ok/fail、每轮 elapsedMs、CONNECTED 行、err 行）。
- launch_longrun.ps1 / reconnect_stress.ps1 / input_probe.ps1 / key_probe.ps1 /
  sendinput_probe.ps1 / pin_gui.ps1：manual 验证脚本（不入 CI）。
- 代码：CS-1..CS-9 分布在 pc/src/host_tcp_transport.{cpp,h}、shared/transport/
  transport_manager.{cpp,h}、shared/transport/transport.h、pc/src/transport_config.h、
  esp32/main/main.cpp、esp32/main/Kconfig、esp32/sdkconfig.defaults、
  pc/src/tcp_transport_test.cpp、pc/src/transport_config_test.cpp。
## Y. M7-A 独立 OLED 状态显示（2026-08-15 实测；wire format 未修改）

M7-A 新增一块 128×64 I2C OLED（SSD1306/SH1106 兼容），作为**独立的诊断/状态显示**
（transport/session/IP/RSSI/帧数/错误/堆/运行时间）。它不是 LVGL 的显示后端，不承载
Application 画面；**OLED 显示内容永远不是权威 framebuffer**（ESP32 的 LVGL/RGB565
画面仍是唯一权威）。wire format / Packet Header / CRC / Message / CHUNKED / Frame 语义
全部不变；OLED 组件零依赖 protocol/display/lvgl/transport。

### Y.1 硬件接线（实测 2026-08-15）

- OLED I2C：SDA → GPIO21，SCL → GPIO22，VCC → 3.3V，GND → GND；
- GPIO21/22 无冲突（非 strapping/flash/ADC，工程内零引用；VSPI 默认 IO_MUX 不影响
  I2C matrix 路由）；
- 板级无外部上拉 → 驱动开启内部上拉（`enable_internal_pullup=1`；400kHz 下够用，
  高速场景建议外接上拉）；
- I2C 时钟 400kHz；地址自动探测 0x08..0x77（优先 0x3C/0x3D）；实测探测到 **0x3C**。
- 控制器探测只能确认地址、无法可靠区分 SSD1306/SH1106 → Kconfig 提供
  AUTO（默认 SSD1306）/ 强制 SSD1306 / 强制 SH1106；实测 AUTO → SSD1306。
  SSD1306 = 128×64 GDDRAM（0x21/0x22 水平寻址）；SH1106 = 132×64（列偏移 2，
  逐页 0xB0|page + 列地址命令），命令序列差异见 `shared/oled/oled_cmd.cpp`。
  **segment remap 固定 OFF（0xA0）+ 页内列序反转上传**：实机首版（0xA1）与
  0xA0 对照均呈现水平镜像（控制器疑似忽略 remap 命令）→ 2026-08-15 最终修正为
  按页反转列序上传（地址列 C ← fb 列 127-C，页顺序 0..7 不变，垂直不受影响；
  golden bytes 同步更新）；COM scan 保持 0xC8（dec），垂直方向实测正常。
   反转上传后整串方向正确，但单个字形仍左右镜像 → 根因是 `drawText` 按 **bit7=最左**
   读取内置字体（每个字形内部被镜像）→ 改为按 **bit0=最左** 逐列转置（字体表本身
   即 bit0=最左约定：'C'={0x3C,0x66,0x03,...} 按 bit0 读为开口向右的正 C）；
   '/'、'\' 两个字形在 bit0 约定下原数据即方向正确，无需互换；host golden bytes
   同步重算。

### Y.2 驱动/组件结构（冻结）

- `shared/oled/`：`OledFb`（128×64 1bpp 页式 fb，8 pages × 128B = **1KB** + 内置
  8×8 ASCII 字体）、`oled_cmd`（SSD1306/SH1106 init/on/off/contrast 命令序列 +
  帧上传分段生成），纯 C++17 零平台依赖，host 单测 golden bytes / 分段边界；
- `esp32/components/oled/`：
  - `OledI2c`：ESP-IDF v6.0.2 **新 I2C 驱动** `driver/i2c_master.h`
    （`i2c_new_master_bus` + `i2c_master_probe` + `i2c_master_bus_add_device` +
    `i2c_master_transmit`）；**bus 级无 clk_speed_hz，速率在设备级 `scl_speed_hz`**；
    `trans_queue_depth=0` 同步模式（异步模式与 `i2c_master_probe` 不兼容，驱动源码
    明确 warning）；`allow_pd=0`；每次 transmit ≤32B 分段；
  - `OledDisplay`：低优先级任务 + 有界错误恢复；`status_ui`：状态页渲染；
  - Kconfig：`ESPVIEW_OLED_ENABLE` 默认 **n**（禁用时零代码路径）、SDA=21/SCL=22、
    I2C 400kHz、ADDR_AUTO、控制器 choice、REFRESH 1000ms、任务栈 4096/优先级 2、
    I2C 超时 50ms、MAX_REINIT 3。

### Y.3 线程与刷新模型（冻结）

- 独立 `espview_oled` 任务（优先级 2 < stats=3 < session=5）；每 refreshMs(1s)
  调用 main 注入的 StatusProvider（`StatusSnapshot` 值语义）→ 渲染进 1KB fb →
  按段上传（每段 ≤32B 含控制字节）；
- OLED 任务**绝不触碰 protocol sendMutex / Transport**；状态快照字段由 main 填充
  （读原子/普通量，与 statsLoop 同一约定）；**绝不显示/打印 Wi-Fi SSID/密码**；
- IP 获取按 transport 类型门控：仅 TCP 模式取 esp_netif IP 且判空（UART-only 下
  esp_netif 未初始化不可取 IP，显示 "--"）。

### Y.4 错误恢复（冻结）

- 错误计数只增（errorCount / refreshCount / lastErrorMs 原子）；连续失败达
  MAX_REINIT 触发恢复：① bus_reset + 重发 init/清屏（用现有设备句柄）；② 失败则
  整体重建（bus → probe → addDevice → init）；
- 每个故障窗口重初始化轮数有界（kMaxReinitCycles=3）+ 指数退避 0.5s→30s +
  30s 冷却，**不允许无限重置循环**；degraded 时仍按 refresh 周期尝试；
- stop() 置停止标志 + 通知唤醒 + 有界等待任务退出（2s），幂等可重复调用。

### Y.5 状态页（8 行 × 8px 字体）

`ESPView` / `UART|TCP + 会话状态` / `IP --|<ip>` / `RSSI CH`（TCP 且 apInfo 有效）/
`FRM <n>` / `ERR <n>` / `HEAP <n>` / `UP hh:mm:ss`；计数 clamp 防溢出 128px 行宽。

### Y.6 诊断通道（ERROR 文本行，非 wire 格式）

statsLoop 每 3s 追加一行（≤64B，makeError 限制）：
`oled a=<addr> c=<ctrl> err=<errCount> ok=<0|1>`；err 计数 clamp 到 5 位；
经现有 ERROR 消息通道上报，不修改协议。

### Y.7 实测结果（2026-08-15 真实硬件，CH340 COM4 @ 115200，UART transport 验收模式）

- 探测：`a=0x3C c=SSD1306`；初始化/清屏成功；`ok=1`；
- 首版实机发现字体水平镜像（"ESPView"→"weivpse"）→ 0xA0/0xA1 remap 对照均镜像，
  按页反转列序上传后整串方向正确、但字形仍单个左右镜像 → `drawText` 改为按 bit0=最左
  读取字体（字形内部翻转修复），host golden bytes 同步重算；
- 15 分钟稳定性：298 条 `oled` 行全部 `err=0 ok=1`（errorCount 只增、末值仍 0）；
- 协议零污染：采集期间 0 CRC 错误 / 0 bad magic / 0 protocol errors；
- LVGL+UART 帧流同场验证：FULL=153600B + PARTIAL=6392B（dirty 4.16%），
  0 CRC / 0 mismatch（`pc_com3_lvgl_sanity.py` PASS）；
- 构建与 host：OLED_ENABLE=y / =n 双配置 ESP32 构建通过（-Wall -Werror 零警告）；
  host 全量 **210,504 checks / 0 failures**（新增 OLED host 测试 1,553 checks）；
  ctest 1/1。

### Y.8 边界声明

- OLED 是诊断显示，不是 LVGL 输出 / HardwareDisplay / Mirror；不改变 DisplayMode
  架构与 Application 代码；
- 不显示 SSID/密码；Wi-Fi 凭据仍只存在于未跟踪本地 `esp32/sdkconfig`；
- 不修改 wire format / flash 分区 / LVGL 路径；`scripts/pc_oled_monitor.py`（被动
  串口监控 OLED 诊断行）为新增脚本，不进入协议链路。


## Z. M7-B OLED 生产语义收尾（2026-08-15；wire format 未修改）

M7-B 是 M7-A 的生产化收尾：不改变 OLED 显示内容与协议，只修复架构审计（Agent 1）
给出的 3 个 Mandatory 问题（OLED/I2C 生命周期、Transport 生命周期、状态可观测性），
并补充 statsLoop `mem` 堆诊断行与 `pc_oled_monitor.py` 握手/保活/错误解析重写。
wire format / Packet Header / CRC / Message / CHUNKED / Frame 语义零改动。

### Z.1 定位（重申，未变）

- OLED 是 Diagnostic Sink（诊断显示），不是显示后端；不进入 DisplayMode /
  Application 代码路径；状态页内容沿用 Y.5，本阶段不新增页面。

### Z.2 OLED/I2C 生命周期（审计 M#1，已修复）

- `stop()` 只置停止标志 + 通知唤醒 + 等待任务退出；**I2C 资源（bus/device）释放
  只发生在 taskLoop 退出路径**，绝不在 stop() 调用线程释放（消除“任务内 transmit 与
  释放竞争”导致的 UAF）；
- join 超时（2s）时保留 task_ 句柄并进入 kStopping 态，禁止 start() 重启同一实例；
  析构提供 15s 兜底等待，仍不退出则 `i2c_.release()`——宁可泄漏资源也绝不释放仍可能
  被任务访问的内存；
- `uploadFrame` / `executeInitSequence` 每段 transmit 前检查 `running_` 停止谓词
  （原子），使停止请求可中断长上传；
- `taskExited_` / `running_` 改为 release/acquire 语义，消除编译器重排风险。

### Z.3 Transport 快照（审计 M#2，已修复）

- statsLoop 不再在锁外解引用 `g_mgr.transport()` 裸指针；TransportManager 新增
  `diagSnapshot()`（`TransportDiagSnapshot` 值语义：kind/connected/reconnectCount/
  txBytes/rxBytes/wifiApInfo），一次临界区内复制，锁外安全消费；
- `capabilities()` 改为按值返回（同上生命周期安全）；
- `tcp_transport` 的 `reconnectCount_` 原子化。

### Z.4 OledStatus 可观测性（审计 M#3，已修复）

- `OledStatus` 新增 `state`（`OledState` 枚举：kDisabled/kInitializing/kReady/
  kDegraded/kStopping，原子）与 `lastFlushDurationMs`（最近一次整页上传耗时）；
- `errorCount` / `refreshCount` / `lastErrorMs` 沿用 Y.4 只增原子约定不变。

### Z.5 statsLoop `mem` 诊断行（M7-B 新增，非 wire 格式）

- statsLoop 每 3s 追加一行（≤64B，makeError 限制）：
  `mem h=<freeHeap> lg=<largestBlock> mn=<minFreeHeap>`；
- 值来源：`esp_get_free_heap_size` / `heap_caps_get_largest_free_block` /
  `esp_get_minimum_free_heap_size`；
- 用途：长稳堆趋势监控（host monitor 判定末值比初值低 5% 判泄漏）；不修改协议。

### Z.6 pc_oled_monitor.py 更新（M7-B 重写，非 wire 格式）

- HELLO 握手：先等 ESP32 启动 HELLO 再发本端；兼容已运行场景（3s 未收到则主动发）；
- 主动每 1s PING 维持会话（原因：FULL 153600B @115200 ≈13.5s 期间 ESP32 自身 PING
  被 sendMutex 放弃，仅被动回 PONG 会触发 5s peer timeout）；
- ERROR 负载解析修正：必须剥离 3 字节头（u16 errorCode + u8 msgLen），此前未剥离
  导致 `startswith("oled")` 不命中；
- `mem` 行解析 + 堆趋势判定（末值比初值低 5% 判泄漏）+ 8 位 clamp。

### Z.7 回归与构建（M7-B）

- host 全量 **218,625 checks / 0 failures**；ctest 1/1；
  verify_host / verify_qt / verify_lvgl（host）全部 PASS；
- ESP32 构建（-Wall -Werror 零警告）：
  - 生产 OLED=y（TCP）：**1,092,448 B**（app 余量 48%）；
  - OLED=n：**1,062,512 B**；
  - 测试 profile（UART + TEST_TRANSPORT_SWITCH=y + OLED=y）：**449,744 B**；
- 修复：`esp32/components/oled/CMakeLists.txt` 源路径补 ${CMAKE_CURRENT_LIST_DIR}
  前缀（否则 ESP32 构建必炸）；GCC16 `-Warray-bounds` 误报在
  `remote_display_test.cpp` 单文件抑制；
- `shared/transport/tests/transport_manager_test.cpp` 新增 `diagSnapshot` vs
  `switchTo` 双线程并发压力测试。

### Z.8 实测（2026-08-15 真实硬件 COM4 @ 115200 / TCP）

- 30 分钟稳定性长跑（UART 测试固件 + `pc_oled_monitor.py --duration 1800`）：
  - 1800.1s，597 条 `oled` 行全 `err=0 ok=1`，final_oledErrorCount=0；
  - heap：first=160096 last=168568 **delta=+8472**（mn=156508，无泄漏趋势）；
  - 0 CRC / 0 bad magic / 0 protocol errors；rx_ping 完整；RESULT PASS；
- LVGL sanity（`pc_com3_lvgl_sanity.py --watch 30`）：FULL=153600B + PARTIAL=6392B
  （dirty 4.16%），0 CRC / 0 mismatch；
- input_send_test（9 事件 mouse/key/wheel）：rx=9 dropped=0，0 CRC/seq；
- TCP reconnect stress（生产 OLED=y TCP 固件，PC 当前局域网 IP:8765）：
  初始 FULL 连接成功；10 轮 GUI kill/重启 → ESP32 重连 + HELLO + FULL resync
  全部 OK（**rounds=10 ok=10 fail=0**，每轮 ~2.0s）；0 CRC / 0 seqGap / 0 session
  错误；OLED 状态行全程 `err=0 ok=1`。

### Z.9 边界声明

- 本阶段不实现：新 Transport、LVGL 后端变更、DisplayMode 变更、输入扩展、压缩、
  TinyUSB、真实 LCD、AP outage 实测；
- OLED 仍是诊断显示，永不成为权威 framebuffer；状态页不显示 SSID/密码；
- 不修改 wire format / 分区 / Kconfig 默认值语义；`mem` 行与 monitor 脚本均不在
  协议链路内。
## AA. M7-C1/C2 多显示架构与物理显示后端（2026-08-15 实测；wire additive: kSplit=3）

### AA.1 定位（覆盖 Z.1/Z.9 中"OLED 仅是 Diagnostic Sink / 本阶段不实现 DisplayMode 变更"的声明）

M7-C1 冻结 DisplayRouter 四模式路由（0=VirtualOnly / 1=PhysicalOnly / 2=Mirror / 3=Split）；
M7-C2 把 M7-A/B 的 OLED 从"独立诊断显示"提升为正式 Physical Display Sink
（同时保留 Diagnostics 诊断页）。wire format 只新增 kSplit=3（SET_MODE payload 仍为
[0] mode；HELLO mode_mask 0b0111 → 0b1111，bit3=SPLIT）。协议 packet/message/frame
语义零改动。

### AA.2 DisplayCapabilities / IDisplaySink（C1 冻结，C2 消费）

- DisplayCapabilities：width/height/format/color/mono/canReadback/sinkKind。
- IDisplaySink：init(caps) / capabilities() / present(rect, px) / flush() /
  setEnabled(bool) / isAvailable() / status()。
- PhysicalDisplaySink.capabilities() = 128x64 / RGB565(生产者格式) / 1bpp mono /
  canReadback=false / kPhysical。
- DisplayRouter 仅扇出与状态机：按模式选择 sink 目标集；任一成功→kOk 聚合；
  writeRect 逐 sink 以 isAvailable() 门控；presentScene(PhysicalScene) 仅 kSplit
  接受（C1）；setMode 切换窗口 disable 所有 sink + staleClear + fullResync 钩子。

### AA.3 PhysicalScene 与模式→场景映射（定稿）

- PhysicalScene::kDiagnostics / kApplication。
- Mirror(2)/PhysicalOnly(1) → Application（OLED 显示 LVGL 应用缩略帧）；
- Split(3)/VirtualOnly(0) → Diagnostics（VirtualOnly 下应用帧禁用，诊断页继续）。
- 映射由 main.cpp sceneOf(mode) 派生，Kconfig 不单独设项（避免可配置矛盾）。

### AA.4 PhysicalDisplaySink（esp32/components/oled）

- 非拥有引用 OledDisplay*；init 校验生产者能力（v0.1 仅 RGB565、正分辨率），
  落定 128x64 mono caps；present 仅在 Application 场景 + enabled 时经
  OledDisplay::presentAppFrame 同步渲染进共享 1KB 应用 fb（mutex 保护，
  绝不持有 px 指针）；Diagnostics 场景 no-op（诊断页由 OLED 任务 renderStatus
  自绘）；flush 为同步 no-op（无排队内容）；isAvailable = OLED kReady（I2C 存活），
  与 setEnabled 正交。
- I2C 上传只发生在 OLED 任务内（锁内 memcpy 1KB 快照 → 锁外分段 ≤32B 上传）；
  flush_cb / present 路径零 I2C、零长阻塞 → 物理失败绝不影响 LVGL/Virtual 路径。

### AA.5 OLED PhysicalRenderer（shared/oled，host 可测）

- RGB565 → Mono1：逐字节 LE 组合（无 reinterpret_cast），R5/G6/B5 放大到 8bit，
  luminance = (299R + 587G + 114B)/1000 四舍五入，threshold 128（Y>=th → 亮）。
- crop/scale：源 320x240 → 目标 128x64 保持宽高比；scale=2/5 最近邻
  （ox = sx*2/5，oy = sy*2/5），垂直 center crop 上下各 16px（可见源区
  y∈[40,199]）；无堆分配、无异常、矩形增量与越界裁剪。
- golden tests：全白=全1、全黑=全0、棋盘格式 1KB memcmp、阈值边界、crop/scale/
  center-crop、空矩形、确定性；整帧 153600B 实测 ≈0.25ms。

### AA.6 LVGL flush_cb → Router 映射（lvgl_port）

- flush_cb：router_->writeRect(Rect, px) / (flush_is_last → router_->flush())；
  无 router 回退 remote_ 直连（保持既有契约）。
- VirtualSink 适配器（lvgl_port 匿名命名空间）：present→remote_->writeRect、
  flush→remote_->flush、setEnabled→remote_->setEnabled、
  isAvailable→debugState().connected。
- 保留契约：kQueueFull/kFrameBusy 有界等待（slotFreeSem 10ms 步进，FULL 长等/
  PARTIAL 短等预算 flushWaitMs()）；超时 → remote_->dropPendingFrame() + 全屏
  置脏（下一帧 FULL resync）；lv_disp_flush_ready 无条件调用；px_map 只在
  flush_cb 内有效（同步消费）。Mirror/Split 下 Router 聚合可能被物理接受掩盖
  Virtual 背压 → 用 VirtualSink::lastPresentStatus() 恢复既有等待/丢弃语义。

### AA.7 四种模式语义

- VirtualOnly：LVGL 帧只走 PC 虚拟显示；OLED 应用帧禁用，系统诊断页持续刷新。
- PhysicalOnly：Virtual sink 禁用；物理 Application 场景继续收应用帧并更新
  （不依赖 Qt framebuffer）。
- Mirror：同一逻辑帧双扇出（Qt 与 OLED 内容语义一致；不要求像素级一致）。
- Split：Virtual=Application、Physical=Diagnostics 同时存在（M7-C4 的运行时基础）。

### AA.8 背压 / degraded / 生命周期

- 物理 sink 不可用（OLED 未 kReady）→ Router 收敛 kDegraded，Virtual 侧不受影响；
  PhysicalOnly/Mirror 下物理 present 失败只影响自身，聚合仍返回 kOk（任一成功）。
- 共享 1KB fb 锁序单向：router.mutex → fb.mutex；OLED 任务只碰 fb.mutex（无环）。
- 生命周期：PhysicalDisplaySink 非拥有引用，销毁序 sink 先于 OledDisplay（全局
  声明序保证）；M7-B 冻结语义零回归（stop flag / 任务退出释放 I2C / release-acquire）；
  stop 后 present → kNotEnabled/kNotConnected。
- 内存：经典 ESP32 无权威整屏缓冲；仅 1KB 物理 fb + 32B x 8 段 staging +
  LVGL 15360B draw buffer；present 渲染路径零堆分配。

### AA.9 Kconfig / main 装配 / 测试钩子

- CONFIG_ESPVIEW_DEFAULT_MODE（int 0..3，默认 0=VirtualOnly）：上电初始模式，
  经与 SET_MODE 相同的 applyDisplayMode 路径。
- CONFIG_ESPVIEW_TEST_MODE_SWITCH（bool，默认 n）：F11（HID 0x44）运行时循环
  0→1→2→3→0，状态经 ERROR 文本通道 "mod sw=<mode> st=<router state> scene=<scene>"
  上报（非 wire 格式）；生产固件 =n 时 F11 走正常输入链路。
- SET_MODE 白名单 0..3（原 >kMirror 拒绝）；HELLO mode_mask=0b1111。
- OLED_ENABLE=n：physical_display_sink.cpp 不编译、main 不注册/不引用；
  Router 无 physical sink 时需要物理的模式 setMode 返回 kInvalidParam（不崩溃），
  双配置构建无未定义引用。

### AA.10 Host 测试 / 实测

- host：display_router_test 追加 8 个 C2 用例（PhysicalOnly/Mirror/Split 路由、
  physical unavailable→kDegraded、physical 失败不阻塞 virtual、enable/disable、
  scene switch）；physical_renderer_test 追加 golden 用例。全量 224,136 checks /
  0 failures；verify_host.bat ALL PASS。
- 实测（2026-08-15，UART transport + OLED，COM4 @ 115200，LVGL app）：
  OLED `a=0x3C c=SSD1306 err=0 ok=1`；F11 循环验证模式序列 2→3→0→1→2，
  scene 映射 3/0→Diagnostics、1/2→Application 全部正确；HELLO/PING 0 CRC/0 seq。
- 边界声明：本阶段不实现 Qt 四模式 UI / Wi-Fi wizard / 双语 / build tooling /
  OTA / Touch / 压缩 / 协议重设计；wire format 仅 additive kSplit=3。
## AB. M7-C3 Qt 多显示 UI / Split Drawer / 双语界面（2026-08-15 实测；wire format 未修改）

### AB.1 定位

M7-C3 在冻结的 C1/C2 架构上补齐 Qt 侧 UX：四种显示模式（VirtualOnly/PhysicalOnly/
Mirror/Split）的可视化选择与 Apply、Split Drawer（ESP32 物理/诊断侧栏）、中英双语、
Display state 可视化面板、transport-aware display configuration。wire format 零改动
（SET_MODE payload 仍为 [0] mode；无新增 Message；无 OLED framebuffer uplink）。

### AB.2 Display Mode UI 与 0..3 语义（冻结）

- 下拉四模式：Virtual Only(0) / Physical Only(1) / Mirror(2) / Split(3)，用户可见
  文案为模式名，绝不显示 0/1/2/3。
- 语义沿用 AA.7：VirtualOnly=Virtual:Application / Physical:Disabled（OLED 继续
  独立 system diagnostics，不冒充 Physical Application）；PhysicalOnly=Virtual:
  Disabled / Physical:Application（Qt 仍是控制器/状态面板，不关闭）；Mirror=双
  Application（同一逻辑源，不要求像素级一致）；Split=Virtual:Application /
  Physical:Diagnostics（绝不用 "Both" 掩盖语义差异）。
- Apply → SET_MODE（ACK_REQ）→ ACK ok → appliedMode 更新 + FULL resync pending →
  新 FULL 帧 → READY。ACK fail / 超时（30s 看门狗）→ 回退选择到 appliedMode +
  错误提示，不无限重试。
- 断开时允许改变选择，Apply 进入 "Waiting for connection"（不假装成功）；重连且
  FULL 后若 selected != applied 自动补发一次 SET_MODE（单飞行 + 看门狗），失败回退。

### AB.3 Capability-driven UI（PC 侧推断，无 wire 上行）

- 协议无 capability 上行：HELLO mode_mask 是固件编译期常量（0b1111），不得作为
  物理可用证明；SET_MODE ACK ok 只证明"请求被接受"，不证明物理可用。
- PC 侧唯一真实可观测源 = ERROR 文本遥测 `oled a=... c=... err=... ok=`（3s 周期）：
  收到 `ok=1` → physicalAvailable=true → PhysicalOnly/Mirror/Split 可选；未收到 →
  三个物理模式不可选并显示 "(Unavailable)"。默认允许选择全部模式，但状态标签
  一律以遥测为准（无遥测 → Unknown/—，禁显示 OK）。

### AB.4 Safe Switch（复用 M6-D，不重新设计）

- Apply → SWITCHING（Apply 禁用，防重复）→ clear stale display → SET_MODE → ACK
  → FULL resync → 新 FULL COMMIT → READY。30s 看门狗复用 M6-D 机制。
- Display Mode 切换与 Transport 切换正交：模式切换走独立 sendDisplayMode 队列
  （ConnectionManager::sendDisplayMode → SerialWorker 互斥队列 → pumpLoop drain →
  makeSetMode + sendMessage），保留会话，绝不复用 switchTransport（不 stop/join）。
- 模式切换期间（switchingInProgress / fullResyncPending）onFrameReady 不写屏：
  旧模式帧 / 重同步前的 PARTIAL 一律不上屏；首个新 FULL 才恢复显示并收敛状态。
  PhysicalOnly 下无虚拟帧 → 画面保持清屏占位（"虚拟输出关闭"语义）。
- ACK 丢失/发送被丢弃（未连接）→ modeWatchdog 30s 超时回退，Apply 永不永久禁用。

### AB.5 Split Drawer（UI 层布局，非第二套 framebuffer）

- QSplitter(screen_ | drawer_)：VirtualScreenWidget 保持 320x240 逻辑分辨率不变
  （实际 widget 缩放），drawer 宽度 [200, 560]（SplitState 常量，手柄驱动）。
- 第一版只显示 Physical Diagnostics/Status 文本（RSSI/CH/Heap/Frame/Errors/
  Transport/OLED addr/Scene/Availability），数据来自 ERROR 文本遥测 → PhysicalStatus
  解析；绝不在 drawer 伪造 OLED framebuffer —— 当前 wire format 没有 Physical
  framebuffer uplink（协议 gap，第一版明确不实现；未来若需像素级镜像须新增 wire
  Message，属 C4+ 范围）。
- 开合/宽度持久化：split/drawerVisible + split/drawerWidth（300ms 去抖 + 显式保存）；
  Split 选中自动 open()，用户可手动开合（不触发连接/清屏/模式操作）。
- 无数据规则：任何 *Valid==false 字段显示 "—"，禁止用 0/"0.0" 伪造。

### AB.6 状态面板（DisplayStatusPanel）

- 分组：Transport / Session / Display Mode / Virtual / Physical / Router / Errors /
  FULL resync；状态区分 Unavailable / Degraded / Disabled / Active（红/橙/灰/绿）。
- 数据源：Worker statusChanged / statsChanged / diagAdded（解析后喂入）/ 模式模型
  （selectedMode + routerState + fullResyncPending）。GUI 线程只读消费，不改底层
  WorkerStats 结构。

### AB.7 i18n（English + 简体中文）

- 最小目录机制（不用 .ts/.qm）：`i18n.h/.cpp` 提供 trText(UiLang, key)，key 即英文
  原文（msgid 风格），未命中回退英文；LanguageSelector 下拉（English/中文）。
- 语言切换只重刷 UI 文案（registerLabel + retranslateUi + 各 widget setUiLanguage），
  不触碰 ConnectionManager/SerialWorker、不触发重连、不清 framebuffer、不重建 widget。
- 诊断行/动态状态细节保持 ASCII（日志稳定）；UI 框架词（Transport/Display/模式名/
  Apply/状态面板分组等）双语覆盖。持久化键 ui/language（白名单内）。

### AB.8 QSettings 扩展（白名单统一管理）

- persistedSettingsKeys() 9 键：transport/type, uart/port, uart/baud, tcp/port,
  window/size, display/mode, ui/language, split/drawerVisible, split/drawerWidth。
- 绝对禁止保存 Wi-Fi SSID/密码/PSK/凭据（结构性保证：白名单键名不含
  wifi/ssid/password/psk/credential；TransportConfig 无凭据字段）。

### AB.9 Wi-Fi Wizard 边界

- 本阶段只预留入口语义：Settings → Wi-Fi Setup 提示 "将在 M7-C5 实现"；只展示
  当前 Wi-Fi 状态（RSSI/CH/trx 遥测）。禁止实现 PC 下发 SSID/Password/TCP server
  IP/Port —— 协议无对应 Message；发现需新控制消息即报告 PROTOCOL GAP，不得自改。

### AB.10 线程模型 / 大帧

- GUI 线程：Widget/布局/翻译/状态展示/模型（DisplayUiState/SplitState 纯 C++17，
  零锁）；Worker 线程：Transport/Protocol/FrameAssembler/遥测文本上行解析边界。
- 大帧 153600B 仍只在 Worker 组装 + queued 投递；GUI 每帧一次 blit+paint；
  drawer/状态面板纯 label 赋值，无第二份整帧转换（禁止 GUI 侧再拷贝 153KB 级缓冲）。

### AB.11 Host 测试（M7-C3）

- display_ui_state_test（139 checks）：四模式 / physical unavailable / degraded /
  disconnected / switching / FULL resync pending / apply disabled during switching。
- physical_status_test（158 checks）：六类遥测行解析、混合合并、坏行忽略、clamp。
- split_state_test（68 checks）：开合 / 夹取 200..560 / 键值往返 / 非法值容错。
- i18n_test（744 checks，独立可执行）：English/Chinese/纯函数/全 key 双语非空 +
  必需词条钉死。
- transport_config_test 白名单 5→9 键断言更新。全量 224,501 checks / 0 failures
  （协议套件）+ 91（transport_config）+ 744（i18n）；verify_host.bat ALL PASS。

### AB.12 Qt 构建 / 实测（2026-08-15 真实硬件，UART transport，COM4 @ 115200）

- verify_qt.bat ALL PASS；espview_virtual_display.exe 全量 clean build 零警告
  （-Wall -Wextra -Wpedantic）。
- 实测：UART HELLO 320x240 握手 PASS；FULL 帧渲染（PNG dump 验证 LVGL 画面）；
  OLED 遥测 `a=0x3C c=SSD1306 err=0 ok=1` 持续解析（physicalAvailable=true）；
  F11 模式循环 2→3→0→1→2，mod 行 st/scene 与 AA.3 映射一致
  （3/0→Diagnostics、1/2→Application）；中英文启动（ui/language=0/1）exit 0。
- 回归：M0..M7-C2 全部 host 测试通过；verify_host/verify_qt 通过。

### AB.13 边界声明

- 未实现：OLED 像素级镜像（wire gap，需新增 ESP32→PC Message，属 C4+）、Wi-Fi
  credential 传输（C5）、C5 wizard、OTA/Touch/压缩。本阶段不修改 wire format、
  不改 OLED renderer、不重新实现 PhysicalDisplaySink。

============================================================
AC. M7-C4 Qt UX / Capability / Physical Diagnostics semantics（2026-08-15 实测；wire format 未修改）
============================================================

### AC.1 定位

M7-C4 在冻结的 C3 基础上做 UX/能力/诊断产品化：PhysicalCapabilitySnapshot
（能力/健康分离收敛点）、Display Mode 卡片化 UI、Split Drawer 三区侧栏、
i18n 完整化、Transport/Display switch 互斥、会话 epoch 帧门控、诊断环加锁。
wire format 零改动（无新增 Message；无 Physical framebuffer uplink；不修改
Packet Header/CRC/Frame 语义）。

### AC.2 Capability policy（PC 侧收敛，无 wire 上行）

- 协议 v0.1 无 capability uplink：HELLO mode_mask 是固件编译期常量，SET_MODE
  ACK ok 只证明"请求被接受"。PC 侧唯一真实可观测源 = ERROR 文本遥测 oled 行。
- 新增 `shared/display/physical_capability_snapshot.{h,cpp}`（纯 C++17 值类型，
  espview_display 静态库）：单一收敛点，消费方不再各自 `if (oled ok)`。
  语义分层：
  - `capabilityKnown` —— 本会话曾见 `oled ok=1`（学习结果，只置位）；管门控；
  - `healthy` —— 最近遥测 oledOk（动态）；管降级；
  - `telemetryFresh` —— 会话内最近收到过 oled 行（stale 判定）；
  - `provenance` —— 当前 kOledTelemetry；为未来 CAPABILITIES 上行预留扩展点。
- 修复 M7-C3 一次性闩锁缺陷：断开（Disconnected/Error）→ `resetPhysicalCapability()`
  撤销学习结果（跨会话残留清除）；健康降级（ok=0）→ `onPhysicalDegraded` 走
  routerState=kDegraded，不撤销 capabilityKnown（门控不随健康抖动）。
- 分辨率/格式推断：SSD1306/SH1106 → 128x64、mono=1bpp、canReadback=false
  （与 ESP32 侧 PhysicalDisplaySink::init 落定值一致；标注为推断 provenance，
  不冒充设备声明）；未知控制器 → 0x0（不伪造）。
- 能力/健康接线映射：main.cpp onDiagAdded → snapshot 派生 → capabilityKnown
  上升沿调 `onPhysicalAvailable(true)`；known 后 `onPhysicalDegraded(!healthy)`。
  status_panel / split_drawer 仍直读 PhysicalStatus valid 标志（各自正确，
  不做第二套推断）。

### AC.3 Display UI state machine（冻结 + P1 补强）

- 状态：selectedMode / appliedMode / routerState / physicalAvailable /
  sessionConnected / switchingInProgress / fullResyncPending / applyEnabled /
  waitingForConnection / pendingInterruptedApply / lastError。
- 新增 `pendingInterruptedApply`（P1-1）：在线 Apply 在飞（SET_MODE 已发、ACK
  未回）被断线打断 → onDisconnected 记录；重连后 needsAutoSend 条件扩展为
  `(waitingForConnection || pendingInterruptedApply) && selected != applied`，
  自动补发一次（单飞行 + 30s 看门狗），消除"切换意图静默丢失"。
- UI 呈现（display_mode_widget 卡片化）：每模式显示名称 + 描述 + Virtual 侧 +
  Physical 侧状态；selected != applied 时橙色加粗差异（Selected/Applied/Router
  三行），绝不只显示单个 Mode 值。
- 门控：`physicalAvailable=false` → 物理相关模式不可选（Unavailable）；
  `switchingInProgress` → Apply 禁用；`fullResyncPending` → 只收 FULL。

### AC.4 Safe display switch / Transport 互斥（§六）

- Display switch（VirtualOnly↔PhysicalOnly↔Mirror↔Split）：独立 sendDisplayMode
  队列 + ACK + 30s 看门狗 + FULL resync；保留会话，不复用 switchTransport。
- Transport switch（UART↔TCP）：M6-D 语义保持（stop/join → 新 transport →
  FULL 判 CONNECTED）。
- 互斥：beginSwitch 期间强制 Display Mode Apply 禁用（两套 switch transaction
  不打架）；abortSwitch / 完成恢复由 DisplayUiState 状态机收敛。语言切换 /
  Drawer resize / QSettings load 不触发 reconnect、不需要 FULL。

### AC.5 Session epoch 帧门控（P1-2）

- DisplayFrame 增加 `sessionId`（传输会话 epoch）；SerialWorker 每次 runLoop
  入口 `sessionId_ = ++sessionCounter_`（原子），onFrameCommit 打戳。
- GUI：beginSwitch 置 currentSessionId_=0，Connected 时更新为
  manager_.sessionId()；onFrameReady 只处理 `frame.sessionId==currentSessionId_`
  的帧 —— 消除 transport switch 后旧会话 stale FULL 伪造"CONNECTED (FULL
  resync done)"的误判（TCP 下 ~2% 量级）。

### AC.6 DiagnosticsRing 线程安全（P1-3）

- `shared/protocol/runtime_stats.h/.cpp`：DiagnosticsRing 全访问（push/clear/
  size/items/last）加 mutex；修复 transport RX 线程（setStateCallback）与
  worker 线程并发 pushDiag 的无锁 deque 并发写 UB。last() 返回指针仅限测试/
  单线程调试（生产无并发调用）。

### AC.7 Split Drawer（产品化，Diagnostics only）

- QSplitter 右侧三区文本面板：Physical Display（Controller/Resolution/I2C/
  State/Scene/Last Flush/Errors）、Wi-Fi/TCP（SSID/RSSI/Channel/IP/TCP
  status/Session/Frame/Heap）、Session/State（彩色横幅 + Last refresh + stale）。
- 行为：标题栏收起、拖拽宽度 [200,480]（Qt 层；SplitState 模型保持 [200,560]
  超集）、DPI aware（QFontMetrics 推导最小宽，无硬编码像素阈值）、QScrollArea
  纵向滚动、300ms 去抖持久化 split/drawerVisible + split/drawerWidth。
- stale 判定：kStaleThresholdMs=5000（遥测 ≥1Hz，5s 无数据即 Stale）；
  1Hz 检查；clearStatus 清除时间戳。
- 性能：setPhysicalStatus 只存快照 + ≥200ms 节流（实际写 label ≤5Hz）；
  禁止 50Hz 重绘；无 QImage copy storm。
- 不伪造：本阶段不绘制 OLED 像素（见 AC.10 决策）。

### AC.8 i18n（完整化）

- trText(key) 目录机制（key=英文原文；未知 key 回退英文）：217 key 英文 +
  217 key 中文（i18n_test 1296 checks）。WorkerStats 动态统计、连接/切换/
  重连状态、错误原因、Drawer/面板/模式 UI 全部走目录；无硬编码英文格式串。
- 运行时切换：只重翻译 UI；不 disconnect/reconnect/switch mode/clear
  framebuffer（路径审计确认：onLanguageChanged → retranslateUi，无 manager_
  调用、无 screen_->clearDisplay）。

### AC.9 QSettings

- 白名单 9 键保持（transport/type、uart/port、uart/baud、tcp/port、window/size、
  display/mode、ui/language、split/drawerVisible、split/drawerWidth）；无凭据键
  （结构性保证）。QSettings load 不触发 reconnect（restoreDisplayModeFromSettings
  只改 selectedMode，能力未知前仅 VirtualOnly 可恢复）。

### AC.10 Physical preview 决策（正式冻结）

- **Path A（本阶段冻结）**：Split Drawer = Physical Diagnostics/Status only。
  当前 wire format 不携带 Physical framebuffer uplink（ESP→PC 数据面仅 HELLO、
  FRAME_*（Virtual RGB565）、ERROR 文本遥测、PING/PONG/ACK；PixelFormat 无
  1bpp 类型；OLED canReadback=false 冻结），因此无法在 Drawer 显示真实 OLED
  像素，也不伪造预览。
- **Path B（未来，C4+ 输入）**：PHYSICAL_PREVIEW 建议消息（TYPE 0x13 空槽；
  payload = frameId u16 + width u16 + height u16 + pixelFormat u8（kMono1 新枚举）
  + flags u8 + pixels[1024]，共 1032B < 4096 单包，无需 CHUNKED；fire-and-forget
  （与 FRAME_* 数据面一致）；每帧完整快照自重同步；带宽：UART 115200 ≈ 91ms/帧
  （1fps 占 ~91% 链路）、921600 ≈ 11.4ms/帧、TCP ≈ 1.7ms/帧；ESP32 侧 1KB 权威
  fb 免重采样）。本阶段不实施、不修改 wire。

### AC.11 Wi-Fi UI boundary

- 禁止：SSID/Password/TCP server IP/Port 下发（协议无对应 Message；C5 解决）。
- 允许：Wi-Fi Status 展示（SSID 当前网络名或安全占位、RSSI、Channel、IP、
  TCP 状态）；绝不显示密码、不新增 credential storage。

### AC.12 Host 测试（M7-C4）

- physical_capability_snapshot_test（+55 checks）：初始/学习（SSD1306→128x64）/
  健康降级保持门控/跨会话撤销/未知控制器不伪造/场景派生/学习单调性。
- display_ui_state / physical_status / split_state 扩展（+221 checks，G 代理）：
  selected!=applied、capability gate、watchdog 代理、open/close/resize/persist/
  restore、stale/disconnected diagnostics 等（任务书 §十六 30 项矩阵覆盖）。
- 协议套件最终 224,777 checks / 0 failures；transport_config 91/0；
  i18n_test 1296/0；verify_host.bat ALL PASS。

### AC.13 Qt / ESP32 构建与实测（2026-08-15）

- verify_qt.bat ALL PASS；espview_virtual_display.exe clean build 零警告
  （-Wall -Wextra -Wpedantic）；offscreen 冒烟 --autoclose-ms 2000 EXIT 0。
- ESP32（uart_hw 配置）idf.py build 通过（shared 修改在 ESP-IDF 侧编译 OK）。
- 真实硬件（COM4 @ 115200，OLED 0x3C，UART 验证固件，Agent H 基线）：
  - 四模式矩阵：VirtualOnly/PhysicalOnly/Mirror/Split 各 ×5 连续切换（F11 钩子
    20 次 + 每模式 offscreen Qt 会话），全 st=2(Connected)，scene 映射
    0/3→Diagnostics、1/2→Application 全部正确；Qt 侧验证：PhysicalOnly 0 FULL
    PNG（No signal）、其余模式各 1 张 FULL PNG。
  - Input 回归：矩阵 tx=300（含 40×F11）→ ESP32 rx +280 精确（F11 被调试钩子
    拦截不计入），0 丢失；长稳 tx=39 → +25 精确；mouse/key/wheel 切换前后无粘滞。
  - 长稳：UART 连续 660.2s（11.0 分钟）：0 crash、0 decoder/CRC/seq 错误
    （ESP32 侧 sess2 e=0 c=0 s=0）、14 次切换均 FULL 提交、仅 1 帧背压丢弃、
    heap free 156.5–167.0 KB 振荡无持续下降、OLED err=0 ok=1 全程。
    已知：PC RX 11 分钟内 1 次瞬态 seq-gap（CH340 丢 1 包，会话自恢复，可接受）。
  - TCP：当前 uart_hw 基线固件 ESPVIEW_TRANSPORT_TCP=false，TCP 不可用
    （如实记录；TCP 生产固件验证留待后续里程碑）。

### AC.14 边界声明

- 未实现：OLED 像素级预览（wire gap，AC.10 Path B 冻结为 C4+）、Wi-Fi
  credential（C5）、OTA/Touch/压缩。不修改 wire format、不改 OLED renderer、
  不重新实现 PhysicalDisplaySink/DisplayRouter/ProtocolEndpoint/FrameAssembler。

============================================================
AD. M7-D1 CAPABILITIES（2026-08-15 设计冻结；wire additive，Packet Header/CRC/既有消息未修改）
============================================================

### AD.1 定位

M7-D1 正式冻结 CAPABILITIES（TYPE 0x02）payload layout 并实现两端：
ESP32 在每会话 HELLO 握手后发送一次能力快照；PC 解析并消费。
wire format 只做 additive：不修改 Packet Header / CRC / 既有消息 payload /
Frame 语义；HELLO layout 不动；SET_MODE 0..3 语义不动。

### AD.2 CAPABILITIES payload（v0.1，定长 32 字节，多字节 LE）

| 偏移 | 宽度 | 字段 | 取值 | 语义 |
|---|---|---|---|---|
| 0 | 1 | version | 0x01 | payload 版本（独立于 kProtocolVersion；0x00 或 >0x01 → 丢弃） |
| 1 | 1 | flags | bit0=virtualPresent bit1=physicalPresent | sink 存在位（派生自真实 sink 状态，非编译期常量） |
| 2 | 2 | width | 1..4096 | 虚拟几何宽（与 HELLO 对齐；320） |
| 4 | 2 | height | 1..4096 | 虚拟几何高（240） |
| 6 | 1 | pixelFormat | 0=RGB565 | 虚拟像素格式（v0.1） |
| 7 | 1 | colorDepth | 16 | 虚拟色深 bpp |
| 8 | 1 | virtualMono | 0/1 | 虚拟单色标志（false） |
| 9 | 1 | virtualCanReadback | 0/1 | 虚拟回读支持（true） |
| 10 | 1 | modeMask | bit0..3=WINDOW/DEVICE/MIRROR/SPLIT | 显示模式掩码（硬件派生，禁止编译期常量冒充） |
| 11 | 5 | rsvd | 0 | 保留（发送方填 0；接收方忽略） |
| 16 | 2 | physWidth | 1..4096；0=未知 | 物理宽（OLED=128） |
| 18 | 2 | physHeight | 1..4096；0=未知 | 物理高（OLED=64） |
| 20 | 1 | physPixelFormat | 0=RGB565 / 1=Mono1 | 物理像素格式（OLED=1） |
| 21 | 1 | physColorDepth | 1 | 物理色深 bpp |
| 22 | 1 | physMono | 0/1 | 物理单色（OLED=true） |
| 23 | 1 | physCanReadback | 0/1 | 物理回读（OLED=false） |
| 24 | 1 | physController | 0=AUTO 1=SSD1306 2=SH1106 0xFF=UNKNOWN | 物理控制器枚举（对齐 shared/oled ControllerType 与 OledControllerCode） |
| 25 | 1 | physI2cAddress | 0x00=未知 | I2C 7-bit 地址（0x3C 等） |
| 26 | 1 | sceneSupport | bit0=kApplication bit1=kDiagnostics | 物理场景支持（v0.1=0b11） |
| 27 | 5 | rsvd2 | 0 | 保留（发送方填 0；接收方忽略） |

合计 32 字节 < 4096：单包，无需 CHUNKED。

### AD.3 ACK / 兼容性 / 版本策略

- ACK：**不带 ACK_REQ**（fire-and-forget）。每会话 CONNECTED 后发送一次，
  重连重发；重复按刷新处理。理由：旧 PC 对未知控制消息静默丢弃，带 ACK_REQ
  只会产生无意义重试噪音；能力快照丢失由下一会话补发，无重试价值。
- 旧 PC ↔ 新 ESP：0x02 类型合法、CRC 通过、不 failSession；旧 PC 静默丢弃。
- 新 PC ↔ 旧 ESP：旧设备不发 CAPABILITIES → PC 保持 oled 遥测推断 +
  「Capability unavailable」展示；**不得**因 HELLO mode_mask 或 SET_MODE ACK ok
  自动放行 OLED/Split。只有明确 capability snapshot 才能启用对应 UI。
- 解析规则：短于 32B 丢弃（诊断计数）；长于 32B 忽略尾部；version≠0x01 丢弃；
  未知枚举白名单映射到 UNKNOWN（0xFF），杜绝 UI 数值注入。

### AD.4 Host 模型映射

- ESP32 发送侧：DisplayCapabilities（display_capabilities.h，sink init 事实）→
  wire 全直接：width/height→physW/H、format→physPixelFormat、color→physColorDepth、
  mono→physMono、canReadback→physCanReadback、sinkKind→flags 存在位、controller/
  i2cAddress 来自 OledFb 常量（SSD1306/0x3C）、sceneSupport=0b11。
- PC 消费侧：PhysicalCapabilitySnapshot（physical_capability_snapshot.h）——
  capabilityKnown←physicalPresent；width/height/mono/canReadback/controller/address/
  scene 由「推断/遥测」升级为 wire 直接映射；healthy/telemetryFresh 仍属遥测
  （能力≠健康）；provenance 追加 kCapabilitiesMessage。

### AD.5 边界

- 不修改：Packet Header、CRC、HELLO、SET_MODE、Frame 语义。
- CAPABILITIES 是 additive：老固件无该消息时 Qt graceful fallback（遥测推断）。
- 本消息不含任何凭据。

### AD.6 已实现（M7-D1，2026-08-15）

- shared/protocol：新增 `kCapabilitiesPayloadVersion=0x01`；`CapabilitiesInfo` +
  `makeCapabilities()`/`parseCapabilities()`（32B 定长 LE；<32B 丢弃、>32B
  忽略尾部、version≠0x01 丢弃、未知 controller/format 白名单映射 UNKNOWN）。
- ProtocolEndpoint：`kCapabilities` 分派 `handleCapabilities`（非法 payload
  仅计数 `capabilitiesDropped`，不 failSession）；`onCapabilities` 回调；
  `sendCapabilities()`（requireConnected=true，fire-and-forget）；统计
  rx/txCapabilities。
- ESP32：CONNECTED 后每会话发送一次（重连重发）；字段全部硬件派生（sink
  存在位、DisplayCapabilities 几何/格式、OledFb 常量、oled status 运行时
  事实；modeMask=physicalPresent?0x0F:0x01，无编译期冒充）。
- PC：SerialWorker → ConnectionManager 信号转发；main.cpp 消费更新
  PhysicalCapabilitySnapshot（provenance=kCapabilitiesMessage、
  capabilityKnown=physicalPresent）；未收到 CAPABILITIES 时维持遥测推断
  fallback。
- 验证：host 224,899 checks / 0 failures；Qt 构建通过；ESP32 构建通过。

============================================================
AE. M7-D2 PHYSICAL_PREVIEW（2026-08-15 设计冻结；wire additive）
============================================================

### AE.1 定位

M7-D2 冻结 PHYSICAL_PREVIEW（TYPE 0x13）payload layout：ESP32 把 OLED
（SSD1306 128x64 1bpp）当前内容作为诊断/UX 预览帧上行到 PC Split Drawer。
Preview 是诊断/UX 特性，**不是第二权威 framebuffer**：ESP32 Application
authority 不变；不改 PhysicalDisplaySink/DisplayRouter 渲染决策；
caps.canReadback 保持 false。wire additive：不修改 Packet Header/CRC/
既有消息/Frame 语义。

### AE.2 PHYSICAL_PREVIEW payload（v0.1，1032 字节，多字节 LE）

| 偏移 | 宽度 | 字段 | 取值 | 语义 |
|---|---|---|---|---|
| 0 | 2 | frameId | 0..65535 回绕 | 预览帧序号；PC 侧 `(int16_t)(id-last)>0` 丢弃乱序/过期 |
| 2 | 2 | width | 128 | 物理宽（v1 恒 128；解析方以本字段为准） |
| 4 | 2 | height | 64 | 物理高（v1 恒 64） |
| 6 | 1 | pixelFormat | 1=Mono1（新增 kMono1=1；HELLO 布局不改） | 1bpp 页式 |
| 7 | 1 | flags | bit0=0（v1 恒 FULL，预留增量） | 帧类型位 |
| 8 | 1024 | pixels | 页式 1bpp | 与 OledFb::data() 逐字节一致的快照 |

合计 1032B < 4096：单包，Encoder 恒产出 1 个 CHUNKED=0 包，恰消耗 1 个 SEQ。

### AE.3 传输语义

- CHUNKED：**不需要**（1032B 单包）。
- ACK：**fire-and-forget**（与 FRAME_* 数据面一致；ACK 只服务控制消息；
  丢帧自重同步、背压整帧丢弃已覆盖；重试引入乱序/重复无收益）。
- 序列：复用 Packet SEQ（字节流连续性）；frameId 做消息级去重/过期；
  握手时双方清零；断线重连 PC 清 lastFrameId 后首帧无条件接受。
- 频率：上限 10 Hz，默认 2 Hz（对齐 OLED 500ms 刷新节拍）；
  UART 115200 → 1 Hz（占空比 9.1%/fps；勘误 AC.10 的 "1fps 占 91%" 应为
  9.1%/fps，91% 对应 10fps）；921600 → 5 Hz；TCP → 5–10 Hz。
- 内存：复用 ESP32 权威 1KB fb 免重采样；OLED 任务在内容确定点做 1KB
  短拷贝进预览槽（最新帧合并，不阻塞、不触碰 transport）；发送在会话侧
  任务；峰值额外 ≈ +3KB（槽 1KB + payload 1032B + 包缓冲 1052B 复用）。
- 重连/断线：ESP32 仅 CONNECTED 发送，握手重置 frameId/清槽；PC 断线
  清空预览位图，重连等首帧 FULL。
- DisplayMode 交互：Preview 只镜像面板当前内容（Diagnostics→诊断页 fb_，
  Application→appFb_），不改变真实 Physical output；任何模式下 Preview 均可
  作为 Drawer 辅助观察。

### AE.4 安全

载荷 = 诊断页或 1bpp 应用缩略帧，无凭据（StatusSnapshot 无 SSID/密码）；
与 Virtual 帧同等级明文，不引入新暴露面。

### AE.5 边界

- 不修改：Packet Header、CRC、HELLO、SET_MODE、Frame 语义、DisplayCapabilities。
- Preview 使能由 PC 侧 UI 控制（QSettings preview enabled），ESP32 侧默认关闭
  或按 Kconfig 默认开启需在 D2 实现时定夺并记录。
- 本消息不含任何凭据。

### AE.6 已实现（M7-D2，2026-08-15）

- shared/protocol：新增 `kPhysicalPreview=0x13`；`PhysicalPreviewInfo` +
  `makePhysicalPreview()`/`parsePhysicalPreview()`（1032B 定长 LE；<1032B
  丢弃、>1032B 忽略尾部、未知 pixelFormat 兜底 kMono1）。
- ProtocolEndpoint：`kPhysicalPreview` 分派（非法 payload 仅计数
  `physicalPreviewDropped`，不 failSession）；`onPhysicalPreview` 回调
  （info + pixels 指针，回调期间有效）；`sendPhysicalPreview()` 走
  tryTransmit（非阻塞；锁忙/背压整帧丢弃，AE.3）；统计 rx/txPhysicalPreview。
- shared/oled：`OledPreviewSlot`（seqlock 无锁；store/snapshot/reset/
  makePhysicalPreviewPayload）；`OledDisplay` 持有预览槽并在 taskLoop 两个
  内容确定点 store（应用帧 memcpy 后、诊断页 renderStatus 后）。
- ESP32：`esp32/components/oled` 复用 oled_preview.cpp；main.cpp 在
  sessionLoop 以 500ms（默认 2Hz）节拍、仅 CONNECTED 时经
  makePhysicalPreviewPayload → sendPhysicalPreview 上行；握手/断线 reset 槽。
- PC：SerialWorker 持有 PhysicalPreviewState（worker 线程写：会话/断线
  语义 + setFrame 去重）→ previewFrame 信号（queued）→ ConnectionManager
  转发 → main.cpp 接线 PhysicalPreviewWidget（Split Drawer 顶部，经
  SplitDrawer::addExternalWidget）；i18n 词条 + `ui/previewEnabled`
  QSettings 白名单键。
- 验证：host 383,672 checks / 0 failures；Qt 构建通过；ESP32 构建通过。
  真实硬件 OLED 预览验收见 M7-E 硬件阶段。

============================================================
AF. M7-D3 Wi-Fi Provisioning Protocol（2026-08-15 设计冻结；wire additive）
============================================================

### AF.1 定位

M7-D3 冻结 Wi-Fi 配置协议族（4 个新增 TYPE，全部单包 ≤4096B、additive；
VERSION 保持 0x01；复用既有 ACK（500ms×2 重试、单槽串行）/ERROR/CHUNKED
机制）。目标：UART bootstrap → 扫描 → 配置 SSID/password/TCP server →
GOT_IP → TCP handoff。**凭据只经 UART bootstrap 下发，绝不经 TCP 下发。**

### AF.2 消息族与 Wire layout

**WIFI_SCAN_REQ（0x06，PC→ESP，必须 ACK_REQ，2B）**
| 偏移 | 宽度 | 字段 | 语义 |
|---|---|---|---|
| 0 | 1 | flags | bit0..7 保留（0） |
| 1 | 1 | maxEntries | 0=默认 32；上限 64 |

**WIFI_SCAN_RESULT（0x07，ESP→PC，fire-and-forget，≤2693B）**
| 偏移 | 宽度 | 字段 | 语义 |
|---|---|---|---|
| 0 | 1 | scanSeq | 扫描序号（响应 REQ 计数，回绕） |
| 1 | 1 | count | records 数（0..maxEntries） |
| 2 | 1 | flags | bit0=truncated |
| 3 | 2 | total | 总可见 AP 数（含截断） |
| 5 | count×42 | records | ssid[32] NUL 填充 + bssid[6] + rssi(1) + channel(1) + authmode(1，ESP-IDF 0..8) + rsvd(1)；按 RSSI 降序 top-N |

**WIFI_CONFIG（0x08，PC→ESP，必须 ACK_REQ，103B）**
| 偏移 | 宽度 | 字段 | 语义 |
|---|---|---|---|
| 0 | 1 | flags | bit0=CLEAR（清除凭据并断开）；其余保留 |
| 1 | 32 | ssid | UTF-8 定长 NUL 填充（1..32 字节） |
| 33 | 64 | password | 0 长度=开放网络；8..63 字节 passphrase；64-hex raw-PSK 预留 |
| 97 | 4 | serverIp | 网络序 IPv4；禁 0.0.0.0 |
| 101 | 2 | serverPort | LE u16，1..65535 |

**WIFI_STATUS（0x09，ESP→PC，fire-and-forget，≤49B）——绝无密码字段**
| 偏移 | 宽度 | 字段 | 语义 |
|---|---|---|---|
| 0 | 1 | phase | IDLE/SCANNING/CONFIG_APPLYING/WIFI_CONNECTING/WIFI_CONNECTED/GOT_IP/TCP_CONNECTING/TCP_CONNECTED/ERROR/CLEARED |
| 1 | 2 | errorCode | 复用 kInvalidParam/kBusy/kInternal + 新增 5..12（scan/auth/AP-not-found/DHCP-timeout/not-configured/server-unreachable/storage/api） |
| 3 | 1 | flags | 保留 |
| 4 | 1 | rssi | dBm（-128=无） |
| 5 | 1 | channel | 0=无 |
| 6 | 4 | ip | 本机 IPv4（网络序） |
| 10 | 4 | serverIp | 目标 server IPv4（网络序） |
| 14 | 2 | serverPort | LE |
| 16 | 1 | ssidLen | 0..32 |
| 17 | ssidLen | ssid | 当前网络名（非 secret metadata） |

错误码新增：5=kScanFailed 6=kAuthFailed 7=kApNotFound 8=kDhcpTimeout
9=kNotConfigured 10=kServerUnreachable 11=kStorageError 12=kApiError。

### AF.3 传输/ACK 语义

- WIFI_SCAN_REQ / WIFI_CONFIG：必须 ACK_REQ（控制消息）；ACK ERR → 不上报
  成功、不落存储；PC 探针（0x06）ACK OK 后才发真实凭据（0x08）。
- WIFI_SCAN_RESULT / WIFI_STATUS：fire-and-forget（状态流，无重试价值）。
- 兼容性：老固件对未知 ACK_REQ 确定性回 ACK ERR kInvalidParam → PC 探针失败
  即提示"固件不支持 Wi-Fi provisioning"，优雅降级，**不发真实凭据**。

### AF.4 凭据生命周期（安全冻结）

- **默认 RAM-only**：断电即失，无 at-rest 秘密；Wi-Fi 重连不需重启
  （esp_wifi_stop→set_config→start→connect）。
- NVS 明文持久化（"上电免配网"）为显式选项：当前无 flash encryption，属
  **plaintext at-rest**（esp_partition_read 可读），须记录风险 + 提供 CLEAR
  动作；不推荐默认启用。本阶段不实现 NVS 持久化。
- password 永不进：日志 / ERROR 文本遥测 / RuntimeStats / 诊断 / QSettings /
  DESIGN / git / PNG dump / UI（仅可记录长度）。
- PC：对话框输入（禁 CLI 参数）→ 专用内存构造 → ACK 成功或耗尽后立即
  secureErase → 不持久化。SSID 可选保存（非 secret metadata），默认不保存。
- ESP32：接收后复制到 RAM 并清零消息缓冲；应用后清零副本；重连不重发；
  CLEAR（flags bit0）清除凭据并断开。
- **明文风险声明**：SSID/password 经 UART（及未来任何 TCP 下发）为明文载荷
  （CRC32 仅完整性非加密）。**M7-D 只能做局域网开发工具用途，不能宣称安全
  provisioning；不实现 TLS。** UART bootstrap 配网完成后建议断开物理通道；
  威胁模型限于可信开发局域网。
- TCP server 暴露面：PC TCP server 默认建议 bind 127.0.0.1（Peer token 机制
  留待后续），凭据路径锁定 UART-only。

### AF.5 边界

- 不修改：Packet Header、CRC、HELLO、SET_MODE、Frame 语义、CAPABILITIES。
- 本阶段不实现 NVS 持久化、不实现 TLS、不做 AP outage 测试（用户明确禁止）。
- 新增错误码 5..12 为 additive 扩展（不影响既有 0..4）。
