#include "message.h"

#include <utility>

namespace espview {
namespace proto {

namespace {

void putU16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(static_cast<uint8_t>(val & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void putU32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>(val & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
}

void putU64(std::vector<uint8_t>& v, uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        v.push_back(static_cast<uint8_t>((val >> (8 * i)) & 0xFFu));
    }
}

Message messageWithPayload(uint8_t type, uint8_t flags, std::vector<uint8_t> payload) {
    Message m;
    m.type = type;
    m.flags = flags;
    m.payload = std::move(payload);
    return m;
}

}  // namespace

Message makeMessage(uint8_t type, uint8_t flags, std::vector<uint8_t> payload) {
    return messageWithPayload(type, flags, std::move(payload));
}

std::optional<Message> makeHello(uint8_t protocolVersion, uint8_t deviceClass,
                                 uint16_t width, uint16_t height, PixelFormat pixelFormat,
                                 uint8_t modeMask, std::string_view deviceName) {
    if (deviceName.size() > 32) {
        return std::nullopt;
    }
    if (width < 1 || width > 4096 || height < 1 || height > 4096) {
        return std::nullopt;
    }
    std::vector<uint8_t> p;
    p.reserve(9 + deviceName.size());
    p.push_back(protocolVersion);
    p.push_back(deviceClass);
    putU16(p, width);
    putU16(p, height);
    p.push_back(static_cast<uint8_t>(pixelFormat));
    p.push_back(modeMask);
    p.push_back(static_cast<uint8_t>(deviceName.size()));
    p.insert(p.end(), deviceName.begin(), deviceName.end());
    return messageWithPayload(static_cast<uint8_t>(MessageType::kHello), 0, std::move(p));
}

Message makeSetMode(DisplayMode mode) {
    std::vector<uint8_t> p;
    p.push_back(static_cast<uint8_t>(mode));
    return messageWithPayload(static_cast<uint8_t>(MessageType::kSetMode), kFlagAckReq,
                              std::move(p));
}

Message makeAck(uint16_t ackSeq, uint8_t status, ErrorCode errorCode) {
    std::vector<uint8_t> p;
    putU16(p, ackSeq);
    p.push_back(status);
    putU16(p, static_cast<uint16_t>(errorCode));
    return messageWithPayload(static_cast<uint8_t>(MessageType::kAck), 0, std::move(p));
}

std::optional<Message> makeError(ErrorCode errorCode, std::string_view text) {
    if (text.size() > 64) {
        return std::nullopt;
    }
    std::vector<uint8_t> p;
    putU16(p, static_cast<uint16_t>(errorCode));
    p.push_back(static_cast<uint8_t>(text.size()));
    p.insert(p.end(), text.begin(), text.end());
    return messageWithPayload(static_cast<uint8_t>(MessageType::kError), 0, std::move(p));
}

Message makePing(uint64_t timestampMs) {
    std::vector<uint8_t> p;
    putU64(p, timestampMs);
    return messageWithPayload(static_cast<uint8_t>(MessageType::kPing), 0, std::move(p));
}

Message makePong(uint64_t timestampMs) {
    std::vector<uint8_t> p;
    putU64(p, timestampMs);
    return messageWithPayload(static_cast<uint8_t>(MessageType::kPong), 0, std::move(p));
}

Message makeInputKey(uint32_t keycode, uint16_t modifiers, bool down) {
    std::vector<uint8_t> p;
    putU32(p, keycode);
    putU16(p, modifiers);
    p.push_back(down ? 1u : 0u);
    p.push_back(0u);  // rsvd
    return messageWithPayload(static_cast<uint8_t>(MessageType::kInputKey), 0, std::move(p));
}

Message makeInputMouse(uint8_t buttons, uint16_t x, uint16_t y, int8_t wheel, uint8_t flags) {
    std::vector<uint8_t> p;
    p.push_back(buttons);
    putU16(p, x);
    putU16(p, y);
    p.push_back(static_cast<uint8_t>(wheel));
    p.push_back(flags);
    p.push_back(0u);  // rsvd
    return messageWithPayload(static_cast<uint8_t>(MessageType::kInputMouse), 0, std::move(p));
}

std::optional<Message> makeFrameBegin(uint16_t frameId, FrameType frameType,
                                      PixelFormat pixelFormat,
                                      uint16_t width, uint16_t height, uint32_t byteHint) {
    if (width < 1 || width > 4096 || height < 1 || height > 4096) {
        return std::nullopt;
    }
    std::vector<uint8_t> p;
    p.reserve(12);
    putU16(p, frameId);
    p.push_back(static_cast<uint8_t>(frameType));
    p.push_back(static_cast<uint8_t>(pixelFormat));
    putU16(p, width);
    putU16(p, height);
    putU32(p, byteHint);
    return messageWithPayload(static_cast<uint8_t>(MessageType::kFrameBegin), 0, std::move(p));
}

std::optional<Message> makeFrameRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                     const uint8_t* pixels, size_t pixelBytes) {
    if (w < 1 || h < 1) {
        return std::nullopt;
    }
    // v0.1 像素格式为 RGB565（bpp=2），见 DESIGN.md FRAME_RECT 表。
    const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h) * 2u;
    if (pixelBytes != expected) {
        return std::nullopt;
    }
    std::vector<uint8_t> p;
    p.reserve(8 + pixelBytes);
    putU16(p, x);
    putU16(p, y);
    putU16(p, w);
    putU16(p, h);
    if (pixelBytes > 0) {
        p.insert(p.end(), pixels, pixels + pixelBytes);
    }
    return messageWithPayload(static_cast<uint8_t>(MessageType::kFrameRect), 0, std::move(p));
}

Message makeFrameEnd(uint16_t frameId, uint16_t rectCount, uint32_t byteCount, bool aborted) {
    std::vector<uint8_t> p;
    p.reserve(9);
    putU16(p, frameId);
    putU16(p, rectCount);
    putU32(p, byteCount);
    p.push_back(aborted ? kFrameEndFlagAborted : 0u);
    return messageWithPayload(static_cast<uint8_t>(MessageType::kFrameEnd), 0, std::move(p));
}

}  // namespace proto
}  // namespace espview
