#include "crc32.h"

#include <array>

namespace espview {
namespace proto {

namespace {

// 反射形式 CRC-32 查找表（polynomial 0xEDB88320）。
const std::array<uint32_t, 256>& crc32Table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

}  // namespace

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    const std::array<uint32_t, 256>& table = crc32Table();
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    return crc32Update(kCrc32Init, data, len) ^ kCrc32FinalXor;
}

}  // namespace proto
}  // namespace espview
