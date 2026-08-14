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

    const size_t payloadSize = msg.payload.size();
    if (payloadSize <= kMaxPacketPayload) {
        // 1 Message = 1 Packet，CHUNKED 清除。
        std::vector<uint8_t> buf(kPacketHeaderSize + payloadSize);
        const PacketHeader h = makeHeader(msg.type, msg.flags & ~kFlagChunked, seq_.next(),
                                          static_cast<uint32_t>(payloadSize));
        size_t written = 0;
        const PacketError err = encodePacket(h, msg.payload.data(), payloadSize, buf.data(),
                                             buf.size(), &written);
        if (err != PacketError::kNone) {
            return err;
        }
        if (!sink(buf.data(), buf.size())) {
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
        std::vector<uint8_t> buf(kPacketHeaderSize + chunk);
        const PacketHeader h = makeHeader(msg.type, flags, seq_.next(),
                                          static_cast<uint32_t>(chunk));
        size_t written = 0;
        const PacketError err = encodePacket(h, msg.payload.data() + offset, chunk, buf.data(),
                                             buf.size(), &written);
        if (err != PacketError::kNone) {
            return err;
        }
        if (!sink(buf.data(), buf.size())) {
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

    // packet 级 staging：source 的任意切分都先聚到 4096 再发，保证与
    // encode() 的拆分规则（4096 块 + 末块）逐位一致。
    std::vector<uint8_t> staging(kMaxPacketPayload);
    size_t filled = 0;
    bool eof = false;

    while (true) {
        // 1) 填满一个 packet 的载荷（或遇到 EOF）。
        while (filled < kMaxPacketPayload && !eof) {
            const size_t n = source.read(staging.data() + filled, kMaxPacketPayload - filled);
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
            std::vector<uint8_t> buf(kPacketHeaderSize);
            size_t written = 0;
            const PacketError err =
                encodePacket(h, staging.data(), 0, buf.data(), buf.size(), &written);
            if (err != PacketError::kNone) {
                return err;
            }
            if (!sink(buf.data(), buf.size())) {
                return PacketError::kSinkAborted;
            }
            return PacketError::kNone;
        }

        // 2) 探测是否还有后续数据（读 1 字节；不回退 source）。
        //    只有 !eof 时才可能恰好填满 4096；此时必须区分"满块即末块"与"满块后还有"。
        bool hasMore = false;
        uint8_t probe = 0;
        if (!eof) {
            const size_t n = source.read(&probe, 1);
            if (n == 1) {
                hasMore = true;
            } else {
                eof = true;
            }
        }

        // 3) 发出当前块；末块 CHUNKED=0，其余 CHUNKED=1。
        const bool last = !hasMore;
        uint8_t flags = header.flags & ~kFlagChunked;
        if (!last) {
            flags |= kFlagChunked;
        }
        std::vector<uint8_t> buf(kPacketHeaderSize + filled);
        const PacketHeader h =
            makeHeader(header.type, flags, seq_.next(), static_cast<uint32_t>(filled));
        size_t written = 0;
        const PacketError err = encodePacket(h, staging.data(), filled, buf.data(), buf.size(),
                                             &written);
        if (err != PacketError::kNone) {
            return err;
        }
        if (!sink(buf.data(), buf.size())) {
            return PacketError::kSinkAborted;
        }
        if (last) {
            return PacketError::kNone;
        }

        // 4) 探测字节是下一块的首字节。
        staging[0] = probe;
        filled = 1;
    }
}

}  // namespace proto
}  // namespace espview
