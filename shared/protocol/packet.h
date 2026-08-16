// ESPView — Packet 层（固定 20 字节头 + 载荷）（M0-A）
//
// 规范来源：docs/DESIGN.md E 节「最终 Packet Header / CRC32 规范」。
// 仅实现 Packet 的编解码与 CRC 校验；Message/Frame 层（encoder/decoder/
// frame_assembler）不在 M0-A 范围。
//
// 设计约束：
//   - 禁止把 byte buffer reinterpret_cast 为 PacketHeader（内存布局不保证）。
//   - 所有多字节整数按小端显式组装/解析，保证端序可移植。
//   - 错误路径使用 PacketError 枚举返回，不使用异常。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protocol.h"

namespace espview {
namespace proto {

// 固定包头大小（DESIGN.md：固定 20 字节）。
constexpr size_t kPacketHeaderSize = 20;

enum class PacketError : uint8_t {
    kNone = 0,
    kBufferTooSmall,      // 输入/输出缓冲长度不足
    kInvalidMagic,        // MAGIC 不匹配
    kUnsupportedVersion,  // VERSION != 0x01
    kInvalidType,         // TYPE 超出 0x01..0x51
    kInvalidLength,       // LENGTH > 4096，或与载荷长度不一致
    kCrcMismatch,         // CRC32 校验失败
    kMessageTooLarge,     // Message payload 超过 kMaxMessagePayload（Encoder 使用）
    kSinkAborted,         // encodeStream 的 sink 返回 false（发送中止，非编码错误）
    kInvalidAckReq,       // M8-A1：ACK_REQ 出现在白名单外类型上（实现层错误，非 wire 格式变化）
};

// Header 的逻辑表示（与 20 字节线格式分离）。
struct PacketHeader {
    std::array<uint8_t, 4> magic{};  // 期望 kProtocolMagic
    uint8_t version = 0;             // 期望 kProtocolVersion
    uint8_t type = 0;                // MessageType
    uint8_t flags = 0;               // kFlagChunked / kFlagAckReq
    uint8_t rsvd = 0;                // 保留，接收方必须忽略
    uint16_t seq = 0;                // 小端，0..65535 回绕
    uint32_t length = 0;             // 小端，0..kMaxPacketPayload
    uint32_t crc32 = 0;              // 小端，覆盖 header[0,14) + payload
    uint16_t rsvd2 = 0;              // 保留，接收方必须忽略
};

// 构造合法 Header：magic/version/rsvd/crc32/rsvd2 自动填充，只需给出 type/flags/seq/length。
PacketHeader makeHeader(uint8_t type, uint8_t flags, uint16_t seq, uint32_t length);

// 序列化 Header 到 out（小端）。outSize 必须 >= kPacketHeaderSize，否则 kBufferTooSmall。
// h.length > kMaxPacketPayload 时返回 kInvalidLength。
// 成功时 *written = kPacketHeaderSize；失败时 *written = 0。
PacketError encodeHeader(const PacketHeader& h, uint8_t* out, size_t outSize, size_t* written);

// 便捷：固定 20 字节输出的序列化。无失败路径，调用方需保证 header 合法（如由 makeHeader 构造）。
std::array<uint8_t, kPacketHeaderSize> encodeHeaderToArray(const PacketHeader& h);

// 从 in 解码 Header。校验顺序：缓冲大小、MAGIC、VERSION、TYPE 范围、LENGTH 上限。
// RSVD / RSVD2 按 DESIGN.md「接收方必须忽略」，非零不拒绝，仅原样读入结构体。
// 成功时 out 的 magic 归一化为 kProtocolMagic（与校验通过等价）。
PacketError decodeHeader(const uint8_t* in, size_t inSize, PacketHeader* out);

// 完整包编码：20 字节头 + payload，自动计算并写入 CRC（覆盖 header[0,14) + payload）。
// 要求 h.length == payloadLen 且 <= kMaxPacketPayload，否则 kInvalidLength；
// outSize 必须 >= kPacketHeaderSize + payloadLen，否则 kBufferTooSmall。
// 成功时 *written = kPacketHeaderSize + payloadLen；失败时 *written = 0。
PacketError encodePacket(const PacketHeader& h, const uint8_t* payload, size_t payloadLen,
                         uint8_t* out, size_t outSize, size_t* written);

// CRC 校验：按 header[0,14) + payload 重算并与 h.crc32 比对。
// payloadLen 必须等于 h.length，否则返回 kInvalidLength；不一致返回 kCrcMismatch。
PacketError verifyPacketCrc(const PacketHeader& h, const uint8_t* payload, size_t payloadLen);

// 计算完整包 CRC（供测试与后续 M0-B decoder 使用）。
uint32_t computePacketCrc(const PacketHeader& h, const uint8_t* payload, size_t payloadLen);

// PacketError 的可读名称（调试/测试输出用）。
const char* toString(PacketError e);

}  // namespace proto
}  // namespace espview
