// ESPView — 共享协议常量与枚举（M0-A）
//
// 规范来源：docs/DESIGN.md E 节（Protocol v0.1）。
// 纯 C++17，零平台依赖，可同时被 ESP32 与 PC 使用。
// 注意：此处只定义 DESIGN.md 已明确的协议字段，不引入任何新字段。

#pragma once

#include <array>
#include <cstdint>

namespace espview {
namespace proto {

// ---- 常量（DESIGN.md E 节「最终 Packet Header」）----

// 帧同步标识：'E' 'S' 'P' 'V'
inline constexpr std::array<uint8_t, 4> kProtocolMagic = {0x45, 0x53, 0x50, 0x56};

// 协议版本（Header.VERSION）
inline constexpr uint8_t kProtocolVersion = 0x01;

// 单包载荷上限（Header.LENGTH 上限，0..kMaxPacketPayload）
inline constexpr uint32_t kMaxPacketPayload = 4096;

// CAPABILITIES payload 版本（AD.2：payload 独立于 kProtocolVersion；0x00 或 >0x01 丢弃）
inline constexpr uint8_t kCapabilitiesPayloadVersion = 0x01;

// 逻辑 Message 完整 payload 上限（MAX_MESSAGE_PAYLOAD，DESIGN.md E 节，M0-C 正式冻结）。
// 一个 Message 的所有 CHUNKED Packet 拼接后不得超过该值；仅 wire-level 上限，
// 不要求任何组件分配固定 1 MiB 缓冲（流式/分段处理）。
inline constexpr uint32_t kMaxMessagePayload = 1048576;  // 1 MiB

// ---- FLAGS 位（Header.FLAGS）----
inline constexpr uint8_t kFlagChunked = 0x01;  // bit0：本包是消息的续片
inline constexpr uint8_t kFlagAckReq = 0x02;   // bit1：请求对端回 ACK

// Header.TYPE 合法范围（DESIGN.md 消息表：0x01..0x51）
inline constexpr uint8_t kMinMessageType = 0x01;
inline constexpr uint8_t kMaxMessageType = 0x51;

// ---- 消息类型（Header.TYPE，DESIGN.md 消息表）----
enum class MessageType : uint8_t {
    kHello          = 0x01,  // ESP→PC 握手
    kCapabilities   = 0x02,  // 双向（可选）
    kSetMode        = 0x03,  // PC→ESP，必须 ACK_REQ
    kSetResolution  = 0x04,  // 未来
    kSetPixelFormat = 0x05,  // 未来
    kFrameBegin     = 0x10,  // ESP→PC
    kFrameRect      = 0x11,  // ESP→PC
    kFrameEnd       = 0x12,  // ESP→PC
    kPhysicalPreview = 0x13,  // ESP→PC（M7-D2 PHYSICAL_PREVIEW）
    kInputKey       = 0x20,  // PC→ESP
    kInputMouse     = 0x21,  // PC→ESP
    kInputTouch     = 0x22,  // 未来
    kPing           = 0x30,  // 双向
    kPong           = 0x31,  // 双向
    kReset          = 0x40,  // 保留类型号，v0.1 不实现
    kError          = 0x50,  // 双向
    kAck            = 0x51,  // 双向（仅响应 ACK_REQ）
};

// ---- 像素格式（FRAME_BEGIN.pixelFormat / HELLO.pixelFormat）----
enum class PixelFormat : uint8_t {
    kRgb565 = 0,  // 16bpp，低字节在前
};

// ---- 帧类型（FRAME_BEGIN.frameType）----
enum class FrameType : uint8_t {
    kFull    = 0,  // 整帧（握手/重连重同步用）
    kPartial = 1,  // 局部更新
};

// ---- FRAME_END.flags ----
inline constexpr uint8_t kFrameEndFlagAborted = 0x01;  // bit0：发送端主动作废本帧

// ---- 显示模式（SET_MODE.mode）----
enum class DisplayMode : uint8_t {
    kWindow = 0,
    kDevice = 1,
    kMirror = 2,
    kSplit = 3,  // M7-C：additive wire 扩展；0/1/2 值不变。
};

// ---- 错误码（ACK.errorCode / ERROR.errorCode，DESIGN.md 控制消息 Layout）----
enum class ErrorCode : uint16_t {
    kNone            = 0,
    kUnsupportedMode = 1,
    kInvalidParam    = 2,
    kBusy            = 3,
    kInternal        = 4,
};

}  // namespace proto
}  // namespace espview
