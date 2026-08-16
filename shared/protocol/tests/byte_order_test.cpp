// ESPView — byte_order.h LE 读写 Host Tests（M8-A1 Task 1）
//
// 规范来源：docs/DESIGN.md E 节（Packet Header 与全部控制消息 Payload 均 LE）。
// 覆盖：
//   - MAGIC 'E''S''P''V' = 45 53 50 56（协议黄金向量）；
//   - U16/U32/U64 LE 黄金字节向量（0x0000 / 0x00FF / 0xFFFF / 0x8000 / 0x01020304）；
//   - 写→读 round-trip 奇偶校验（含 0、高位、全 F、回绕值）；
//   - 与 packet.cpp / message.cpp 相同的字节序语义（wire 字节序冻结）。
// 纯 C++17，零平台依赖。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "byte_order.h"
#include "packet.h"
#include "protocol.h"
#include "test_util.h"

namespace {

using espview::proto::kProtocolMagic;
using espview::proto::readU16LE;
using espview::proto::readU32LE;
using espview::proto::readU64LE;
using espview::proto::writeU16LE;
using espview::proto::writeU32LE;
using espview::proto::writeU64LE;

// 协议 MAGIC 黄金向量（'E''S''P''V'）。
void magic_golden() {
    CHECK_EQ(kProtocolMagic.size(), 4u);
    CHECK_EQ(kProtocolMagic[0], 0x45);  // 'E'
    CHECK_EQ(kProtocolMagic[1], 0x53);  // 'S'
    CHECK_EQ(kProtocolMagic[2], 0x50);  // 'P'
    CHECK_EQ(kProtocolMagic[3], 0x56);  // 'V'
    const std::array<uint8_t, 4> expected = {0x45, 0x53, 0x50, 0x56};
    CHECK(kProtocolMagic == expected);
}

// U16 黄金字节向量。
void golden_u16() {
    uint8_t b[2] = {0, 0};
    writeU16LE(b, 0x0000);
    CHECK_EQ(b[0], 0x00);
    CHECK_EQ(b[1], 0x00);
    writeU16LE(b, 0x00FF);
    CHECK_EQ(b[0], 0xFF);
    CHECK_EQ(b[1], 0x00);
    writeU16LE(b, 0xFFFF);
    CHECK_EQ(b[0], 0xFF);
    CHECK_EQ(b[1], 0xFF);
    writeU16LE(b, 0x8000);
    CHECK_EQ(b[0], 0x00);
    CHECK_EQ(b[1], 0x80);
}

// U32 黄金字节向量（含 0x01020304 → 04 03 02 01）。
void golden_u32() {
    uint8_t b[4] = {0, 0, 0, 0};
    writeU32LE(b, 0x00000000u);
    CHECK_EQ(b[0], 0x00);
    CHECK_EQ(b[1], 0x00);
    CHECK_EQ(b[2], 0x00);
    CHECK_EQ(b[3], 0x00);
    writeU32LE(b, 0x01020304u);
    CHECK_EQ(b[0], 0x04);
    CHECK_EQ(b[1], 0x03);
    CHECK_EQ(b[2], 0x02);
    CHECK_EQ(b[3], 0x01);
    writeU32LE(b, 0xFFFFFFFFu);
    CHECK_EQ(b[0], 0xFF);
    CHECK_EQ(b[1], 0xFF);
    CHECK_EQ(b[2], 0xFF);
    CHECK_EQ(b[3], 0xFF);
    writeU32LE(b, 0x80000000u);
    CHECK_EQ(b[0], 0x00);
    CHECK_EQ(b[1], 0x00);
    CHECK_EQ(b[2], 0x00);
    CHECK_EQ(b[3], 0x80);
}

// U64 黄金字节向量（0x0102030405060708 → 08 07 06 05 04 03 02 01）。
void golden_u64() {
    uint8_t b[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    writeU64LE(b, 0x0102030405060708ull);
    const uint8_t expected[8] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    CHECK(std::memcmp(b, expected, 8) == 0);
    writeU64LE(b, 0xFFFFFFFFFFFFFFFFull);
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(b[i], 0xFF);
    }
    writeU64LE(b, 0x0ull);
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(b[i], 0x00);
    }
}

// 写→读 round-trip 奇偶校验（同一 buffer 或跨 buffer）。
void roundtrip_parity() {
    const uint16_t u16vals[] = {0, 1, 0x00FF, 0x0100, 0x8000, 0xFFFF, 0x1234, 0xABCD};
    for (const uint16_t v : u16vals) {
        uint8_t b[2];
        writeU16LE(b, v);
        CHECK_EQ(readU16LE(b), v);
    }
    const uint32_t u32vals[] = {0u,          1u,          0x0000FFFFu, 0xFFFF0000u,
                                0x80000000u, 0xFFFFFFFFu, 0x01020304u, 0xDEADBEEFu};
    for (const uint32_t v : u32vals) {
        uint8_t b[4];
        writeU32LE(b, v);
        CHECK_EQ(readU32LE(b), v);
    }
    const uint64_t u64vals[] = {0ull,
                                1ull,
                                0x00000000FFFFFFFFull,
                                0xFFFFFFFF00000000ull,
                                0x8000000000000000ull,
                                0xFFFFFFFFFFFFFFFFull,
                                0x0102030405060708ull};
    for (const uint64_t v : u64vals) {
        uint8_t b[8];
        writeU64LE(b, v);
        CHECK_EQ(readU64LE(b), v);
    }
    // 跨写读：写入相邻字段（模拟 header 布局）后按偏移读取。
    uint8_t hdr[8];
    writeU16LE(hdr, 0x1234);      // 偏移 0
    writeU32LE(hdr + 2, 0x01020304u);  // 偏移 2
    writeU16LE(hdr + 6, 0x00FF);  // 偏移 6
    CHECK_EQ(readU16LE(hdr), 0x1234);
    CHECK_EQ(readU32LE(hdr + 2), 0x01020304u);
    CHECK_EQ(readU16LE(hdr + 6), 0x00FF);
}

// 与 packet/message 编码语义一致性：makeHeader + encodeHeaderToArray 的 LE 字段
// 可由 byte_order 黄金向量验证（SEQ/LENGTH/CRC32/RSVD2）。
void header_layout_consistency() {
    using espview::proto::PacketHeader;
    using espview::proto::encodeHeaderToArray;
    using espview::proto::makeHeader;
    using espview::proto::kPacketHeaderSize;

    const PacketHeader h = makeHeader(0x03, 0x02, 0x1234, 0x01020304u);
    const std::array<uint8_t, kPacketHeaderSize> raw = encodeHeaderToArray(h);
    // SEQ（偏移 8，LE）
    CHECK_EQ(raw[8], 0x34);
    CHECK_EQ(raw[9], 0x12);
    CHECK_EQ(readU16LE(raw.data() + 8), 0x1234);
    // LENGTH（偏移 10，LE）
    CHECK_EQ(raw[10], 0x04);
    CHECK_EQ(raw[11], 0x03);
    CHECK_EQ(raw[12], 0x02);
    CHECK_EQ(raw[13], 0x01);
    CHECK_EQ(readU32LE(raw.data() + 10), 0x01020304u);
}

}  // namespace

void runByteOrderTests() {
    std::printf("  magic_golden\n");
    magic_golden();
    std::printf("  golden_u16\n");
    golden_u16();
    std::printf("  golden_u32\n");
    golden_u32();
    std::printf("  golden_u64\n");
    golden_u64();
    std::printf("  roundtrip_parity\n");
    roundtrip_parity();
    std::printf("  header_layout_consistency\n");
    header_layout_consistency();
}
