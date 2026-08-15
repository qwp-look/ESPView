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

#include <cstddef>
#include <cstdint>
#include <optional>
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
