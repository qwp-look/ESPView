// Packet 层单元测试（M0-A）。
// 规范来源：docs/DESIGN.md E 节「最终 Packet Header / CRC32 规范 / 帧级错误处理」。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "packet.h"
#include "protocol.h"
#include "test_util.h"

namespace {

using espview::proto::decodeHeader;
using espview::proto::encodeHeader;
using espview::proto::encodeHeaderToArray;
using espview::proto::encodePacket;
using espview::proto::kFlagAckReq;
using espview::proto::kFlagChunked;
using espview::proto::kMaxMessageType;
using espview::proto::kMaxPacketPayload;
using espview::proto::kMinMessageType;
using espview::proto::kPacketHeaderSize;
using espview::proto::MessageType;

// scoped enum 别名（makeHeader 接受 uint8_t type）。
constexpr uint8_t kMsgPing = static_cast<uint8_t>(MessageType::kPing);
constexpr uint8_t kMsgFrameRect = static_cast<uint8_t>(MessageType::kFrameRect);
using espview::proto::kProtocolMagic;
using espview::proto::kProtocolVersion;
using espview::proto::makeHeader;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::verifyPacketCrc;

std::vector<uint8_t> makePayload(size_t n, uint8_t seed) {
    std::vector<uint8_t> p(n);
    for (size_t i = 0; i < n; ++i) {
        p[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
    }
    return p;
}

// ---- 用户要求的最小测试集 ----

// TEST: empty_payload — LENGTH=0 合法（DESIGN.md：0..4096），CRC 只覆盖 14 字节头。
void empty_payload() {
    const PacketHeader h = makeHeader(kMsgPing, 0, 7, 0);
    std::array<uint8_t, kPacketHeaderSize> buf{};
    size_t written = 0;
    CHECK_EQ(encodePacket(h, nullptr, 0, buf.data(), buf.size(), &written), PacketError::kNone);
    CHECK_EQ(written, kPacketHeaderSize);
    PacketHeader d{};
    CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
    CHECK_EQ(d.length, 0u);
    CHECK_EQ(d.type, kMsgPing);
    CHECK_EQ(d.seq, 7u);
    CHECK_EQ(verifyPacketCrc(d, nullptr, 0), PacketError::kNone);
}

// TEST: one_byte_payload
void one_byte_payload() {
    const std::vector<uint8_t> payload = makePayload(1, 0xAB);
    const PacketHeader h = makeHeader(kMsgPing, 0, 0, 1);
    std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
    size_t written = 0;
    CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), &written),
             PacketError::kNone);
    CHECK_EQ(written, buf.size());
    PacketHeader d{};
    CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
    CHECK_EQ(d.length, 1u);
    CHECK_EQ(buf[kPacketHeaderSize], 0xAB);
    CHECK_EQ(verifyPacketCrc(d, buf.data() + kPacketHeaderSize, d.length), PacketError::kNone);
}

// TEST: max_payload — LENGTH=4096（上限）。
void max_payload() {
    const std::vector<uint8_t> payload = makePayload(kMaxPacketPayload, 0x11);
    const PacketHeader h = makeHeader(kMsgFrameRect, 0, 1, kMaxPacketPayload);
    std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
    CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
             PacketError::kNone);
    PacketHeader d{};
    CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
    CHECK_EQ(d.length, kMaxPacketPayload);
    CHECK_EQ(verifyPacketCrc(d, buf.data() + kPacketHeaderSize, d.length), PacketError::kNone);
    CHECK(std::memcmp(buf.data() + kPacketHeaderSize, payload.data(), payload.size()) == 0);
}

// 边界长度 4095 / 4096 均可编码、解码、过 CRC。
void payload_boundaries_4095_4096() {
    for (uint32_t len : {4095u, 4096u}) {
        const std::vector<uint8_t> payload = makePayload(len, 0x22);
        const PacketHeader h = makeHeader(kMsgFrameRect, 0, 2, len);
        std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
        CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
                 PacketError::kNone);
        PacketHeader d{};
        CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
        CHECK_EQ(d.length, len);
        CHECK_EQ(verifyPacketCrc(d, buf.data() + kPacketHeaderSize, d.length), PacketError::kNone);
    }
}

// TEST: oversized_payload_rejected — LENGTH=4097 必须在编码侧与解码侧都被拒绝。
void oversized_payload_rejected() {
    // 解码侧：手工构造 LENGTH=4097 的原始头，decodeHeader 必须拒绝。
    {
        const PacketHeader h = makeHeader(kMsgFrameRect, 0, 3, 4097);
        const std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(h);
        PacketHeader d{};
        CHECK_EQ(decodeHeader(raw.data(), raw.size(), &d), PacketError::kInvalidLength);
    }
    // 编码侧：encodeHeader / encodePacket 必须拒绝。
    {
        const PacketHeader h = makeHeader(kMsgFrameRect, 0, 3, 4097);
        std::array<uint8_t, kPacketHeaderSize> raw{};
        CHECK_EQ(encodeHeader(h, raw.data(), raw.size(), nullptr), PacketError::kInvalidLength);

        std::vector<uint8_t> payload(4097, 0x33);
        std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
        CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
                 PacketError::kInvalidLength);
    }
    // h.length 与 payloadLen 不一致也必须拒绝。
    {
        const PacketHeader h = makeHeader(kMsgFrameRect, 0, 3, 10);
        std::vector<uint8_t> payload(11, 0x33);
        std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
        CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
                 PacketError::kInvalidLength);
    }
}

// TEST: header_roundtrip — decode(encode(h)) == h（全部字段）。
void header_roundtrip() {
    const PacketHeader h = makeHeader(kMsgFrameRect, kFlagChunked | kFlagAckReq, 0x1234, 100);
    const std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(h);
    PacketHeader d{};
    CHECK_EQ(decodeHeader(raw.data(), raw.size(), &d), PacketError::kNone);
    CHECK(d.magic == kProtocolMagic);
    CHECK_EQ(d.version, h.version);
    CHECK_EQ(d.type, h.type);
    CHECK_EQ(d.flags, h.flags);
    CHECK_EQ(d.rsvd, h.rsvd);
    CHECK_EQ(d.seq, h.seq);
    CHECK_EQ(d.length, h.length);
    CHECK_EQ(d.crc32, h.crc32);
    CHECK_EQ(d.rsvd2, h.rsvd2);
}

// TEST: packet_roundtrip — decode(encode(packet)) == packet，载荷逐字节一致。
void packet_roundtrip() {
    const std::vector<uint8_t> payload = makePayload(64, 0x44);
    const PacketHeader h = makeHeader(kMsgFrameRect, kFlagChunked,
                                      static_cast<uint16_t>(0xABCD),
                                      static_cast<uint32_t>(payload.size()));
    std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
    CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
             PacketError::kNone);
    PacketHeader d{};
    CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
    CHECK_EQ(d.seq, h.seq);
    CHECK_EQ(d.type, h.type);
    CHECK_EQ(d.flags, h.flags);
    CHECK_EQ(d.length, payload.size());
    CHECK_EQ(verifyPacketCrc(d, buf.data() + kPacketHeaderSize, d.length), PacketError::kNone);
    CHECK(std::memcmp(buf.data() + kPacketHeaderSize, payload.data(), payload.size()) == 0);
}

// TEST: bad_magic — MAGIC 错误必须返回 kInvalidMagic。
void bad_magic() {
    std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(makeHeader(kMsgPing, 0, 1, 0));
    raw[0] ^= 0x01;
    PacketHeader d{};
    CHECK_EQ(decodeHeader(raw.data(), raw.size(), &d), PacketError::kInvalidMagic);
}

// TEST: bad_version — VERSION != 0x01 必须返回 kUnsupportedVersion。
void bad_version() {
    std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(makeHeader(kMsgPing, 0, 1, 0));
    raw[4] = 0x02;
    PacketHeader d{};
    CHECK_EQ(decodeHeader(raw.data(), raw.size(), &d), PacketError::kUnsupportedVersion);
}

// TEST: bad_crc — 翻转一个载荷字节，CRC 校验必须失败。
void bad_crc() {
    const std::vector<uint8_t> payload = makePayload(32, 0x55);
    const PacketHeader h = makeHeader(kMsgPing, 0, 1, 32);
    std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
    CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
             PacketError::kNone);
    buf[kPacketHeaderSize + 5] ^= 0xFF;
    PacketHeader d{};
    CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
    CHECK_EQ(verifyPacketCrc(d, buf.data() + kPacketHeaderSize, d.length), PacketError::kCrcMismatch);
}

// TEST: little_endian_fields — 所有多字节整数按小端编码：SEQ=0x1234→34 12；
// LENGTH=300(0x0000012C)→2C 01 00 00；CRC32 字段按 LE 落盘且可还原。
void little_endian_fields() {
    const std::vector<uint8_t> payload = makePayload(300, 0x66);
    const PacketHeader h = makeHeader(kMsgFrameRect, 0, 0x1234, 300);
    const std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(h);
    CHECK_EQ(raw[8], 0x34);
    CHECK_EQ(raw[9], 0x12);
    CHECK_EQ(raw[10], 0x2C);
    CHECK_EQ(raw[11], 0x01);
    CHECK_EQ(raw[12], 0x00);
    CHECK_EQ(raw[13], 0x00);

    const uint32_t crc = espview::proto::computePacketCrc(h, payload.data(), payload.size());
    std::vector<uint8_t> buf(kPacketHeaderSize + payload.size());
    CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
             PacketError::kNone);
    CHECK_EQ(buf[14], static_cast<uint8_t>(crc & 0xFFu));
    CHECK_EQ(buf[15], static_cast<uint8_t>((crc >> 8) & 0xFFu));
    CHECK_EQ(buf[16], static_cast<uint8_t>((crc >> 16) & 0xFFu));
    CHECK_EQ(buf[17], static_cast<uint8_t>((crc >> 24) & 0xFFu));

    PacketHeader d{};
    CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
    CHECK_EQ(d.seq, 0x1234);
    CHECK_EQ(d.length, 300u);
    CHECK_EQ(d.crc32, crc);
}

// TEST: sequence_boundaries — SEQ=0 与 SEQ=65535（回绕边界）往返一致。
void sequence_boundaries() {
    for (uint16_t seq : {static_cast<uint16_t>(0), static_cast<uint16_t>(0xFFFF)}) {
        const PacketHeader h = makeHeader(kMsgPing, 0, seq, 0);
        const std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(h);
        PacketHeader d{};
        CHECK_EQ(decodeHeader(raw.data(), raw.size(), &d), PacketError::kNone);
        CHECK_EQ(d.seq, seq);
    }
    const std::array<uint8_t, kPacketHeaderSize> rawMax =
        encodeHeaderToArray(makeHeader(kMsgPing, 0, 0xFFFF, 0));
    CHECK_EQ(rawMax[8], 0xFF);
    CHECK_EQ(rawMax[9], 0xFF);
}

// ---- 补充边界测试 ----

// RSVD / RSVD2 非零：按 DESIGN.md「接收方必须忽略」，不拒绝，仅原样读入。
void reserved_fields_ignored() {
    PacketHeader h = makeHeader(kMsgPing, 0, 5, 0);
    h.rsvd = 0xA5;
    h.rsvd2 = 0x5A;
    std::vector<uint8_t> buf(kPacketHeaderSize);
    CHECK_EQ(encodePacket(h, nullptr, 0, buf.data(), buf.size(), nullptr), PacketError::kNone);
    PacketHeader d{};
    CHECK_EQ(decodeHeader(buf.data(), buf.size(), &d), PacketError::kNone);
    CHECK_EQ(d.rsvd, 0xA5);
    CHECK_EQ(d.rsvd2, 0x5A);
    // CRC 覆盖 RSVD（字节 7），重算一致，校验仍通过。
    CHECK_EQ(verifyPacketCrc(d, nullptr, 0), PacketError::kNone);
}

// 缓冲不足：encodeHeader/decodeHeader/encodePacket 都返回 kBufferTooSmall。
void buffer_too_small() {
    const PacketHeader h = makeHeader(kMsgPing, 0, 1, 8);
    std::array<uint8_t, kPacketHeaderSize - 1> small{};
    size_t written = 999;
    CHECK_EQ(encodeHeader(h, small.data(), small.size(), &written), PacketError::kBufferTooSmall);
    CHECK_EQ(written, 0u);

    PacketHeader d{};
    CHECK_EQ(decodeHeader(small.data(), small.size(), &d), PacketError::kBufferTooSmall);

    std::vector<uint8_t> payload(8, 0x77);
    std::vector<uint8_t> buf(kPacketHeaderSize + payload.size() - 1);
    CHECK_EQ(encodePacket(h, payload.data(), payload.size(), buf.data(), buf.size(), nullptr),
             PacketError::kBufferTooSmall);
}

// TYPE 合法性（DESIGN.md 解码状态机：TYPE 0x01..0x51）：0x00 与 0x52 必须拒绝。
void invalid_type_rejected() {
    for (uint8_t t : {static_cast<uint8_t>(0x00), static_cast<uint8_t>(0x52)}) {
        const std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(makeHeader(t, 0, 1, 0));
        PacketHeader d{};
        CHECK_EQ(decodeHeader(raw.data(), raw.size(), &d), PacketError::kInvalidType);
    }
    // 边界内类型（0x01 / 0x51）合法。
    for (uint8_t t : {kMinMessageType, kMaxMessageType}) {
        const std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(makeHeader(t, 0, 1, 0));
        PacketHeader d{};
        CHECK_EQ(decodeHeader(raw.data(), raw.size(), &d), PacketError::kNone);
    }
}

// verifyPacketCrc 的载荷长度必须等于 header.length。
void verify_crc_length_mismatch() {
    const PacketHeader h = makeHeader(kMsgPing, 0, 1, 4);
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    CHECK_EQ(verifyPacketCrc(h, payload, 5), PacketError::kInvalidLength);
}

}  // namespace

void runPacketTests() {
    std::printf("  empty_payload\n");
    empty_payload();
    std::printf("  one_byte_payload\n");
    one_byte_payload();
    std::printf("  max_payload\n");
    max_payload();
    std::printf("  payload_boundaries_4095_4096\n");
    payload_boundaries_4095_4096();
    std::printf("  oversized_payload_rejected\n");
    oversized_payload_rejected();
    std::printf("  header_roundtrip\n");
    header_roundtrip();
    std::printf("  packet_roundtrip\n");
    packet_roundtrip();
    std::printf("  bad_magic\n");
    bad_magic();
    std::printf("  bad_version\n");
    bad_version();
    std::printf("  bad_crc\n");
    bad_crc();
    std::printf("  little_endian_fields\n");
    little_endian_fields();
    std::printf("  sequence_boundaries\n");
    sequence_boundaries();
    std::printf("  reserved_fields_ignored\n");
    reserved_fields_ignored();
    std::printf("  buffer_too_small\n");
    buffer_too_small();
    std::printf("  invalid_type_rejected\n");
    invalid_type_rejected();
    std::printf("  verify_crc_length_mismatch\n");
    verify_crc_length_mismatch();
}
