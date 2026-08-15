#include "message.h"

#include <cstring>
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

std::optional<Message> makeCapabilities(
    bool virtualPresent, bool physicalPresent, uint16_t width, uint16_t height,
    PixelFormat pixelFormat, uint8_t colorDepth, bool virtualMono,
    bool virtualCanReadback, uint8_t modeMask, uint16_t physWidth, uint16_t physHeight,
    PhysicalPixelFormat physPixelFormat, uint8_t physColorDepth, bool physMono,
    bool physCanReadback, CapabilitiesController physController,
    uint8_t physI2cAddress, uint8_t sceneSupport) {
    // AD.2 校验（违规输入 → nullopt）。
    if (width < 1 || width > 4096 || height < 1 || height > 4096) {
        return std::nullopt;  // 虚拟几何 1..4096（与 HELLO 对齐）
    }
    if (physWidth > 4096 || physHeight > 4096) {
        return std::nullopt;  // 物理几何 0=未知 或 1..4096
    }
    if (pixelFormat != PixelFormat::kRgb565) {
        return std::nullopt;  // v0.1 虚拟像素格式仅 RGB565
    }
    if (physPixelFormat != PhysicalPixelFormat::kRgb565 &&
        physPixelFormat != PhysicalPixelFormat::kMono1) {
        return std::nullopt;
    }
    if (physController != CapabilitiesController::kAuto &&
        physController != CapabilitiesController::kSsd1306 &&
        physController != CapabilitiesController::kSh1106 &&
        physController != CapabilitiesController::kUnknown) {
        return std::nullopt;
    }
    if ((modeMask & 0xF0u) != 0 || (sceneSupport & 0xFCu) != 0) {
        return std::nullopt;  // 保留位发送方必须填 0
    }

    // AD.2 布局：定长 32 字节 LE；rsvd/rsvd2 填 0（按下标写入，禁止追加）。
    std::vector<uint8_t> p(kCapabilitiesPayloadSize, 0);
    p[0] = kCapabilitiesPayloadVersion;
    p[1] = (virtualPresent ? 0x01u : 0u) | (physicalPresent ? 0x02u : 0u);
    p[2] = static_cast<uint8_t>(width & 0xFFu);
    p[3] = static_cast<uint8_t>((width >> 8) & 0xFFu);
    p[4] = static_cast<uint8_t>(height & 0xFFu);
    p[5] = static_cast<uint8_t>((height >> 8) & 0xFFu);
    p[6] = static_cast<uint8_t>(pixelFormat);
    p[7] = colorDepth;
    p[8] = virtualMono ? 1u : 0u;
    p[9] = virtualCanReadback ? 1u : 0u;
    p[10] = modeMask;
    // [11..15] rsvd = 0
    p[16] = static_cast<uint8_t>(physWidth & 0xFFu);
    p[17] = static_cast<uint8_t>((physWidth >> 8) & 0xFFu);
    p[18] = static_cast<uint8_t>(physHeight & 0xFFu);
    p[19] = static_cast<uint8_t>((physHeight >> 8) & 0xFFu);
    p[20] = static_cast<uint8_t>(physPixelFormat);
    p[21] = physColorDepth;
    p[22] = physMono ? 1u : 0u;
    p[23] = physCanReadback ? 1u : 0u;
    p[24] = static_cast<uint8_t>(physController);
    p[25] = physI2cAddress;
    p[26] = sceneSupport;
    // [27..31] rsvd2 = 0
    return messageWithPayload(static_cast<uint8_t>(MessageType::kCapabilities), 0,
                              std::move(p));
}

bool parseCapabilities(BytesView payload, CapabilitiesInfo& out) {
    // AD.3：短于 32B 丢弃；长于 32B 忽略尾部；version≠0x01 丢弃。
    if (payload.size() < kCapabilitiesPayloadSize) {
        return false;
    }
    const uint8_t version = payload[0];
    if (version != kCapabilitiesPayloadVersion) {
        return false;
    }

    CapabilitiesInfo info;
    info.version = version;
    const uint8_t flags = payload[1];
    info.virtualPresent = (flags & 0x01u) != 0;
    info.physicalPresent = (flags & 0x02u) != 0;
    info.width = static_cast<uint16_t>(payload[2]) |
                 static_cast<uint16_t>(static_cast<uint16_t>(payload[3]) << 8);
    info.height = static_cast<uint16_t>(payload[4]) |
                  static_cast<uint16_t>(static_cast<uint16_t>(payload[5]) << 8);
    // pixelFormat 白名单：v0.1 唯一合法值 kRgb565(0)，其余一律兜底（杜绝数值注入）。
    info.pixelFormat = PixelFormat::kRgb565;
    info.colorDepth = payload[7];
    info.virtualMono = payload[8] != 0;
    info.virtualCanReadback = payload[9] != 0;
    info.modeMask = payload[10];
    info.physWidth = static_cast<uint16_t>(payload[16]) |
                     static_cast<uint16_t>(static_cast<uint16_t>(payload[17]) << 8);
    info.physHeight = static_cast<uint16_t>(payload[18]) |
                      static_cast<uint16_t>(static_cast<uint16_t>(payload[19]) << 8);
    // physPixelFormat 白名单 {0,1}；其余兜底 kRgb565。
    const uint8_t ppf = payload[20];
    info.physPixelFormat =
        ppf == static_cast<uint8_t>(PhysicalPixelFormat::kMono1)
            ? PhysicalPixelFormat::kMono1
            : PhysicalPixelFormat::kRgb565;
    info.physColorDepth = payload[21];
    info.physMono = payload[22] != 0;
    info.physCanReadback = payload[23] != 0;
    // physController 白名单 {0,1,2}；未知值（含 0xFF 语义）→ kUnknown。
    switch (payload[24]) {
        case static_cast<uint8_t>(CapabilitiesController::kAuto):
            info.physController = CapabilitiesController::kAuto;
            break;
        case static_cast<uint8_t>(CapabilitiesController::kSsd1306):
            info.physController = CapabilitiesController::kSsd1306;
            break;
        case static_cast<uint8_t>(CapabilitiesController::kSh1106):
            info.physController = CapabilitiesController::kSh1106;
            break;
        default:
            info.physController = CapabilitiesController::kUnknown;
            break;
    }
    info.physI2cAddress = payload[25];
    info.sceneSupport = payload[26];

    out = info;
    return true;
}

std::optional<Message> makePhysicalPreview(uint16_t frameId, uint16_t width,
                                           uint16_t height,
                                           PhysicalPixelFormat pixelFormat,
                                           uint8_t flags, const uint8_t* pixels) {
    // AE.2 校验（违规返回 nullopt）：几何 1..4096；pixelFormat 仅 kMono1；
    // flags 仅 bit0（v1 恒 0，保留增量）；pixels 必填。
    if (width < 1 || width > 4096 || height < 1 || height > 4096) {
        return std::nullopt;
    }
    if (pixelFormat != PhysicalPixelFormat::kMono1) {
        return std::nullopt;  // v0.1 唯一合法值
    }
    if ((flags & 0xFEu) != 0) {
        return std::nullopt;  // 保留位必须为 0
    }
    if (pixels == nullptr) {
        return std::nullopt;
    }

    // AE.2 布局：frameId u16 LE + width u16 LE + height u16 LE +
    // pixelFormat u8 + flags u8 + pixels[1024]；合计 1032 字节。
    std::vector<uint8_t> p(kPhysicalPreviewPayloadSize, 0);
    p[0] = static_cast<uint8_t>(frameId & 0xFFu);
    p[1] = static_cast<uint8_t>((frameId >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>(width & 0xFFu);
    p[3] = static_cast<uint8_t>((width >> 8) & 0xFFu);
    p[4] = static_cast<uint8_t>(height & 0xFFu);
    p[5] = static_cast<uint8_t>((height >> 8) & 0xFFu);
    p[6] = static_cast<uint8_t>(pixelFormat);
    p[7] = flags;
    std::memcpy(p.data() + kPhysicalPreviewPixelOffset, pixels,
                kPhysicalPreviewPixelBytes);
    return messageWithPayload(static_cast<uint8_t>(MessageType::kPhysicalPreview), 0,
                              std::move(p));
}

bool parsePhysicalPreview(BytesView payload, PhysicalPreviewInfo& out) {
    // AE.3：< 1032B 丢弃；> 1032B 忽略尾部。
    if (payload.size() < kPhysicalPreviewPayloadSize) {
        return false;
    }

    PhysicalPreviewInfo info;
    info.frameId = static_cast<uint16_t>(payload[0]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(payload[1]) << 8);
    info.width = static_cast<uint16_t>(payload[2]) |
                 static_cast<uint16_t>(static_cast<uint16_t>(payload[3]) << 8);
    info.height = static_cast<uint16_t>(payload[4]) |
                  static_cast<uint16_t>(static_cast<uint16_t>(payload[5]) << 8);
    // pixelFormat 白名单：v0.1 唯一合法值 kMono1(1)，未知值兜底 kMono1
    //（杜绝 UI 数值注入；AE.2 解析方以 wire 字段为准渲染）。
    info.pixelFormat = PhysicalPixelFormat::kMono1;
    info.flags = payload[7];

    out = info;
    return true;
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
