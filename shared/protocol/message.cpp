#include "message.h"

#include <cstring>
#include <utility>

#include "byte_order.h"

namespace espview {
namespace proto {

namespace {

// LE 写入统一走 byte_order.h（M8-A1 内部重构；wire 字节序冻结，逐位不变）。
void putU16(std::vector<uint8_t>& v, uint16_t val) {
    const size_t off = v.size();
    v.resize(off + 2);
    writeU16LE(v.data() + off, val);
}

void putU32(std::vector<uint8_t>& v, uint32_t val) {
    const size_t off = v.size();
    v.resize(off + 4);
    writeU32LE(v.data() + off, val);
}

void putU64(std::vector<uint8_t>& v, uint64_t val) {
    const size_t off = v.size();
    v.resize(off + 8);
    writeU64LE(v.data() + off, val);
}

// 网络序 u32（AF.2：serverIp/ip 字段按大端写线；入参为网络序 u32）。
void pushNetU32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>(val & 0xFFu));
}

uint16_t readU16(BytesView p, size_t off) {
    // 小端读取（调用方保证 off+2 <= p.size()）。
    return readU16LE(p.data() + off);
}

// 网络序 u32 读取（首字节 = 地址第一段）。
uint32_t readNetU32(BytesView p, size_t off) {
    return (static_cast<uint32_t>(p[off]) << 24) |
           (static_cast<uint32_t>(p[off + 1]) << 16) |
           (static_cast<uint32_t>(p[off + 2]) << 8) |
           static_cast<uint32_t>(p[off + 3]);
}

// 定长 NUL 填充字段提取（SSID/password；遇首个 NUL 即止，不含填充字节）。
std::string readFixedString(BytesView p, size_t off, size_t len) {
    size_t n = 0;
    while (n < len && p[off + n] != 0) {
        ++n;
    }
    return std::string(reinterpret_cast<const char*>(p.data() + off), n);
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

std::optional<Message> makeWifiScanReq(uint8_t flags, uint8_t maxEntries) {
    // AF.2：flags 保留位必须为 0；maxEntries 0=默认32 或 1..64。
    if (flags != 0) {
        return std::nullopt;
    }
    if (maxEntries > kWifiScanReqMaxEntries) {
        return std::nullopt;
    }
    std::vector<uint8_t> p = {flags, maxEntries};
    return messageWithPayload(static_cast<uint8_t>(MessageType::kWifiScanReq), kFlagAckReq,
                              std::move(p));
}

bool parseWifiScanReq(BytesView payload, uint8_t& flags, uint8_t& maxEntries) {
    if (payload.size() != kWifiScanReqPayloadSize) {
        return false;
    }
    flags = payload[0];
    maxEntries = payload[1];
    return true;
}

std::optional<Message> makeWifiScanResult(uint8_t scanSeq, bool truncated, uint16_t total,
                                          const WifiScanRecordInfo* records, size_t count) {
    // AF.2：0..64 条；每条 ssid 1..32 可见字节、authmode 0..8（ESP-IDF 白名单）。
    if (count > kWifiScanResultMaxRecords) {
        return std::nullopt;
    }
    if (count > 0 && records == nullptr) {
        return std::nullopt;
    }
    for (size_t i = 0; i < count; ++i) {
        if (records[i].ssid.empty() || records[i].ssid.size() > kWifiSsidMaxBytes) {
            return std::nullopt;
        }
        if (records[i].authmode > 8) {
            return std::nullopt;
        }
    }

    // AF.2 布局：scanSeq(1) count(1) flags(1) total(2 LE) records[count*42]。
    std::vector<uint8_t> p;
    p.reserve(5 + count * kWifiScanResultRecordBytes);
    p.push_back(scanSeq);
    p.push_back(static_cast<uint8_t>(count));
    p.push_back(truncated ? kWifiScanResultFlagTruncated : 0u);
    putU16(p, total);
    for (size_t i = 0; i < count; ++i) {
        const WifiScanRecordInfo& r = records[i];
        for (size_t j = 0; j < kWifiSsidMaxBytes; ++j) {
            p.push_back(j < r.ssid.size() ? static_cast<uint8_t>(r.ssid[j]) : 0);
        }
        p.insert(p.end(), r.bssid.begin(), r.bssid.end());
        p.push_back(static_cast<uint8_t>(r.rssi));
        p.push_back(r.channel);
        p.push_back(r.authmode);
        p.push_back(0);  // rsvd
    }
    return messageWithPayload(static_cast<uint8_t>(MessageType::kWifiScanResult), 0,
                              std::move(p));
}

bool parseWifiScanResult(BytesView payload, WifiScanResultInfo& out) {
    // AF.3：短包/超限拒绝；长于 5+count*42 忽略尾部。
    if (payload.size() < 5) {
        return false;
    }
    const uint8_t count = payload[1];
    if (count > kWifiScanResultMaxRecords) {
        return false;
    }
    const size_t need = 5 + static_cast<size_t>(count) * kWifiScanResultRecordBytes;
    if (payload.size() < need) {
        return false;
    }

    WifiScanResultInfo info;
    info.scanSeq = payload[0];
    info.count = count;
    info.flags = payload[2];
    info.total = readU16(payload, 3);
    info.records.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const size_t o = 5 + i * kWifiScanResultRecordBytes;
        WifiScanRecordInfo rec;
        rec.ssid = readFixedString(payload, o, kWifiSsidMaxBytes);
        std::memcpy(rec.bssid.data(), payload.data() + o + kWifiSsidMaxBytes, 6);
        rec.rssi = static_cast<int8_t>(payload[o + 38]);
        rec.channel = payload[o + 39];
        rec.authmode = payload[o + 40];
        info.records.push_back(std::move(rec));
    }
    out = std::move(info);
    return true;
}

std::optional<Message> makeWifiConfig(std::string_view ssid, std::string_view password,
                                      uint32_t serverIp, uint16_t serverPort) {
    // AF.2 校验：ssid 1..32；password 0（开放网络）或 8..63（64-hex raw-PSK 预留，
    // v0.1 拒绝）；serverIp 网络序且非 0；serverPort 1..65535。
    if (ssid.empty() || ssid.size() > kWifiSsidMaxBytes) {
        return std::nullopt;
    }
    if (!password.empty() && (password.size() < 8 || password.size() > 63)) {
        return std::nullopt;
    }
    if (serverIp == 0) {
        return std::nullopt;
    }
    if (serverPort == 0) {
        return std::nullopt;
    }

    // AF.2 布局：定长 103 字节；flags(0) ssid[32] password[64] serverIp(4 网络序)
    // serverPort(2 LE)。按下标写入（禁止追加，避免破坏定长）。
    std::vector<uint8_t> p(kWifiConfigPayloadSize, 0);
    p[0] = 0;  // 非 CLEAR
    for (size_t i = 0; i < kWifiSsidMaxBytes && i < ssid.size(); ++i) {
        p[1 + i] = static_cast<uint8_t>(ssid[i]);
    }
    for (size_t i = 0; i < kWifiPasswordFieldBytes && i < password.size(); ++i) {
        p[33 + i] = static_cast<uint8_t>(password[i]);
    }
    p[97] = static_cast<uint8_t>((serverIp >> 24) & 0xFFu);
    p[98] = static_cast<uint8_t>((serverIp >> 16) & 0xFFu);
    p[99] = static_cast<uint8_t>((serverIp >> 8) & 0xFFu);
    p[100] = static_cast<uint8_t>(serverIp & 0xFFu);
    p[101] = static_cast<uint8_t>(serverPort & 0xFFu);
    p[102] = static_cast<uint8_t>((serverPort >> 8) & 0xFFu);
    return messageWithPayload(static_cast<uint8_t>(MessageType::kWifiConfig), kFlagAckReq,
                              std::move(p));
}

Message makeWifiClear() {
    // AF.2：CLEAR（flags bit0=1），其余字段全 0；必须 ACK_REQ。
    std::vector<uint8_t> p(kWifiConfigPayloadSize, 0);
    p[0] = kWifiConfigFlagClear;
    return messageWithPayload(static_cast<uint8_t>(MessageType::kWifiConfig), kFlagAckReq,
                              std::move(p));
}

bool parseWifiConfig(BytesView payload, WifiConfigInfo& out) {
    // AF.3：短包拒绝；长于 103B 忽略尾部。password 为宿主内存副本，用后须清零。
    if (payload.size() < kWifiConfigPayloadSize) {
        return false;
    }
    WifiConfigInfo info;
    info.flags = payload[0];
    info.ssid = readFixedString(payload, 1, kWifiSsidMaxBytes);
    info.password = readFixedString(payload, 33, kWifiPasswordFieldBytes);
    info.serverIp = readNetU32(payload, 97);
    info.serverPort = readU16(payload, 101);
    out = std::move(info);
    return true;
}

std::optional<Message> makeWifiStatus(uint8_t phase, uint16_t errorCode, uint8_t flags,
                                      int8_t rssi, uint8_t channel, uint32_t ip,
                                      uint32_t serverIp, uint16_t serverPort,
                                      std::string_view ssid) {
    // AF.2：phase 白名单 0..9；ssid 0..32 字节；载荷 17+ssidLen ≤ 49B。
    if (phase > static_cast<uint8_t>(WifiStatusPhase::kCleared)) {
        return std::nullopt;
    }
    if (ssid.size() > kWifiSsidMaxBytes) {
        return std::nullopt;
    }

    std::vector<uint8_t> p;
    p.reserve(17 + ssid.size());
    p.push_back(phase);
    putU16(p, errorCode);
    p.push_back(flags);
    p.push_back(static_cast<uint8_t>(rssi));
    p.push_back(channel);
    pushNetU32(p, ip);
    pushNetU32(p, serverIp);
    putU16(p, serverPort);
    p.push_back(static_cast<uint8_t>(ssid.size()));
    p.insert(p.end(), ssid.begin(), ssid.end());
    return messageWithPayload(static_cast<uint8_t>(MessageType::kWifiStatus), 0,
                              std::move(p));
}

bool parseWifiStatus(BytesView payload, WifiStatusInfo& out) {
    // AF.3：短包拒绝；长于 17+ssidLen 忽略尾部；ssidLen 上限 32。
    if (payload.size() < 17) {
        return false;
    }
    const uint8_t ssidLen = payload[16];
    if (ssidLen > kWifiSsidMaxBytes) {
        return false;
    }
    if (payload.size() < 17u + ssidLen) {
        return false;
    }

    WifiStatusInfo info;
    info.phase = payload[0];
    info.errorCode = readU16(payload, 1);
    info.flags = payload[3];
    info.rssi = static_cast<int8_t>(payload[4]);
    info.channel = payload[5];
    info.ip = readNetU32(payload, 6);
    info.serverIp = readNetU32(payload, 10);
    info.serverPort = readU16(payload, 14);
    info.ssidLen = ssidLen;
    info.ssid.assign(reinterpret_cast<const char*>(payload.data() + 17), ssidLen);
    out = std::move(info);
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

// M8-A1 ACK_REQ 白名单：仅这些控制消息可携带 ACK_REQ（DESIGN.md E 节 ACK 语义
// v0.1 SET_MODE + M7-D3 WIFI_SCAN_REQ/WIFI_CONFIG「必须 ACK_REQ」）。其余类型
// （FRAME_*/INPUT_*/PING/PONG/ACK/ERROR/HELLO/CAPABILITIES/PHYSICAL_PREVIEW/
// WIFI_RESULT/WIFI_STATUS）一律禁止。
bool allowedAckRequestType(uint8_t type) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::kSetMode:      // 0x03 SET_MODE
        case MessageType::kWifiScanReq:  // 0x06 WIFI_SCAN_REQ
        case MessageType::kWifiConfig:   // 0x08 WIFI_CONFIG
            return true;
        default:
            return false;
    }
}

}  // namespace proto
}  // namespace espview
