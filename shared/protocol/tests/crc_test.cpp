// CRC-32 单元测试（M0-A）。
// 规范来源：docs/DESIGN.md E 节「CRC32 规范」。

#include <cstdint>
#include <cstdio>

#include "crc32.h"
#include "test_util.h"

namespace {

using espview::proto::crc32;
using espview::proto::crc32Update;
using espview::proto::kCrc32FinalXor;
using espview::proto::kCrc32Init;

// TEST: crc_standard_vector
// CRC32("123456789") == 0xCBF43926（DESIGN.md 规定的交叉验证向量）。
void crc_standard_vector() {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc32(data, 9), 0xCBF43926u);
}

// 空输入：zlib crc32(0, NULL, 0) == 0。
void crc_empty() {
    CHECK_EQ(crc32(nullptr, 0), 0u);
}

// 增量 update 与一次性计算等价（覆盖 DESIGN.md 的"header 分段 + payload 分段"用法）。
void crc_incremental_matches_oneshot() {
    const uint8_t data[] = {0x00, 0x01, 0x02, 0x03, 0x80, 0xFF, 0x10, 0x20,
                            0x30, 0x40, 0x50, 0x60, 0x70, 0xAA, 0xBB, 0xCC};
    const uint32_t oneshot = crc32(data, sizeof(data));
    uint32_t running = kCrc32Init;
    running = crc32Update(running, data, 5);
    running = crc32Update(running, data + 5, 4);
    running = crc32Update(running, data + 9, 3);
    running = crc32Update(running, data + 12, sizeof(data) - 12);
    running ^= kCrc32FinalXor;
    CHECK_EQ(running, oneshot);
}

}  // namespace

void runCrcTests() {
    std::printf("  crc_standard_vector\n");
    crc_standard_vector();
    std::printf("  crc_empty\n");
    crc_empty();
    std::printf("  crc_incremental_matches_oneshot\n");
    crc_incremental_matches_oneshot();
}
