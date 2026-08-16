#include "packet.h"

#include <cstring>

#include "byte_order.h"
#include "crc32.h"

namespace espview {
namespace proto {

namespace {

// 把 header 的 CRC 覆盖部分（字节 [0,14)：MAGIC、VERSION、TYPE、FLAGS、RSVD、
// SEQ(LE)、LENGTH(LE)）写入 dst[0..14)。
void writeCrcCoveredHeader(const PacketHeader& h, uint8_t* dst) {
    dst[0] = h.magic[0];
    dst[1] = h.magic[1];
    dst[2] = h.magic[2];
    dst[3] = h.magic[3];
    dst[4] = h.version;
    dst[5] = h.type;
    dst[6] = h.flags;
    dst[7] = h.rsvd;
    writeU16LE(dst + 8, h.seq);
    writeU32LE(dst + 10, h.length);
}

// 完整 20 字节序列化（小端）。
void writeHeader(const PacketHeader& h, uint8_t* out) {
    writeCrcCoveredHeader(h, out);
    writeU32LE(out + 14, h.crc32);
    writeU16LE(out + 18, h.rsvd2);
}

}  // namespace

PacketHeader makeHeader(uint8_t type, uint8_t flags, uint16_t seq, uint32_t length) {
    PacketHeader h;
    h.magic = kProtocolMagic;
    h.version = kProtocolVersion;
    h.type = type;
    h.flags = flags;
    h.rsvd = 0;
    h.seq = seq;
    h.length = length;
    h.crc32 = 0;
    h.rsvd2 = 0;
    return h;
}

PacketError encodeHeader(const PacketHeader& h, uint8_t* out, size_t outSize, size_t* written) {
    if (written != nullptr) {
        *written = 0;
    }
    if (out == nullptr || outSize < kPacketHeaderSize) {
        return PacketError::kBufferTooSmall;
    }
    if (h.length > kMaxPacketPayload) {
        return PacketError::kInvalidLength;
    }
    writeHeader(h, out);
    if (written != nullptr) {
        *written = kPacketHeaderSize;
    }
    return PacketError::kNone;
}

std::array<uint8_t, kPacketHeaderSize> encodeHeaderToArray(const PacketHeader& h) {
    std::array<uint8_t, kPacketHeaderSize> out{};
    writeHeader(h, out.data());
    return out;
}

PacketError decodeHeader(const uint8_t* in, size_t inSize, PacketHeader* out) {
    if (in == nullptr || inSize < kPacketHeaderSize || out == nullptr) {
        return PacketError::kBufferTooSmall;
    }
    if (in[0] != kProtocolMagic[0] || in[1] != kProtocolMagic[1] ||
        in[2] != kProtocolMagic[2] || in[3] != kProtocolMagic[3]) {
        return PacketError::kInvalidMagic;
    }
    if (in[4] != kProtocolVersion) {
        return PacketError::kUnsupportedVersion;
    }
    const uint8_t type = in[5];
    if (type < kMinMessageType || type > kMaxMessageType) {
        return PacketError::kInvalidType;
    }
    const uint32_t length = readU32LE(in + 10);
    if (length > kMaxPacketPayload) {
        return PacketError::kInvalidLength;
    }

    out->magic = kProtocolMagic;
    out->version = in[4];
    out->type = type;
    out->flags = in[6];
    out->rsvd = in[7];
    out->seq = readU16LE(in + 8);
    out->length = length;
    out->crc32 = readU32LE(in + 14);
    out->rsvd2 = readU16LE(in + 18);
    return PacketError::kNone;
}

PacketError encodePacket(const PacketHeader& h, const uint8_t* payload, size_t payloadLen,
                         uint8_t* out, size_t outSize, size_t* written) {
    if (written != nullptr) {
        *written = 0;
    }
    if (out == nullptr || outSize < kPacketHeaderSize + payloadLen) {
        return PacketError::kBufferTooSmall;
    }
    if (payloadLen > kMaxPacketPayload) {
        return PacketError::kInvalidLength;
    }
    if (h.length != payloadLen) {
        return PacketError::kInvalidLength;
    }

    PacketHeader wire = h;
    wire.crc32 = 0;
    writeHeader(wire, out);
    if (payloadLen > 0) {
        std::memmove(out + kPacketHeaderSize, payload, payloadLen);
    }
    const uint32_t crc = computePacketCrc(wire, payload, payloadLen);
    writeU32LE(out + 14, crc);
    if (written != nullptr) {
        *written = kPacketHeaderSize + payloadLen;
    }
    return PacketError::kNone;
}

PacketError verifyPacketCrc(const PacketHeader& h, const uint8_t* payload, size_t payloadLen) {
    if (payloadLen != h.length) {
        return PacketError::kInvalidLength;
    }
    if (computePacketCrc(h, payload, payloadLen) != h.crc32) {
        return PacketError::kCrcMismatch;
    }
    return PacketError::kNone;
}

uint32_t computePacketCrc(const PacketHeader& h, const uint8_t* payload, size_t payloadLen) {
    uint8_t covered[14];
    writeCrcCoveredHeader(h, covered);
    uint32_t crc = crc32Update(kCrc32Init, covered, sizeof(covered));
    crc = crc32Update(crc, payload, payloadLen);
    return crc ^ kCrc32FinalXor;
}

const char* toString(PacketError e) {
    switch (e) {
        case PacketError::kNone:
            return "kNone";
        case PacketError::kBufferTooSmall:
            return "kBufferTooSmall";
        case PacketError::kInvalidMagic:
            return "kInvalidMagic";
        case PacketError::kUnsupportedVersion:
            return "kUnsupportedVersion";
        case PacketError::kInvalidType:
            return "kInvalidType";
        case PacketError::kInvalidLength:
            return "kInvalidLength";
        case PacketError::kCrcMismatch:
            return "kCrcMismatch";
        case PacketError::kMessageTooLarge:
            return "kMessageTooLarge";
        case PacketError::kSinkAborted:
            return "kSinkAborted";
        case PacketError::kInvalidAckReq:
            return "kInvalidAckReq";
    }
    return "kUnknown";
}

}  // namespace proto
}  // namespace espview
