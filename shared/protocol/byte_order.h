// ESPView — 字节序辅助（LE 读写，header-only）（M8-A1）
//
// 规范来源：docs/DESIGN.md E 节「最终 Packet Header / 控制消息 Payload Layout」：
//   所有多字节整数按小端（LE）显式组装/解析，保证端序可移植（禁止
//   reinterpret_cast / 未对齐访问）。
// 仅 shared/protocol 内部与测试使用；不改变 wire format（wire 字节序冻结）。
// 网络序字段（IP/Server IP，message.cpp readNetU32 / pushNetU32）不在此列——
// 那些是显式大端，保持原样，勿用本文件改写。
// 纯 C++17，零平台依赖。所有函数无越界检查：调用方必须先保证缓冲区足够。

#pragma once

#include <cstdint>

namespace espview {
namespace proto {

// ---- 小端（LE）读写 ----

inline uint16_t readU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline void writeU16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void writeU32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline uint64_t readU64LE(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

inline void writeU64LE(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFFu);
    }
}

}  // namespace proto
}  // namespace espview
