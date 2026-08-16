// ESPView — Message Encoder（M0-B1 / M8-A1）
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

#include <array>
#include <atomic>
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
// M8-A1：seq_ 为 std::atomic<uint16_t>（RX/transport 线程 reset 与 TX 线程
// next() 可并发；relaxed 语义足够——单次 fetch_add 原子，无需跨线程排序）。
class SequenceCounter : public ISequenceProvider {
public:
    explicit SequenceCounter(uint16_t initial = 0) : seq_(initial) {}

    uint16_t next() override {
        // 返回旧值；uint16_t 算术回绕：0xFFFF → 0x0000。
        return seq_.fetch_add(1, std::memory_order_relaxed);
    }

    // 复位（DESIGN.md 连接状态机：HANDSHAKE 阶段双方 packet.seq 清零）。
    // 由会话层（ProtocolEndpoint）在握手完成/重连时调用。
    void reset(uint16_t initial = 0) { seq_.store(initial, std::memory_order_relaxed); }

private:
    std::atomic<uint16_t> seq_;
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
//
// M8-A1（1 MiB 上限 + 单缓冲 staging）：
//   - 逻辑 Message 完整 payload（所有 CHUNKED Packet 拼接后）不得超过
//     kMaxMessagePayload（1 MiB）：encode()/encodeStream() 在入口按
//     msg.payload.size() 拒绝；encodeStreaming() 在发出每包前按累计
//     emittedPayload 检查，超限返回 kMessageTooLarge。超限时已发出的
//     prefix 保留在 wire 上（与 kSinkAborted 语义一致：已消耗的 SEQ 不回滚）；
//     恰好 1 MiB → kNone（256 个满包）。
//   - 热路径复用单个 packetBuf_（20B 头 + 4096B payload，4116B），
//     encodeStreaming/encodeStream 全程零堆分配；payload 填在
//     packetBuf_.data()+kPacketHeaderSize，整包从 packetBuf_.data() 发出。
//   - ACK_REQ 白名单（allowedAckRequestType）：白名单外类型携带 ACK_REQ →
//     PacketError::kInvalidAckReq（实现层错误，非 wire 格式变化）；ACK_REQ 且
//     payload > kMaxPacketPayload（单包规则）同样拒绝；encodeStreaming 路径
//     对任何类型一律拒绝 ACK_REQ（与 Endpoint 流式发送面一致）。
class MessageEncoder {
public:
    explicit MessageEncoder(ISequenceProvider& seq) : seq_(seq) {}

    // 编码 msg 为 1..N 个完整 Packet 字节序列。
    // 成功：返回 kNone，out 清空后按序填入；
    // 失败（TYPE 越界 / payload > 1 MiB / ACK_REQ 白名单违规）：返回 PacketError，out 清空。
    PacketError encode(const Message& msg, std::vector<std::vector<uint8_t>>& out);

    // 流式编码（长消息低内存路径）：逐包调用 sink；sink 返回 false 立即终止
    // （返回 kSinkAborted；已消耗的 SEQ 不回滚，与 encode() 失败语义一致）。
    // 与 encode() 生成的包字节逐位一致（同一拆分规则 / CHUNKED 位 / SEQ 消耗）。
    // 用途：ESP32 上发送 153 KB 级 FRAME_RECT 时避免一次性物化全部 Packet
    //   （峰值内存从 payload+全量包 降到 payload+单包）。
    // M8-A1：单包缓冲复用 packetBuf_（无每包堆分配）；1 MiB 上限与 ACK_REQ
    // 白名单同 encode()。
    PacketError encodeStream(
        const Message& msg,
        const std::function<bool(const uint8_t* data, size_t len)>& sink);

    // 流式编码（M1-3C）：payload 不驻留内存，由 source 按需产生。
    // 与 encode()/encodeStream() 对同一逻辑载荷生成的 Packet 字节逐位一致
    // （同一拆分规则 / CHUNKED 位 / SEQ 消耗），CRC 由 encodePacket 计算。
    // sink 返回 false 立即终止（返回 kSinkAborted；已消耗 SEQ 不回滚）。
    // 用途：经典 ESP32 无 PSRAM 时发送 153608B 级 FRAME_RECT，峰值内存
    //   = 单个 packetBuf_（4116B），不要求整段 payload 驻留内存。
    // M8-A1：1 MiB 上限在每包发出前累计检查（恰好 1 MiB → kNone；
    // 超限 → kMessageTooLarge：绝不发出超出上限的字节，已发出的 prefix 保留；
    // 注意超限前可能已从 source 多读约 4096+1 字节（满块填充 + 1 字节探测）——
    // 只是不发出，不会精确停在超限边界处）；
    // 不接受 ACK_REQ（任何类型 → kInvalidAckReq，与 transmitStreamingImpl 一致）；
    // source.read 返回值超 maxBytes 时截断（防御，防 staging 溢出）。
    // 重入约束：sink 回调不得重入同一 ProtocolEndpoint 的阻塞式
    //   sendMessage/sendMessageStreaming（sendMutex_ 不可重入 → 自死锁）。
    PacketError encodeStreaming(
        const MessageHeader& header, IMessagePayloadSource& source,
        const std::function<bool(const uint8_t* data, size_t len)>& sink);

private:
    ISequenceProvider& seq_;

    // M8-A1：单包缓冲（20B 头 + 4096B payload = 4116B）。encodeStreaming /
    // encodeStream 热路径复用，避免每包堆分配。payload 字节从
    // data()+kPacketHeaderSize 起填；encodeStreaming 的 1 字节 hasMore 探测
    // 字节先读入局部变量（发出当前包前不能落在 packetBuf_[kPacketHeaderSize]，
    // 否则覆盖当前包 payload 首字节），当前包发出后复制到
    // packetBuf_[kPacketHeaderSize]（= 下一包 payload 首字节），filled=1 续用。
    std::array<uint8_t, kPacketHeaderSize + kMaxPacketPayload> packetBuf_{};
};

}  // namespace proto
}  // namespace espview
