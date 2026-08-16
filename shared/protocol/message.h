// ESPView — Message 层：逻辑消息及其载荷构造（M0-B1）
//
// 规范来源：docs/DESIGN.md E 节「消息表 / 帧消息 Payload Layout / 控制消息 Payload Layout」。
// Message = 一条逻辑协议消息（TYPE + FLAGS + 完整载荷）。
// 本文件只负责"按 DESIGN.md 布局序列化载荷"；Message → Packet 的拆分见 encoder.h。
// 注意：SET_RESOLUTION / SET_PIXEL_FORMAT / INPUT_TOUCH / RESET 在
// DESIGN.md 中没有 payload layout（可选/未来），因此不提供专用 builder，只能用 makeMessage。
// CAPABILITIES 的 payload layout 已由 M7-D1 AD.2 冻结，专用 builder/解析器见下文。
// 纯 C++17，零平台依赖。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protocol.h"

namespace espview {
namespace proto {

// 一条逻辑消息（载荷 <= kMaxPacketPayload 时为 1 个 Packet；超过则拆包，见 MessageEncoder）。
struct Message {
    uint8_t type = 0;              // Header.TYPE（MessageType）
    uint8_t flags = 0;             // Header.FLAGS；CHUNKED 位由 Encoder 管理，调用方勿手填
    std::vector<uint8_t> payload;  // 完整消息载荷（按各消息 Layout 序列化）
};

// 流式消息头（M1-3C）：等价 Message 的 type/flags，但不携带 payload。
// 用于 Streaming Message API —— payload 由 IMessagePayloadSource 按需产生，
// 避免大型 Message（如 153608B FRAME_RECT）整段驻留内存。
struct MessageHeader {
    uint8_t type = 0;   // Header.TYPE（MessageType）
    uint8_t flags = 0;  // Header.FLAGS；CHUNKED 位由 Encoder 管理，调用方勿手填
};

// 通用构造（用于没有专用 builder 的消息类型，或自定义载荷）。
Message makeMessage(uint8_t type, uint8_t flags, std::vector<uint8_t> payload);

// ---- 控制消息（DESIGN.md「控制消息 Payload Layout」）----

// HELLO (0x01)：protocolVersion, deviceClass, width(1..4096), height(1..4096),
// pixelFormat, modeMask, deviceName(0..32 字节)。违规返回 nullopt。
std::optional<Message> makeHello(uint8_t protocolVersion, uint8_t deviceClass,
                                 uint16_t width, uint16_t height, PixelFormat pixelFormat,
                                 uint8_t modeMask, std::string_view deviceName);

// SET_MODE (0x03)：mode。按 DESIGN.md「必须 ACK_REQ」自动置 kFlagAckReq。
Message makeSetMode(DisplayMode mode);

// ---- CAPABILITIES (0x02)（M7-D1 AD.2 冻结 payload；v0.1 定长 32 字节 LE）----

// AD.2：payload 定长（< 32B 丢弃；> 32B 忽略尾部）。
inline constexpr size_t kCapabilitiesPayloadSize = 32;

// 物理控制器枚举（AD.2 physController：0=AUTO 1=SSD1306 2=SH1106 0xFF=UNKNOWN；
// 数值对齐 shared/oled ControllerType 与 display OledControllerCode）。
enum class CapabilitiesController : uint8_t {
    kAuto = 0,
    kSsd1306 = 1,
    kSh1106 = 2,
    kUnknown = 0xFF,
};

// 物理像素格式（AD.2 physPixelFormat：0=RGB565 1=Mono1；虚拟侧仍用 proto::PixelFormat）。
enum class PhysicalPixelFormat : uint8_t {
    kRgb565 = 0,
    kMono1 = 1,
};

// 场景支持位（AD.2 sceneSupport：bit0=kApplication bit1=kDiagnostics）。
inline constexpr uint8_t kSceneSupportApplication = 0x01;
inline constexpr uint8_t kSceneSupportDiagnostics = 0x02;

// C++17 只读字节区间视图（std::span 为 C++20；shared/protocol 保持 C++17
// 零平台依赖）。语义与 std::span<const uint8_t> 对齐：data()/size()/operator[]。
class BytesView {
public:
    BytesView() = default;
    BytesView(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    BytesView(const std::vector<uint8_t>& v) : data_(v.data()), size_(v.size()) {}
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    const uint8_t& operator[](size_t i) const { return data_[i]; }
    bool empty() const { return size_ == 0; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

// CAPABILITIES 解析结果（AD.2 字段全集；枚举字段已按白名单映射，见 parseCapabilities）。
struct CapabilitiesInfo {
    uint8_t version = 0;
    bool virtualPresent = false;
    bool physicalPresent = false;
    uint16_t width = 0;
    uint16_t height = 0;
    PixelFormat pixelFormat = PixelFormat::kRgb565;
    uint8_t colorDepth = 0;
    bool virtualMono = false;
    bool virtualCanReadback = false;
    uint8_t modeMask = 0;
    uint16_t physWidth = 0;
    uint16_t physHeight = 0;
    PhysicalPixelFormat physPixelFormat = PhysicalPixelFormat::kRgb565;
    uint8_t physColorDepth = 0;
    bool physMono = false;
    bool physCanReadback = false;
    CapabilitiesController physController = CapabilitiesController::kUnknown;
    uint8_t physI2cAddress = 0;
    uint8_t sceneSupport = 0;
};

// ---- PHYSICAL_PREVIEW (0x13)（M7-D2 AE.2 冻结 payload；v0.1 定长 1032 字节 LE）----

// AE.2：payload 定长（< 1032B 丢弃；> 1032B 忽略尾部）。
inline constexpr size_t kPhysicalPreviewPayloadSize = 1032;
// AE.2：pixels 起始偏移（8 = frameId2 + width2 + height2 + pixelFormat1 + flags1）。
inline constexpr size_t kPhysicalPreviewPixelOffset = 8;
// AE.2：pixels 字节数（128x64 页式 1bpp）。
inline constexpr size_t kPhysicalPreviewPixelBytes = 1024;

// PHYSICAL_PREVIEW 解析结果（AE.2 字段全集；pixels 不驻留于本结构——
// 解析方经 onPhysicalPreview 回调直接引用 payload 数据，见 protocol_endpoint.h）。
struct PhysicalPreviewInfo {
    uint16_t frameId = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    PhysicalPixelFormat pixelFormat = PhysicalPixelFormat::kMono1;
    uint8_t flags = 0;
};

// 构造 PHYSICAL_PREVIEW（AE.2 布局；无 ACK_REQ，fire-and-forget）。违规输入返回
// nullopt：width/height 须 1..4096；pixelFormat 仅 kMono1（v0.1 唯一合法值）；
// flags 仅 bit0（保留位必须为 0）；pixels 不得为 nullptr。
std::optional<Message> makePhysicalPreview(uint16_t frameId, uint16_t width,
                                           uint16_t height,
                                           PhysicalPixelFormat pixelFormat,
                                           uint8_t flags, const uint8_t* pixels);

// 解析 PHYSICAL_PREVIEW（AE.3）：< 1032B → false；> 1032B 忽略尾部；
// pixelFormat 白名单兜底（v0.1 唯一合法值 kMono1，未知值映射 kMono1，
// 杜绝 UI 数值注入）；width/height 以 wire 字段为准（AE.2）。
bool parsePhysicalPreview(BytesView payload, PhysicalPreviewInfo& out);

// ---- Wi-Fi provisioning（TYPE 0x06..0x09）（M7-D3 AF.2 冻结 payload）----

// AF.2 常量：SSID 1..32 字节；password 字段 64 字节定宽（0=开放网络，8..63 passphrase）；
// WIFI_CONFIG 定长 103B；WIFI_SCAN_REQ 定长 2B；SCAN_RESULT record 42B、最多 64 条
// （5 + 64*42 = 2693B ≤ 4096 单包上限）；WIFI_STATUS 17 + ssidLen(≤32) = 49B 上限。
inline constexpr size_t kWifiSsidMaxBytes = 32;
inline constexpr size_t kWifiPasswordFieldBytes = 64;
inline constexpr size_t kWifiConfigPayloadSize = 103;
inline constexpr size_t kWifiScanReqPayloadSize = 2;
inline constexpr uint8_t kWifiScanReqMaxEntries = 64;
inline constexpr size_t kWifiScanResultRecordBytes = 42;
inline constexpr size_t kWifiScanResultMaxRecords = 64;
inline constexpr size_t kWifiStatusMaxPayload = 49;
// AF.2：WIFI_CONFIG.flags bit0 = CLEAR（清除凭据并断开）。
inline constexpr uint8_t kWifiConfigFlagClear = 0x01;
// AF.2：WIFI_SCAN_RESULT.flags bit0 = truncated。
inline constexpr uint8_t kWifiScanResultFlagTruncated = 0x01;

// SCAN_RESULT 单条 record（42B）解析结果（ssid 为可见字节 1..32，无 NUL）。
struct WifiScanRecordInfo {
    std::string ssid;                  // 1..32 可见字节
    std::array<uint8_t, 6> bssid{};    // 原始 6 字节 BSSID
    int8_t rssi = -128;                // dBm
    uint8_t channel = 0;               // 1..14 / 0=未知
    uint8_t authmode = 0;              // ESP-IDF wifi_auth_mode_t 0..8
};

// SCAN_RESULT 完整解析结果（≤64 条，RSSI 降序）。
struct WifiScanResultInfo {
    uint8_t scanSeq = 0;               // 响应 REQ 计数（回绕）
    uint8_t count = 0;                 // records.size()（0..64）
    uint8_t flags = 0;                 // bit0=truncated
    uint16_t total = 0;                // 总可见 AP 数（含截断）
    std::vector<WifiScanRecordInfo> records;  // count 条
};

// WIFI_CONFIG 解析结果（AF.2 103B）。password 为宿主内存副本（仅调用方持有，
// 用后须清零；本结构绝不进入日志/持久化/UI）。
struct WifiConfigInfo {
    uint8_t flags = 0;                 // bit0=CLEAR
    std::string ssid;                  // 1..32 可见字节
    std::string password;              // 0（开放网络）或 8..63 字节
    uint32_t serverIp = 0;             // 网络序 IPv4（禁 0.0.0.0）
    uint16_t serverPort = 0;           // 1..65535
};

// WIFI_STATUS 解析结果（AF.2；绝无密码字段）。
struct WifiStatusInfo {
    uint8_t phase = 0;                 // WifiStatusPhase
    uint16_t errorCode = 0;            // ErrorCode（0=无）
    uint8_t flags = 0;                 // 保留
    int8_t rssi = -128;                // dBm（-128=无）
    uint8_t channel = 0;               // 0=无
    uint32_t ip = 0;                   // 本机 IPv4（网络序）
    uint32_t serverIp = 0;             // 目标 server IPv4（网络序）
    uint16_t serverPort = 0;           // LE
    uint8_t ssidLen = 0;               // 0..32
    std::string ssid;                  // 当前网络名（非 secret metadata）
};

// 构造 WIFI_SCAN_REQ（0x06，必须 ACK_REQ，2B）。flags 保留位必须为 0；
// maxEntries 0=默认32 或 1..64。违规返回 nullopt。
std::optional<Message> makeWifiScanReq(uint8_t flags, uint8_t maxEntries);

// 解析 WIFI_SCAN_REQ（定长 2B）。
bool parseWifiScanReq(BytesView payload, uint8_t& flags, uint8_t& maxEntries);

// 构造 WIFI_SCAN_RESULT（0x07，fire-and-forget，无 ACK_REQ，≤2693B）。
// count = records 条数（0..64）；records 为 null 时 count 必须为 0；
// 每条 ssid 须 1..32 可见字节、authmode 0..8。违规返回 nullopt。
std::optional<Message> makeWifiScanResult(uint8_t scanSeq, bool truncated, uint16_t total,
                                          const WifiScanRecordInfo* records, size_t count);

// 解析 WIFI_SCAN_RESULT：payload ≥ 5 + count*42（尾部忽略）；count > 64 → false。
bool parseWifiScanResult(BytesView payload, WifiScanResultInfo& out);

// 构造 WIFI_CONFIG（0x08，必须 ACK_REQ，103B）。ssid 1..32 字节；
// password 0（开放网络）或 8..63 字节；serverIp 网络序且不得为 0；serverPort 1..65535。
// 违规返回 nullopt（含 1..7 字节短密码与 64 字节 raw-PSK——v0.1 不实现）。
std::optional<Message> makeWifiConfig(std::string_view ssid, std::string_view password,
                                      uint32_t serverIp, uint16_t serverPort);

// 构造 WIFI_CONFIG CLEAR（0x08，flags bit0=1，103B，其余字段全 0；必须 ACK_REQ）。
Message makeWifiClear();

// 解析 WIFI_CONFIG（定长 103B，尾部忽略）。password 拷贝进 out 供调用方
// 用后清零（本函数不落任何日志/持久化）。
bool parseWifiConfig(BytesView payload, WifiConfigInfo& out);

// 构造 WIFI_STATUS（0x09，fire-and-forget，无 ACK_REQ，17+ssidLen ≤49B）。
// phase 白名单 0..9；ssid 0..32 可见字节。违规返回 nullopt。
std::optional<Message> makeWifiStatus(uint8_t phase, uint16_t errorCode, uint8_t flags,
                                      int8_t rssi, uint8_t channel, uint32_t ip,
                                      uint32_t serverIp, uint16_t serverPort,
                                      std::string_view ssid);

// 解析 WIFI_STATUS：payload ≥ 17 + ssidLen（尾部忽略）；ssidLen > 32 → false。
bool parseWifiStatus(BytesView payload, WifiStatusInfo& out);

// 构造 CAPABILITIES（AD.2 布局；无 ACK_REQ，fire-and-forget）。违规输入返回 nullopt：
//   width/height 须 1..4096；physWidth/physHeight 0=未知 或 1..4096；
//   pixelFormat 仅 kRgb565；physPixelFormat 仅 kRgb565/kMono1；
//   physController 仅白名单 4 值；modeMask 高 4 位、sceneSupport 高 6 位必须为 0。
std::optional<Message> makeCapabilities(
    bool virtualPresent, bool physicalPresent, uint16_t width, uint16_t height,
    PixelFormat pixelFormat, uint8_t colorDepth, bool virtualMono,
    bool virtualCanReadback, uint8_t modeMask, uint16_t physWidth, uint16_t physHeight,
    PhysicalPixelFormat physPixelFormat, uint8_t physColorDepth, bool physMono,
    bool physCanReadback, CapabilitiesController physController,
    uint8_t physI2cAddress, uint8_t sceneSupport);

// 解析 CAPABILITIES（AD.3 规则）：< 32B → false；> 32B 忽略尾部；version≠0x01 → false；
// 未知枚举白名单映射（physController → kUnknown；physPixelFormat/pixelFormat → 白名单兜底），
// 杜绝 UI 数值注入。成功 → out 填充并返回 true。
bool parseCapabilities(BytesView payload, CapabilitiesInfo& out);

// ---- ACK_REQ 白名单（M8-A1；DESIGN.md E 节 ACK 语义）----
// 仅 SET_MODE(0x03) / WIFI_SCAN_REQ(0x06) / WIFI_CONFIG(0x08) 可携带 ACK_REQ；
// 其余类型（FRAME_*/INPUT_*/PING/PONG/ACK/ERROR/HELLO/CAPABILITIES/
// PHYSICAL_PREVIEW/WIFI_RESULT/WIFI_STATUS）一律禁止。
//   Encoder（encode/encodeStream/encodeStreaming）：白名单外 + ACK_REQ
//     → PacketError::kInvalidAckReq（实现层错误，非 wire 格式变化）；
//   Endpoint RX：白名单外 + ACK_REQ → 忽略消息 + 计数（不回 ACK、不发错误）；
//   StreamDecoder 保持 wire-transparent，不解释 ACK_REQ。
bool allowedAckRequestType(uint8_t type);

// ACK (0x51)：ackSeq = 被确认包的 SEQ；status = 0(OK)/1(ERR)；errorCode。
Message makeAck(uint16_t ackSeq, uint8_t status, ErrorCode errorCode);

// ERROR (0x50)：errorCode + 文本（0..64 字节）。违规返回 nullopt。
std::optional<Message> makeError(ErrorCode errorCode, std::string_view text);

// PING / PONG (0x30/0x31)：timestampMs（LE u64，发送方单调毫秒时间）。
Message makePing(uint64_t timestampMs);
Message makePong(uint64_t timestampMs);

// INPUT_KEY (0x20)：keycode（USB HID usage）, modifiers, down。
Message makeInputKey(uint32_t keycode, uint16_t modifiers, bool down);

// INPUT_MOUSE (0x21)：buttons, x, y, wheel（有符号增量）, flags。
Message makeInputMouse(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel, uint8_t flags);

// ---- 帧消息（DESIGN.md「帧消息 Payload Layout」）----

// FRAME_BEGIN (0x10)：width/height 须为 1..4096，否则 nullopt。
std::optional<Message> makeFrameBegin(uint16_t frameId, FrameType frameType,
                                      PixelFormat pixelFormat,
                                      uint16_t width, uint16_t height, uint32_t byteHint);

// FRAME_RECT (0x11)：像素格式由 BEGIN 声明（v0.1 为 RGB565，bpp=2）。
// pixelBytes 必须 == w*h*2，否则返回 nullopt。
std::optional<Message> makeFrameRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                     const uint8_t* pixels, size_t pixelBytes);

// FRAME_END (0x12)：frameId 必须与 BEGIN 一致（调用方保证），aborted 置 FRAME_END.flags.ABORTED。
Message makeFrameEnd(uint16_t frameId, uint16_t rectCount, uint32_t byteCount, bool aborted);

}  // namespace proto
}  // namespace espview
