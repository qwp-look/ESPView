// ESPView — Streaming Message API Host Tests（M1-3C）
//
// 规范来源：docs/DESIGN.md E 节（三层概念 / CHUNKED 语义 / CRC）+ M1-3C 任务书。
// 目标：验证 MessageEncoder::encodeStreaming()：
//   - 与既有 encode() 对同一逻辑载荷生成的 Packet 字节逐位一致（含 Header/CRC）；
//   - CHUNKED 拆分规则不变（<=4096 单包；>4096 前 n-1 个 CHUNKED=1、末包 CHUNKED=0）；
//   - SEQ 每包 +1（含 65535 → 0 回绕）；
//   - 不要求整段 payload 驻留内存（IMessagePayloadSource 按需产生）；
//   - Streaming → StreamDecoder → FrameAssembler 对 153600B 单 RECT 全帧 round-trip；
//   - ProtocolEndpoint::sendMessageStreaming() 双端握手后跨端提交。
//
// 原则：协议数据全部由 shared/protocol 的 Encoder 产生；本测试不手工拼接协议字节；
// 测试侧为便于对照持有参考 vector（TEST ONLY，不代表生产内存模型）。
// 纯 C++17，零平台依赖。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "counting_allocator.h"
#include "test_util.h"

namespace {

using espview::proto::CommittedFrame;
using espview::proto::DecoderError;
using espview::proto::EndpointConfig;
using espview::proto::FrameAssembler;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameType;
using espview::proto::IMessagePayloadSource;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageHeader;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::RectInfo;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionState;
using espview::proto::StreamDecoder;
using espview::proto::kFlagAckReq;
using espview::proto::kFlagChunked;
using espview::proto::kMaxMessagePayload;
using espview::proto::kMaxPacketPayload;

// ---- 测试用 payload source：按 offset 实时产生字节，不持有整段向量 ----
class PatternSource : public IMessagePayloadSource {
public:
    PatternSource(size_t length, std::function<uint8_t(size_t offset)> byteAt)
        : length_(length), byteAt_(std::move(byteAt)) {}

    size_t read(uint8_t* dst, size_t maxBytes) override {
        size_t n = 0;
        while (n < maxBytes && pos_ < length_) {
            dst[n++] = byteAt_(pos_);
            ++pos_;
        }
        return n;
    }

private:
    size_t length_ = 0;
    size_t pos_ = 0;
    std::function<uint8_t(size_t offset)> byteAt_;
};

// ---- 参考 payload（测试侧 vector；仅用于与 encode() 对照）----
std::vector<uint8_t> makeReferencePayload(size_t len) {
    std::vector<uint8_t> v;
    v.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        v.push_back(static_cast<uint8_t>((i * 31u + 7u) & 0xFFu));
    }
    return v;
}

// ---- 收集 sink：把 encoder 输出按包收集 ----
class PacketCollector {
public:
    std::vector<std::vector<uint8_t>> packets;
    bool abortAfter = 0;  // 0 = 不中止；>0 = 第 N 包返回 false

    bool sink(const uint8_t* data, size_t len) {
        packets.emplace_back(data, data + len);
        return abortAfter == 0 || packets.size() < abortAfter;
    }
};

// 把收集到的包逐包 decode 回 PacketHeader + payload。
struct DecodedPacket {
    PacketHeader header;
    std::vector<uint8_t> payload;
};

bool decodeAll(const std::vector<std::vector<uint8_t>>& packets,
               std::vector<DecodedPacket>& out) {
    out.clear();
    for (const auto& p : packets) {
        DecodedPacket dp;
        if (espview::proto::decodeHeader(p.data(), p.size(), &dp.header) != PacketError::kNone) {
            return false;
        }
        dp.payload.assign(p.begin() + espview::proto::kPacketHeaderSize, p.end());
        if (espview::proto::verifyPacketCrc(dp.header, dp.payload.data(), dp.payload.size()) !=
            PacketError::kNone) {
            return false;
        }
        out.push_back(std::move(dp));
    }
    return true;
}

// ---- FRAME_RECT payload source（8B 头 + 320x240 RGB565 像素，按需产生）----
uint8_t rectByteAt(uint16_t frameId, uint16_t w, uint16_t h, size_t offset) {
    (void)h;  // 高度由 payload 总长度隐含；像素生成只依赖 w
    constexpr size_t kHeaderLen = 8;
    if (offset < kHeaderLen) {
        // x, y, w, h 各 2B LE
        static const uint8_t kHeader[kHeaderLen] = {0, 0, 0, 0, 0x40, 0x01, 0xF0, 0x00};
        return kHeader[offset];
    }
    const size_t pixelOff = offset - kHeaderLen;
    const size_t px = pixelOff / 2;
    const uint16_t x = static_cast<uint16_t>(px % w);
    const uint16_t y = static_cast<uint16_t>(px / w);
    return (pixelOff % 2 == 0) ? static_cast<uint8_t>(frameId + x)
                               : static_cast<uint8_t>(frameId + y + 1u);
}

// ---- 逐包比较两个编码结果 ----
bool packetsEqual(const std::vector<std::vector<uint8_t>>& a,
                  const std::vector<std::vector<uint8_t>>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// 运行等价性检查：encode(Message) vs encodeStreaming(header, source)
void checkEquivalence(size_t payloadLen) {
    const std::vector<uint8_t> ref = makeReferencePayload(payloadLen);
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};

    SequenceCounter seqA;
    MessageEncoder encA(seqA);
    std::vector<std::vector<uint8_t>> packetsA;
    Message msg;
    msg.type = header.type;
    msg.flags = header.flags;
    msg.payload = ref;
    CHECK_EQ(encA.encode(msg, packetsA), PacketError::kNone);

    SequenceCounter seqB;
    MessageEncoder encB(seqB);
    PacketCollector coll;
    PatternSource source(payloadLen,
                         [&ref](size_t off) { return ref[off]; });
    CHECK_EQ(encB.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kNone);

    CHECK_MSG(packetsEqual(packetsA, coll.packets),
              "payloadLen=" + std::to_string(payloadLen));
}

// ---- StreamDecoder + FrameAssembler 收集器（round-trip 验证）----
struct FrameCollector {
    std::vector<FrameBeginInfo> begins;
    std::vector<RectInfo> rects;
    std::vector<std::vector<uint8_t>> pixels;
    std::vector<CommittedFrame> commits;
    std::vector<FrameDiscardReason> discards;
    std::vector<DecoderError> decoderErrors;

    void onBegin(const FrameBeginInfo& b) { begins.push_back(b); }
    void onRect(const RectInfo& r, const uint8_t* p, size_t n) {
        rects.push_back(r);
        pixels.emplace_back(p, p + n);
    }
    void onCommit(const CommittedFrame& f) { commits.push_back(f); }
    void onDiscard(FrameDiscardReason r) { discards.push_back(r); }
    void onStreamError(DecoderError e) {
        decoderErrors.push_back(e);
        // 与生产接线一致：错误转发给 assembler 作废当前帧。
    }
};

// ---- 测试 1：空 payload ----
void streaming_empty_payload() {
    MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(0, [](size_t) { return 0; });
    CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kNone);
    CHECK_EQ(coll.packets.size(), 1u);
    std::vector<DecodedPacket> dps;
    CHECK(decodeAll(coll.packets, dps));
    CHECK_EQ(dps[0].header.length, 0u);
    CHECK_EQ(dps[0].header.flags & kFlagChunked, 0u);
    CHECK_EQ(dps[0].payload.size(), 0u);
}

// ---- 测试 2：单字节 payload ----
void streaming_small_payload() {
    MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(1, [](size_t) { return 0xAB; });
    CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kNone);
    CHECK_EQ(coll.packets.size(), 1u);
    std::vector<DecodedPacket> dps;
    CHECK(decodeAll(coll.packets, dps));
    CHECK_EQ(dps[0].header.length, 1u);
    CHECK_EQ(dps[0].payload[0], 0xAB);
}

// ---- 测试 3：4095 / 4096 / 4097 边界 ----
void streaming_boundary_sizes() {
    for (const size_t len : {size_t(4095), size_t(4096), size_t(4097)}) {
        const std::vector<uint8_t> ref = makeReferencePayload(len);
        const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
        SequenceCounter seq;
        MessageEncoder enc(seq);
        PacketCollector coll;
        PatternSource source(len, [&ref](size_t off) { return ref[off]; });
        CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                     coll.sink(d, n);
                     return true;
                 }),
                 PacketError::kNone);
        const size_t expectPackets = (len <= kMaxPacketPayload) ? 1 : 2;
        CHECK_EQ(coll.packets.size(), expectPackets);
        std::vector<DecodedPacket> dps;
        CHECK(decodeAll(coll.packets, dps));
        for (size_t i = 0; i < dps.size(); ++i) {
            const bool last = (i + 1 == dps.size());
            CHECK_EQ(static_cast<bool>(dps[i].header.flags & kFlagChunked), !last);
        }
        checkEquivalence(len);
    }
}

// ---- 测试 4：153608B 大消息拆包 + 等价 ----
void streaming_large_chunked() {
    constexpr size_t kLen = 153608;
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(kLen, [](size_t off) {
        return static_cast<uint8_t>((off * 13u + 5u) & 0xFFu);
    });
    CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kNone);
    const size_t expectPackets = (kLen + kMaxPacketPayload - 1) / kMaxPacketPayload;  // 38
    CHECK_EQ(coll.packets.size(), expectPackets);
    std::vector<DecodedPacket> dps;
    CHECK(decodeAll(coll.packets, dps));
    // CHUNKED：前 n-1 个 =1，末包 =0；SEQ 连续；TYPE 一致。
    for (size_t i = 0; i < dps.size(); ++i) {
        const bool last = (i + 1 == dps.size());
        CHECK_EQ(static_cast<bool>(dps[i].header.flags & kFlagChunked), !last);
        CHECK_EQ(dps[i].header.seq, static_cast<uint16_t>(i));
        CHECK_EQ(dps[i].header.type, static_cast<uint8_t>(MessageType::kFrameRect));
    }
    // 载荷拼接恢复
    size_t total = 0;
    for (const auto& dp : dps) {
        total += dp.payload.size();
    }
    CHECK_EQ(total, kLen);
    checkEquivalence(kLen);
}

// ---- 测试 5：SEQ 65535 → 0 回绕 ----
void streaming_seq_rollover() {
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq(65535);
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(4097, [](size_t off) { return static_cast<uint8_t>(off); });
    CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kNone);
    CHECK_EQ(coll.packets.size(), 2u);
    std::vector<DecodedPacket> dps;
    CHECK(decodeAll(coll.packets, dps));
    CHECK_EQ(dps[0].header.seq, 65535);
    CHECK_EQ(dps[1].header.seq, 0);
}

// ---- 测试 6：source 极小分块（1..3B/read）也必须逐位等价 ----
void streaming_partial_source_reads() {
    constexpr size_t kLen = 10000;
    const std::vector<uint8_t> ref = makeReferencePayload(kLen);
    const MessageHeader h{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(kLen, [&ref](size_t off) { return ref[off]; });
    // 用包装 source 限制每次最多 3 字节（模拟任意切分）。
    IMessagePayloadSource* raw = &source;
    struct TinySource : public IMessagePayloadSource {
        IMessagePayloadSource* inner;
        size_t read(uint8_t* dst, size_t maxBytes) override {
            return inner->read(dst, std::min<size_t>(3, maxBytes));
        }
    } tiny;
    tiny.inner = raw;
    CHECK_EQ(enc.encodeStreaming(h, tiny,
                                 [&coll](const uint8_t* d, size_t n) {
                                     coll.sink(d, n);
                                     return true;
                                 }),
             PacketError::kNone);
    // 与 encode() 对照
    Message msg;
    msg.type = static_cast<uint8_t>(MessageType::kFrameRect);
    msg.payload = ref;
    SequenceCounter seqA;
    MessageEncoder encA(seqA);
    std::vector<std::vector<uint8_t>> packetsA;
    CHECK_EQ(encA.encode(msg, packetsA), PacketError::kNone);
    CHECK_MSG(packetsEqual(packetsA, coll.packets), "tiny-read equivalence");
}

// ---- 测试 7：非法 TYPE ----
void streaming_invalid_type() {
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(4, [](size_t) { return 0; });
    MessageHeader bad{0x00, 0};  // 低于 kMinMessageType
    CHECK_EQ(enc.encodeStreaming(bad, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kInvalidType);
    CHECK_EQ(coll.packets.size(), 0u);
}

// ---- 测试 8：sink 中止 ----
void streaming_sink_abort() {
    const MessageHeader h{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PatternSource source(10000, [](size_t off) { return static_cast<uint8_t>(off); });
    size_t calls = 0;
    const PacketError err = enc.encodeStreaming(
        h, source, [&calls](const uint8_t*, size_t) {
            ++calls;
            return calls < 2;  // 第二包中止
        });
    CHECK_EQ(err, PacketError::kSinkAborted);
    CHECK_EQ(calls, 2u);
}

// ---- 测试 9：153600B 单 RECT 全帧 round-trip（Streaming → Decoder → Assembler）----
void streaming_rect_roundtrip() {
    constexpr uint16_t kFrameId = 42;
    constexpr uint16_t kWidth = 320;
    constexpr uint16_t kHeight = 240;
    constexpr uint32_t kByteCount = 153600;

    FrameCollector fc;
    FrameAssembler::Callbacks acb;
    acb.onBegin = [&fc](const FrameBeginInfo& b) { fc.onBegin(b); };
    acb.onRect = [&fc](const RectInfo& r, const uint8_t* p, size_t n) { fc.onRect(r, p, n); };
    acb.onCommit = [&fc](const CommittedFrame& f) { fc.commits.push_back(f); };
    acb.onDiscard = [&fc](FrameDiscardReason r) { fc.discards.push_back(r); };
    FrameAssembler assembler(std::move(acb));
    StreamDecoder decoder(
        [&assembler](const Message& m) { assembler.onMessage(m); }, nullptr,
        [&fc](DecoderError e) { fc.onStreamError(e); });

    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> wire;

    auto push = [&](const std::vector<std::vector<uint8_t>>& pkts) {
        for (const auto& p : pkts) {
            wire.push_back(p);
        }
    };

    // BEGIN
    auto begin = makeFrameBegin(kFrameId, FrameType::kFull, PixelFormat::kRgb565, kWidth,
                                kHeight, kByteCount);
    CHECK(begin.has_value());
    std::vector<std::vector<uint8_t>> beginPkts;
    CHECK_EQ(enc.encode(*begin, beginPkts), PacketError::kNone);
    push(beginPkts);

    // RECT（streaming，payload 不驻留内存）
    const MessageHeader rectHeader{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    PatternSource rectSource(
        8u + static_cast<size_t>(kByteCount),
        [kFrameId, kWidth, kHeight](size_t off) {
            return rectByteAt(kFrameId, kWidth, kHeight, off);
        });
    PacketCollector rectColl;
    CHECK_EQ(enc.encodeStreaming(rectHeader, rectSource,
                                 [&rectColl](const uint8_t* d, size_t n) {
                                     rectColl.sink(d, n);
                                     return true;
                                 }),
             PacketError::kNone);
    push(rectColl.packets);

    // END
    auto end = makeFrameEnd(kFrameId, 1, kByteCount, false);
    std::vector<std::vector<uint8_t>> endPkts;
    CHECK_EQ(enc.encode(end, endPkts), PacketError::kNone);
    push(endPkts);

    // 全部字节喂给 decoder
    for (const auto& p : wire) {
        decoder.feed(p.data(), p.size());
    }

    CHECK_EQ(fc.commits.size(), 1u);
    if (fc.commits.empty()) {
        return;
    }
    const CommittedFrame& f = fc.commits[0];
    CHECK_EQ(f.frameId, kFrameId);
    CHECK_EQ(f.frameType, FrameType::kFull);
    CHECK_EQ(f.width, kWidth);
    CHECK_EQ(f.height, kHeight);
    CHECK_EQ(f.rectCount, 1u);
    CHECK_EQ(f.byteCount, kByteCount);
    CHECK_EQ(fc.discards.size(), 0u);
    CHECK_EQ(fc.decoderErrors.size(), 0u);
    CHECK_EQ(fc.rects.size(), 1u);
    if (fc.rects.empty()) {
        return;
    }
    CHECK_EQ(fc.rects[0].x, 0u);
    CHECK_EQ(fc.rects[0].y, 0u);
    CHECK_EQ(fc.rects[0].w, kWidth);
    CHECK_EQ(fc.rects[0].h, kHeight);
    // 逐字节校验 153600B
    CHECK_EQ(fc.pixels.size(), 1u);
    if (fc.pixels.empty()) {
        return;
    }
    const auto& px = fc.pixels[0];
    CHECK_EQ(px.size(), static_cast<size_t>(kByteCount));
    for (size_t off = 0; off + 1 < px.size(); off += 2) {
        const size_t pxIdx = off / 2;
        const uint16_t x = static_cast<uint16_t>(pxIdx % kWidth);
        const uint16_t y = static_cast<uint16_t>(pxIdx / kWidth);
        const uint8_t lo = static_cast<uint8_t>(kFrameId + x);
        const uint8_t hi = static_cast<uint8_t>(kFrameId + y + 1u);
        if (px[off] != lo || px[off + 1] != hi) {
            CHECK_MSG(false, "pixel mismatch at byte " + std::to_string(off));
            return;
        }
    }
}

// ---- 测试 10：ProtocolEndpoint::sendMessageStreaming 双端跨端提交 ----
struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct EndpointSide {
    FakeClock clock;
    std::vector<uint8_t> rx;
    std::vector<SessionState> states;
    std::vector<CommittedFrame> commits;
    std::vector<RectInfo> rects;
    std::vector<std::vector<uint8_t>> rectPixels;
    std::unique_ptr<ProtocolEndpoint> ep;
    EndpointSide* peer = nullptr;

    void init(EndpointSide* peerSide) {
        peer = peerSide;
        ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](SessionState s) { states.push_back(s); };
        cb.onFrameBegin = [](const FrameBeginInfo&) {};
        cb.onFrameRect = [this](const RectInfo& r, const uint8_t* p, size_t n) {
            rects.push_back(r);
            rectPixels.emplace_back(p, p + n);
        };
        cb.onFrameCommit = [this](const CommittedFrame& f) { commits.push_back(f); };
        cb.onFrameDiscard = [](FrameDiscardReason) {};
        auto sink = [this](const uint8_t* d, size_t n) {
            peer->rx.insert(peer->rx.end(), d, d + n);
            return SendStatus::kOk;
        };
        ep = std::make_unique<ProtocolEndpoint>(EndpointConfig{}, sink, cb,
                                                [this]() { return clock.now; });
    }
    void pump() {
        std::vector<uint8_t> data = std::move(rx);
        rx.clear();
        if (!data.empty()) {
            ep->onTransportData(data.data(), data.size());
        }
        ep->tick();
    }
};

void streaming_endpoint_send() {
    EndpointSide a, b;
    a.init(&b);
    b.init(&a);
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    a.pump();
    b.pump();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);

    constexpr uint16_t kFrameId = 7;
    constexpr uint16_t kWidth = 320;
    constexpr uint16_t kHeight = 240;
    constexpr uint32_t kByteCount = 153600;

    auto begin = makeFrameBegin(kFrameId, FrameType::kFull, PixelFormat::kRgb565, kWidth,
                                kHeight, kByteCount);
    CHECK(begin.has_value());
    CHECK_EQ(a.ep->sendMessage(*begin), SendResult::kOk);
    a.pump();
    b.pump();

    const MessageHeader rectHeader{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    PatternSource rectSource(
        8u + static_cast<size_t>(kByteCount),
        [kFrameId, kWidth, kHeight](size_t off) {
            return rectByteAt(kFrameId, kWidth, kHeight, off);
        });
    CHECK_EQ(a.ep->sendMessageStreaming(rectHeader, rectSource), SendResult::kOk);
    a.pump();
    b.pump();

    CHECK_EQ(a.ep->sendMessage(makeFrameEnd(kFrameId, 1, kByteCount, false)), SendResult::kOk);
    a.pump();
    b.pump();

    CHECK_EQ(b.commits.size(), 1u);
    if (b.commits.empty()) {
        return;
    }
    CHECK_EQ(b.commits[0].frameId, kFrameId);
    CHECK_EQ(b.commits[0].rectCount, 1u);
    CHECK_EQ(b.commits[0].byteCount, kByteCount);
    CHECK_EQ(b.rectPixels.size(), 1u);
    if (b.rectPixels.empty()) {
        return;
    }
    CHECK_EQ(b.rectPixels[0].size(), static_cast<size_t>(kByteCount));

    // ACK_REQ 必须被拒绝（流式路径仅限数据消息）
    MessageHeader bad{static_cast<uint8_t>(MessageType::kFrameRect), kFlagAckReq};
    PatternSource src(4, [](size_t) { return 0; });
    CHECK_EQ(a.ep->sendMessageStreaming(bad, src), SendResult::kInvalidMessage);
}

// ---- M8-A1：1 MiB 逻辑载荷上限 + 零堆分配热路径 ----

// 恰好 1 MiB：256 个满包，kNone，与 encode() 对同一载荷逐位一致。
void streaming_megabyte_allowed() {
    constexpr size_t kMiB = kMaxMessagePayload;
    const std::vector<uint8_t> ref = makeReferencePayload(kMiB);
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(kMiB, [&ref](size_t off) { return ref[off]; });
    CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kNone);
    CHECK_EQ(coll.packets.size(), 256u);
    // 与 encode() 对照（测试侧持有 1 MiB 参考向量；生产路径零堆分配见 alloc 测试）。
    Message msg;
    msg.type = static_cast<uint8_t>(MessageType::kFrameRect);
    msg.payload = ref;
    SequenceCounter seqA;
    MessageEncoder encA(seqA);
    std::vector<std::vector<uint8_t>> packetsA;
    CHECK_EQ(encA.encode(msg, packetsA), PacketError::kNone);
    CHECK_MSG(packetsEqual(packetsA, coll.packets), "1 MiB streaming == encode()");
    // CHUNKED 规则 + SEQ 连续性（前 255 个 CHUNKED=1，末包 CHUNKED=0）。
    std::vector<DecodedPacket> dps;
    CHECK(decodeAll(coll.packets, dps));
    for (size_t i = 0; i < dps.size(); ++i) {
        const bool last = (i + 1 == dps.size());
        CHECK_EQ(static_cast<bool>(dps[i].header.flags & kFlagChunked), !last);
        CHECK_EQ(dps[i].header.seq, static_cast<uint16_t>(i));
        CHECK_EQ(dps[i].header.type, static_cast<uint8_t>(MessageType::kFrameRect));
    }
}

// 1 MiB − 1（1048575 B）：255 个满包 + 4095B 尾包 = 256 个包，kNone，
// 与 encode() 对同一载荷逐位一致。
void streaming_megabyte_minus_one_allowed() {
    constexpr size_t kMiB = kMaxMessagePayload;
    const std::vector<uint8_t> ref = makeReferencePayload(kMiB - 1);
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(kMiB - 1, [&ref](size_t off) { return ref[off]; });
    CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                 coll.sink(d, n);
                 return true;
             }),
             PacketError::kNone);
    CHECK_EQ(coll.packets.size(), 256u);  // 255×4096 + 4095 = 1048575
    Message msg;
    msg.type = static_cast<uint8_t>(MessageType::kFrameRect);
    msg.payload = ref;
    SequenceCounter seqA;
    MessageEncoder encA(seqA);
    std::vector<std::vector<uint8_t>> packetsA;
    CHECK_EQ(encA.encode(msg, packetsA), PacketError::kNone);
    CHECK_MSG(packetsEqual(packetsA, coll.packets), "1 MiB-1 streaming == encode()");
    // CHUNKED 规则 + 末包 LEN=4095。
    std::vector<DecodedPacket> dps;
    CHECK(decodeAll(coll.packets, dps));
    size_t total = 0;
    for (size_t i = 0; i < dps.size(); ++i) {
        const bool last = (i + 1 == dps.size());
        CHECK_EQ(static_cast<bool>(dps[i].header.flags & kFlagChunked), !last);
        total += dps[i].payload.size();
    }
    CHECK_EQ(total, kMiB - 1);
}

// 跨消息 staging 复用：同一 MessageEncoder 依次编码 1B/4096B/153608B/1B，
// 每次输出都与全新 Encoder 对同一载荷的 encode() 逐位一致（证明旧消息的
// 缓冲残留不会泄漏到下一消息）。
void streaming_staging_reuse_cross_messages() {
    const size_t sizes[] = {1, 4096, 153608, 1};
    // SEQ 是跨消息连续流（握手间不重置）：复用 encoder 与参考 encoder 必须
    // 共享同一起点并按相同步长消耗 SEQ，逐包对照才能字节一致。
    SequenceCounter seq, refSeq;
    MessageEncoder enc(seq), refEnc(refSeq);
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    for (const size_t len : sizes) {
        const std::vector<uint8_t> ref = makeReferencePayload(len);
        PacketCollector coll;
        PatternSource source(len, [&ref](size_t off) { return ref[off]; });
        CHECK_EQ(enc.encodeStreaming(header, source, [&coll](const uint8_t* d, size_t n) {
                     coll.sink(d, n);
                     return true;
                 }),
                 PacketError::kNone);
        Message msg;
        msg.type = static_cast<uint8_t>(MessageType::kFrameRect);
        msg.payload = ref;
        std::vector<std::vector<uint8_t>> refPackets;
        CHECK_EQ(refEnc.encode(msg, refPackets), PacketError::kNone);
        CHECK_MSG(packetsEqual(refPackets, coll.packets),
                  "staging reuse len " + std::to_string(len) + " == fresh encode()");
    }
}

// 1 MiB + 1：发出 256 个满包（1 MiB prefix）后返回 kMessageTooLarge，
// 无第 257 个包；prefix 与 encode() 对 1 MiB 载荷的输出逐位一致。
void streaming_megabyte_plus_one_rejected() {
    constexpr size_t kMiB = kMaxMessagePayload;
    const std::vector<uint8_t> ref = makeReferencePayload(kMiB + 1);
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    PatternSource source(kMiB + 1, [&ref](size_t off) { return ref[off]; });
    const PacketError err = enc.encodeStreaming(
        header, source, [&coll](const uint8_t* d, size_t n) {
            coll.sink(d, n);
            return true;
        });
    CHECK_EQ(err, PacketError::kMessageTooLarge);
    CHECK_EQ(coll.packets.size(), 256u);  // 恰好 256 个满包，无部分第 257 包
    Message msg;
    msg.type = static_cast<uint8_t>(MessageType::kFrameRect);
    msg.payload.assign(ref.begin(), ref.begin() + kMiB);
    SequenceCounter seqA;
    MessageEncoder encA(seqA);
    std::vector<std::vector<uint8_t>> packetsA;
    CHECK_EQ(encA.encode(msg, packetsA), PacketError::kNone);
    // 前 255 个包与 encode() 逐位一致；第 256 包载荷相同但 CHUNKED=1
    // （流被截断：后随数据未发出，末包必须保持 CHUNKED=1，不能伪装成消息末尾）。
    for (size_t i = 0; i + 1 < coll.packets.size(); ++i) {
        CHECK_MSG(packetsA[i] == coll.packets[i],
                  "prefix packet " + std::to_string(i) + " == encode()");
    }
    {
        std::vector<DecodedPacket> dps;
        CHECK(decodeAll(coll.packets, dps));
        size_t total = 0;
        for (const auto& dp : dps) {
            CHECK_EQ(static_cast<bool>(dp.header.flags & kFlagChunked), true);  // 全部非末包
            total += dp.payload.size();
        }
        CHECK_EQ(total, kMiB);
    }
}

// 无限 source：256 个满包后以 kMessageTooLarge 终止（不无限循环、不溢出）。
void streaming_infinite_source_terminates() {
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PacketCollector coll;
    struct InfiniteSource : public IMessagePayloadSource {
        size_t read(uint8_t* dst, size_t maxBytes) override {
            for (size_t i = 0; i < maxBytes; ++i) {
                dst[i] = static_cast<uint8_t>(i * 7u + 1u);
            }
            return maxBytes;  // 永不 EOF
        }
    } infinite;
    const PacketError err = enc.encodeStreaming(
        header, infinite, [&coll](const uint8_t* d, size_t n) {
            coll.sink(d, n);
            return true;
        });
    CHECK_EQ(err, PacketError::kMessageTooLarge);
    CHECK_EQ(coll.packets.size(), 256u);
}

// 单次 encodeStreaming 热路径零堆分配（counting_allocator DELTA）。
// 二进制还链接 display/transport/oled 等 TU，故只断言单次调用前后的增量。
void streaming_zero_alloc_hot_path() {
    const MessageHeader header{static_cast<uint8_t>(MessageType::kFrameRect), 0};
    SequenceCounter seq;
    MessageEncoder enc(seq);
    PatternSource source(153608, [](size_t off) { return static_cast<uint8_t>(off * 3u); });
    espview::proto::test::resetAllocationCounters();
    const PacketError err = enc.encodeStreaming(
        header, source, [](const uint8_t*, size_t) { return true; });
    CHECK_EQ(err, PacketError::kNone);
    CHECK_EQ(espview::proto::test::AllocationCounters::allocations.load(
                 std::memory_order_relaxed),
             0u);
    CHECK_EQ(espview::proto::test::AllocationCounters::bytes.load(std::memory_order_relaxed),
             0u);
}
}  // namespace

void runStreamingEncoderTests() {
    std::printf("  streaming_empty_payload\n");
    streaming_empty_payload();
    std::printf("  streaming_small_payload\n");
    streaming_small_payload();
    std::printf("  streaming_boundary_sizes\n");
    streaming_boundary_sizes();
    std::printf("  streaming_large_chunked\n");
    streaming_large_chunked();
    std::printf("  streaming_byte_equivalence\n");
    for (const size_t len : {size_t(0), size_t(1), size_t(4095), size_t(4096), size_t(4097),
                             size_t(10000), size_t(153608)}) {
        checkEquivalence(len);
    }
    std::printf("  streaming_seq_rollover\n");
    streaming_seq_rollover();
    std::printf("  streaming_partial_source_reads\n");
    streaming_partial_source_reads();
    std::printf("  streaming_invalid_type\n");
    streaming_invalid_type();
    std::printf("  streaming_sink_abort\n");
    streaming_sink_abort();
    std::printf("  streaming_rect_roundtrip\n");
    streaming_rect_roundtrip();
    std::printf("  streaming_endpoint_send\n");
    streaming_endpoint_send();
    std::printf("  streaming_megabyte_allowed\n");
    streaming_megabyte_allowed();
    std::printf("  streaming_megabyte_minus_one_allowed\n");
    streaming_megabyte_minus_one_allowed();
    std::printf("  streaming_staging_reuse_cross_messages\n");
    streaming_staging_reuse_cross_messages();
    std::printf("  streaming_megabyte_plus_one_rejected\n");
    streaming_megabyte_plus_one_rejected();
    std::printf("  streaming_infinite_source_terminates\n");
    streaming_infinite_source_terminates();
    std::printf("  streaming_zero_alloc_hot_path\n");
    streaming_zero_alloc_hot_path();
}
