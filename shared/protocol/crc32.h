// ESPView — CRC-32（IEEE/zlib）实现（M0-A）
//
// 规范来源：docs/DESIGN.md E 节「CRC32 规范」：
//   - 算法：IEEE 802.3 / zlib CRC-32
//   - polynomial：0xEDB88320（反射形式）
//   - 初始值：0xFFFFFFFF
//   - reflected input / output：是 / 是
//   - final XOR：0xFFFFFFFF
//   - 标准向量：CRC32("123456789") == 0xCBF43926
// 纯 C++17，零平台依赖。

#pragma once

#include <cstddef>
#include <cstdint>

namespace espview {
namespace proto {

constexpr uint32_t kCrc32Init = 0xFFFFFFFFu;
constexpr uint32_t kCrc32FinalXor = 0xFFFFFFFFu;

// 增量更新：用 data[0..len) 更新运行中的 CRC 值。
// 语义与 zlib 的 crc32(crc, buf, len) 内部一致（不含 API 层的 init/final xor）。
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

// 计算 data[0..len) 的完整 CRC-32（init + update + final xor）。
// 标准向量：crc32("123456789") == 0xCBF43926。
uint32_t crc32(const uint8_t* data, size_t len);

}  // namespace proto
}  // namespace espview
