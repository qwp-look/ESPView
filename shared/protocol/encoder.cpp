#include "encoder.h"

#include <algorithm>
#include <utility>

namespace espview {
namespace proto {

PacketError MessageEncoder::encode(const Message& msg,
                                   std::vector<std::vector<uint8_t>>& out) {
    out.clear();
    return encodeStream(msg, [&out](const uint8_t* data, size_t len) {
        out.emplace_back(data, data + len);
        return true;
    });
}

PacketError MessageEncoder::encodeStream(
    const Message& msg, const std::function<bool(const uint8_t* data, size_t len)>& sink) {
    if (msg.type < kMinMessageType || msg.type > kMaxMessageType) {
        return PacketError::kInvalidType;
    }
    // MAX_MESSAGE_PAYLOAD（DESIGN.md E 节）：完整 payload 超过 1 MiB 必须拒绝。
    if (msg.payload.size() > kMaxMessagePayload) {
        return PacketError::kMessageTooLarge;
    }
    // M8-A1 ACK_REQ 白名单：仅 SET_MODE/WIFI_SCAN_REQ/WIFI_CONFIG 可置 ACK_REQ。
    if ((msg.flags & kFlagAckReq) != 0 && !allowedAckRequestType(msg.type)) {
        return PacketError::kInvalidAckReq;
    }
    // ACK_REQ 仅限单包控制消息（DESIGN.md）：payload > 4096 与 ACK_REQ 互斥。
    if ((msg.flags & kFlagAckReq) != 0 && msg.payload.size() > kMaxPacketPayload) {
        return PacketError::kInvalidAckReq;
    }

    const size_t payloadSize = msg.payload.size();
    if (payloadSize <= kMaxPacketPayload) {
        // 1 Message = 1 Packet，CHUNKED 清除。
        const PacketHeader h = makeHeader(msg.type, msg.flags & ~kFlagChunked, seq_.next(),
                                          static_cast<uint32_t>(payloadSize));
        size_t written = 0;
        const PacketError err = encodePacket(h, msg.payload.data(), payloadSize,
                                             packetBuf_.data(), packetBuf_.size(), &written);
        if (err != PacketError::kNone) {
            return err;
        }
        if (!sink(packetBuf_.data(), kPacketHeaderSize + payloadSize)) {
            return PacketError::kSinkAborted;
        }
        return PacketError::kNone;
    }

    // 载荷 > 4096：拆为连续 n 个包；前 n-1 个 CHUNKED=1，末包 CHUNKED=0。
    size_t offset = 0;
    while (offset < payloadSize) {
        const size_t chunk =
            std::min(static_cast<size_t>(kMaxPacketPayload), payloadSize - offset);
        const bool last = (offset + chunk == payloadSize);
        uint8_t flags = msg.flags & ~kFlagChunked;
        if (!last) {
            flags |= kFlagChunked;
        }
        const PacketHeader h = makeHeader(msg.type, flags, seq_.next(),
                                          static_cast<uint32_t>(chunk));
        size_t written = 0;
        const PacketError err = encodePacket(h, msg.payload.data() + offset, chunk,
                                             packetBuf_.data(), packetBuf_.size(), &written);
        if (err != PacketError::kNone) {
            return err;
        }
        if (!sink(packetBuf_.data(), kPacketHeaderSize + chunk)) {
            return PacketError::kSinkAborted;
        }
        offset += chunk;
    }
    return PacketError::kNone;
}

PacketError MessageEncoder::encodeStreaming(
    const MessageHeader& header, IMessagePayloadSource& source,
    const std::function<bool(const uint8_t* data, size_t len)>& sink) {
    if (header.type < kMinMessageType || header.type > kMaxMessageType) {
        return PacketError::kInvalidType;
    }
    // M8-A1 ACK_REQ：流式路径从不支持 ACK_REQ（ACK_REQ 只允许单包控制消息，
    // 见 Endpoint transmitStreamingImpl）；对任何类型一律拒绝——同时覆盖白名单
    // 与 "ACK_REQ 且 payload>kMaxPacketPayload" 两条规则。
    if ((header.flags & kFlagAckReq) != 0) {
        return PacketError::kInvalidAckReq;
    }

    // 统一 staging + 输出缓冲：payload 填在 packetBuf_[kPacketHeaderSize..)，
    // 整包（20B 头 + 载荷）从 packetBuf_[0) 发出；单次 encodeStreaming 零堆分配。
    // packet 级 staging 保证 source 的任意切分都先聚到 4096 再发，与 encode()
    // 的拆分规则（4096 块 + 末块）逐位一致。
    size_t filled = 0;
    bool eof = false;
    size_t emittedPayload = 0;  // 已发出 payload 字节数（消息级累计，非成员）

    while (true) {
        // 1) 填满一个 packet 的载荷（或遇到 EOF）。
        //    防御：misbehaving source 返回超过 maxBytes 时截断，不得溢出 staging。
        while (filled < kMaxPacketPayload && !eof) {
            size_t n = source.read(packetBuf_.data() + kPacketHeaderSize + filled,
                                   kMaxPacketPayload - filled);
            if (n > kMaxPacketPayload - filled) {
                n = kMaxPacketPayload - filled;
            }
            if (n == 0) {
                eof = true;
            } else {
                filled += n;
            }
        }

        if (filled == 0) {
            // 空载荷：1 个包，len=0，CHUNKED=0（与 encode() 对空 payload 一致）。
            const PacketHeader h = makeHeader(header.type, header.flags & ~kFlagChunked,
                                              seq_.next(), 0);
            size_t written = 0;
            const PacketError err =
                encodePacket(h, packetBuf_.data() + kPacketHeaderSize, 0,
                             packetBuf_.data(), packetBuf_.size(), &written);
            if (err != PacketError::kNone) {
                return err;
            }
            if (!sink(packetBuf_.data(), kPacketHeaderSize)) {
                return PacketError::kSinkAborted;
            }
            return PacketError::kNone;
        }

        // 2) 探测是否还有后续数据（读 1 字节；不回退 source）。
        //    只有 !eof 时才可能恰好填满 4096；此时必须区分"满块即末块"与"满块后还有"。
        //    探测字节读入局部变量（不能在发出当前包前直接落在 packetBuf_[20]——
        //    那会覆盖当前包 payload 首字节）；当前包发出后复制到
        //    packetBuf_[kPacketHeaderSize] 作为下一包 payload 首字节（filled=1）。
        bool hasMore = false;
        uint8_t probe = 0;
        if (!eof) {
            size_t n = source.read(&probe, 1);
            if (n > 1) {
                n = 1;
            }
            if (n == 1) {
                hasMore = true;
            } else {
                eof = true;
            }
        }

        // 3) M8-A1 1 MiB 上限：发出本包后累计不得超限。
        //    超限部分不发出（可能已从 source 多读约 4096+1 字节——满块填充 +
        //    1 字节探测；读到但绝不发出，不保证停在超限边界精确处）；已发出的
        //    prefix 保留（与 kSinkAborted 语义一致：已消耗的 SEQ 不回滚）。
        //    恰好 1 MiB → 256 个满包 → kNone。
        const bool last = !hasMore;
        if (emittedPayload + filled > kMaxMessagePayload) {
            return PacketError::kMessageTooLarge;
        }

        // 4) 发出当前块；末块 CHUNKED=0，其余 CHUNKED=1。
        uint8_t flags = header.flags & ~kFlagChunked;
        if (!last) {
            flags |= kFlagChunked;
        }
        const PacketHeader h =
            makeHeader(header.type, flags, seq_.next(), static_cast<uint32_t>(filled));
        size_t written = 0;
        const PacketError err = encodePacket(h, packetBuf_.data() + kPacketHeaderSize, filled,
                                             packetBuf_.data(), packetBuf_.size(), &written);
        if (err != PacketError::kNone) {
            return err;
        }
        if (!sink(packetBuf_.data(), kPacketHeaderSize + filled)) {
            return PacketError::kSinkAborted;
        }
        emittedPayload += filled;
        if (last) {
            return PacketError::kNone;
        }

        // 5) 探测字节成为下一块的首字节（packetBuf_[kPacketHeaderSize]，filled=1）。
        packetBuf_[kPacketHeaderSize] = probe;
        filled = 1;
    }
}

}  // namespace proto
}  // namespace espview
