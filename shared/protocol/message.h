// ESPView — Message 层：逻辑消息及其载荷构造（M0-B1）
//
// 规范来源：docs/DESIGN.md E 节「消息表 / 帧消息 Payload Layout / 控制消息 Payload Layout」。
// Message = 一条逻辑协议消息（TYPE + FLAGS + 完整载荷）。
// 本文件只负责"按 DESIGN.md 布局序列化载荷"；Message → Packet 的拆分见 encoder.h。
// 注意：CAPABILITIES / SET_RESOLUTION / SET_PIXEL_FORMAT / INPUT_TOUCH / RESET 在
// DESIGN.md 中没有 payload layout（可选/未来），因此不提供专用 builder，只能用 makeMessage。
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
