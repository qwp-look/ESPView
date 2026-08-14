// ESPView — Message Encoder（M0-B1）
//
// 规范来源：docs/DESIGN.md E 节「三层概念」：
//   - Message 载荷 <= 4096 B 时 = 1 个 Packet；
//   - > 4096 B 时拆为连续 n 个 Packet（前 n-1 个 CHUNKED=1，末包 CHUNKED=0）；
//   - 拆包是"消息→包"层行为，与帧边界无关。
//
// Encoder 职责边界：只做 逻辑 Message → 确定性 Packet 序列（完整包，含 CRC）。
// 不负责：UART / buffering / retransmission / timing / ACK retry / frame assembly / 渲染。
//
// 纯 C++17，零平台依赖。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "message.h"
#include "packet.h"
#include "protocol.h"

namespace espview {
namespace proto {

// 每方向独立的 Packet SEQ 提供者（uint16_t，回绕）。
// 设计为可注入的小接口，避免把全局静态计数器藏在 Encoder 内部。
class ISequenceProvider {
public:
    virtual ~ISequenceProvider() = default;

    // 返回当前序号并前进。回绕行为：0xFFFF → 0x0000（uint16_t 截断，已定义）。
    // 注意：Encoder 本身不假设 SEQ 永不回绕，每生成一个 Packet 恰好消耗一个 SEQ。
    virtual uint16_t next() = 0;
};

// 默认实现：简单递增计数器（可指定初值，便于测试 65535 → 0 回绕）。
class SequenceCounter : public ISequenceProvider {
public:
    explicit SequenceCounter(uint16_t initial = 0) : seq_(initial) {}

    uint16_t next() override {
        const uint16_t cur = seq_;
        seq_ = static_cast<uint16_t>(seq_ + 1);  // 回绕：0xFFFF → 0x0000
        return cur;
    }

    // 复位（DESIGN.md 连接状态机：HANDSHAKE 阶段双方 packet.seq 清零）。
    // 由会话层（ProtocolEndpoint）在握手完成/重连时调用。
    void reset(uint16_t initial = 0) { seq_ = initial; }

private:
    uint16_t seq_;
};

// 流式载荷来源（M1-3C）：按需产生 Message payload 字节，不要求整段驻留内存。
// 平台无关；典型实现：写入 RectPatternSource（ESP32 TestPattern）或测试用
// BytePatternSource。read() 向 dst 写入最多 maxBytes 字节，返回实际写入数；
// 返回 0 表示 EOF（载荷结束）。允许任意切分（1..maxBytes），Encoder 内部
// 负责 packet 级 staging，与完整载荷编码逐位等价。
class IMessagePayloadSource {
public:
    virtual ~IMessagePayloadSource() = default;
    virtual size_t read(uint8_t* dst, size_t maxBytes) = 0;
};

// Message → 1..N 个完整 Packet（20B 头 + 载荷片段 + CRC）。
//
// 规则（与 DESIGN.md 一致）：
//   - 载荷 <= kMaxPacketPayload：1 个 Packet，CHUNKED 清除；
//   - 载荷 >  kMaxPacketPayload：连续 n 个 Packet，前 n-1 个 CHUNKED=1，末包 CHUNKED=0；
//   - 所有 Packet 的 TYPE == msg.type；
//   - SEQ 每包 +1，来自注入的 ISequenceProvider；
//   - CHUNKED 位由 Encoder 管理；msg.flags 中的其他位（如 ACK_REQ）原样写入每个 Packet；
//   - 不做任何 chunk 内额外头（DESIGN.md 未定义，禁止创造）。
class MessageEncoder {
public:
    explicit MessageEncoder(ISequenceProvider& seq) : seq_(seq) {}

    // 编码 msg 为 1..N 个完整 Packet 字节序列。
    // 成功：返回 kNone，out 清空后按序填入；
    // 失败（如 TYPE 越界）：返回 PacketError，out 清空。
    PacketError encode(const Message& msg, std::vector<std::vector<uint8_t>>& out);

    // 流式编码（长消息低内存路径）：逐包调用 sink；sink 返回 false 立即终止
    // （返回 kSinkAborted；已消耗的 SEQ 不回滚，与 encode() 失败语义一致）。
    // 与 encode() 生成的包字节逐位一致（同一拆分规则 / CHUNKED 位 / SEQ 消耗）。
    // 用途：ESP32 上发送 153 KB 级 FRAME_RECT 时避免一次性物化全部 Packet
    //   （峰值内存从 payload+全量包 降到 payload+单包）。
    PacketError encodeStream(
        const Message& msg,
        const std::function<bool(const uint8_t* data, size_t len)>& sink);

    // 流式编码（M1-3C）：payload 不驻留内存，由 source 按需产生。
    // 与 encode()/encodeStream() 对同一逻辑载荷生成的 Packet 字节逐位一致
    // （同一拆分规则 / CHUNKED 位 / SEQ 消耗），CRC 由 encodePacket 计算。
    // sink 返回 false 立即终止（返回 kSinkAborted；已消耗 SEQ 不回滚）。
    // 用途：经典 ESP32 无 PSRAM 时发送 153608B 级 FRAME_RECT，峰值内存
    //   = kMaxPacketPayload staging + 单包缓冲（不再要求整段 payload）。
    PacketError encodeStreaming(
        const MessageHeader& header, IMessagePayloadSource& source,
        const std::function<bool(const uint8_t* data, size_t len)>& sink);

private:
    ISequenceProvider& seq_;
};

}  // namespace proto
}  // namespace espview

