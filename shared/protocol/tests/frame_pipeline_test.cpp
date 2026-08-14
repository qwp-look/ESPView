// ESPView — M1-3A In-Memory 双端 Frame Pipeline（host-side 集成测试）
//
// 规范来源：docs/DESIGN.md E 节（三层概念 / 帧消息 Payload Layout / PARTIAL 提交语义 /
// 字节流解码状态机 / 帧级错误处理 / 连接状态机）。
// 目标：用真实 Encoder / StreamDecoder / FrameAssembler / ProtocolEndpoint 组件，
// 通过 InMemoryBytePipe（模拟真实字节流断裂/粘包/随机切割）验证
//   TestPattern/Application → MessageEncoder → BytePipe → StreamDecoder → FrameAssembler → Commit
// 覆盖（对应 M1-3A 任务书第五~十六节）：
//   - FULL 320x240 多 RECT（逐字节 payload 校验，确定性 pattern f(frameId,rectId,x,y)）；
//   - 320x240 单大 RECT（153600 B，自然触发 38 包 CHUNKED）与 CHUNKED 重建；
//   - 固定 chunk（1/2/3/7/20/4096）、完整 Packet、多个 Packet（粘包）、
//     100 组固定种子随机分块、1-byte feed；
//   - 三帧粘包、CRC 损坏恢复、SEQ 跳变恢复；
//   - PARTIAL 无基准 / PARTIAL 跟随 FULL / 基准链；
//   - 控制消息穿插（消息之间，允许；不得进入 CHUNKED 消息内部）；
//   - FULL resync（reset 后新 FULL 不残留半帧）；
//   - ProtocolEndpoint 集成：HELLO 握手后 FULL frame 最终 commit。
// 测试数据全部由真实 MessageEncoder 生成；仅测试侧持有 vector 像素（TEST ONLY，
// 不代表生产内存模型）。纯 C++17，零平台依赖。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "in_memory_byte_pipe.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::CommittedFrame;
using espview::proto::DecoderError;
using espview::proto::EndpointConfig;
using espview::proto::FrameAssembler;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameState;
using espview::proto::FrameType;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeFrameRect;
using espview::proto::makePing;
using espview::proto::Message;
using espview::proto::MessageEncoder;
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
using espview::proto::test::feedChunks;
using espview::proto::test::feedRandomChunks;
using espview::proto::test::feedWholePackets;
using espview::proto::test::InMemoryBytePipe;

// ---- 确定性像素 pattern：f(frameId, rectId, x, y) → RGB565（LE 字节对）----
uint16_t pixelValue(uint16_t frameId, uint16_t rectId, uint16_t x, uint16_t y) {
    const uint8_t lo = static_cast<uint8_t>(frameId + rectId + x);
    const uint8_t hi = static_cast<uint8_t>(frameId + y + 1u);
    return static_cast<uint16_t>(lo) | (static_cast<uint16_t>(hi) << 8);
}

void putPixelLE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

// 生成 rect (w x h) 的 RGB565 LE 像素字节（pattern 使用 rect 内相对坐标）。
std::vector<uint8_t> makePixels(uint16_t frameId, uint16_t rectId, uint16_t w, uint16_t h) {
    const size_t n = static_cast<size_t>(w) * h * 2u;
    std::vector<uint8_t> px;
    px.reserve(n);
    for (uint16_t yy = 0; yy < h; ++yy) {
        for (uint16_t xx = 0; xx < w; ++xx) {
            putPixelLE(px, pixelValue(frameId, rectId, xx, yy));
        }
    }
    return px;
}

struct RectSpec {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
};

// 一帧的引用数据：消息序列 + 期望提交结果。
struct TestFrame {
    uint16_t frameId = 0;
    FrameType type = FrameType::kFull;
    uint16_t width = 320;
    uint16_t height = 240;
    std::vector<Message> messages;  // BEGIN + RECTs + END（按序）
    std::vector<RectSpec> rects;    // 与 RECT 消息一一对应
    std::vector<std::vector<uint8_t>> expectedPixels;
    uint16_t rectCount = 0;
    uint32_t byteCount = 0;
};

TestFrame buildFrame(uint16_t frameId, FrameType type, const std::vector<RectSpec>& specs) {
    TestFrame tf;
    tf.frameId = frameId;
    tf.type = type;
    tf.rects = specs;
    auto begin = makeFrameBegin(frameId, type, PixelFormat::kRgb565, tf.width, tf.height, 0);
    CHECK(begin.has_value());
    tf.messages.push_back(*begin);
    uint16_t rectId = 0;
    for (const auto& r : specs) {
        std::vector<uint8_t> px = makePixels(frameId, rectId, r.w, r.h);
        tf.expectedPixels.push_back(px);
        auto rect = makeFrameRect(r.x, r.y, r.w, r.h, px.data(), px.size());
        CHECK(rect.has_value());
        tf.messages.push_back(*rect);
        tf.byteCount += static_cast<uint32_t>(px.size());
        ++rectId;
    }
    tf.rectCount = static_cast<uint16_t>(specs.size());
    tf.messages.push_back(makeFrameEnd(frameId, tf.rectCount, tf.byteCount, false));
    return tf;
}

// ---- 回调收集 ----
struct Collector {
    std::vector<FrameBeginInfo> begins;
    std::vector<RectInfo> rects;
    std::vector<std::vector<uint8_t>> rectPixels;
    std::vector<CommittedFrame> commits;
    std::vector<FrameDiscardReason> discards;
    std::vector<DecoderError> decoderErrors;

    // 每帧提交时的 rect 区间（[start, end)，对应 commits[] 同下标）。
    // start = 最近一次 FRAME_BEGIN 时已收到的 rect 数；end = 提交时 rect 数。
    struct RectRange {
        size_t start = 0;
        size_t end = 0;
    };
    std::vector<RectRange> commitRanges;
    size_t currentRectStart = 0;  // 最近一次 onBegin 时的 rect 计数

    void onBegin(const FrameBeginInfo& b) {
        begins.push_back(b);
        currentRectStart = rects.size();
    }
    void onRect(const RectInfo& r, const uint8_t* p, size_t n) {
        rects.push_back(r);
        rectPixels.emplace_back(p, p + n);
    }
    void onCommit(const CommittedFrame& f) {
        commits.push_back(f);
        commitRanges.push_back({currentRectStart, rects.size()});
    }
    void onDiscard(FrameDiscardReason r) { discards.push_back(r); }
};

FrameAssembler makeAssembler(Collector& c) {
    FrameAssembler::Callbacks cb;
    cb.onBegin = [&c](const FrameBeginInfo& b) { c.onBegin(b); };
    cb.onRect = [&c](const RectInfo& r, const uint8_t* p, size_t n) { c.onRect(r, p, n); };
    cb.onCommit = [&c](const CommittedFrame& f) { c.onCommit(f); };
    cb.onDiscard = [&c](FrameDiscardReason r) { c.onDiscard(r); };
    return FrameAssembler(std::move(cb));
}

bool hasDiscard(const Collector& c, FrameDiscardReason r) {
    return std::find(c.discards.begin(), c.discards.end(), r) != c.discards.end();
}

bool hasDecoderError(const Collector& c, DecoderError e) {
    return std::find(c.decoderErrors.begin(), c.decoderErrors.end(), e) != c.decoderErrors.end();
}

// ---- 管线：Encoder → Pipe → Decoder → FrameAssembler ----
struct FramePipeline {
    SequenceCounter seq;
    MessageEncoder enc{seq};
    Collector c;
    FrameAssembler assembler;
    StreamDecoder decoder;
    InMemoryBytePipe pipe;

    FramePipeline() : assembler(makeAssembler(c)), decoder(makeDecoderCallbacks()) {}

    void send(const Message& m) {
        std::vector<std::vector<uint8_t>> packets;
        const PacketError err = enc.encode(m, packets);
        CHECK_EQ(err, PacketError::kNone);
        for (const auto& p : packets) {
            pipe.write(p);
        }
    }

    void sendFrame(const TestFrame& tf) {
        for (const auto& m : tf.messages) {
            send(m);
        }
    }

    // 编码一条消息但不写入 pipe，返回全部 packet 字节（供损坏/跳过等操作）。
    std::vector<std::vector<uint8_t>> encodePackets(const Message& m) {
        std::vector<std::vector<uint8_t>> packets;
        CHECK_EQ(enc.encode(m, packets), PacketError::kNone);
        return packets;
    }

    void writePackets(const std::vector<std::vector<uint8_t>>& packets) {
        for (const auto& p : packets) {
            pipe.write(p);
        }
    }

    // ---- pump：把 pipe 中的字节按不同分块方式喂给 decoder，然后清空 pipe ----
    void pumpAll() {
        decoder.feed(pipe.bytes());
        pipe.clear();
    }

    void pumpChunks(const std::vector<size_t>& sizes) {
        feedChunks(decoder, pipe.bytes(), sizes);
        pipe.clear();
    }

    void pumpRandom(uint32_t seed, size_t minChunk, size_t maxChunk) {
        feedRandomChunks(decoder, pipe.bytes(), seed, minChunk, maxChunk);
        pipe.clear();
    }

    void pumpPackets(size_t group) {
        feedWholePackets(decoder, pipe.bytes(), group);
        pipe.clear();
    }

private:
    StreamDecoder makeDecoderCallbacks() {
        return StreamDecoder(
            [this](const Message& m) { assembler.onMessage(m); },
            nullptr,
            [this](DecoderError e) {
                c.decoderErrors.push_back(e);
                assembler.onStreamError(e);
            });
    }
};

// 校验第 commitIndex 次提交的帧与 tf 完全一致（含每个 rect 的元数据与像素）。
void checkCommitted(const Collector& c, const TestFrame& tf, size_t commitIndex) {
    CHECK(commitIndex < c.commits.size());
    if (commitIndex >= c.commits.size()) {
        return;
    }
    const CommittedFrame& f = c.commits[commitIndex];
    CHECK_EQ(f.frameId, tf.frameId);
    CHECK_EQ(f.frameType, tf.type);
    CHECK_EQ(f.pixelFormat, PixelFormat::kRgb565);
    CHECK_EQ(f.width, tf.width);
    CHECK_EQ(f.height, tf.height);
    CHECK_EQ(f.rectCount, tf.rectCount);
    CHECK_EQ(f.byteCount, tf.byteCount);
    CHECK(commitIndex < c.commitRanges.size());
    if (commitIndex >= c.commitRanges.size()) {
        return;
    }
    const size_t start = c.commitRanges[commitIndex].start;
    const size_t end = c.commitRanges[commitIndex].end;
    CHECK_EQ(end - start, tf.rects.size());
    CHECK_EQ(end - start, tf.expectedPixels.size());
    for (size_t i = 0; i < tf.expectedPixels.size() && start + i < c.rectPixels.size(); ++i) {
        CHECK_EQ(c.rects[start + i].x, tf.rects[i].x);
        CHECK_EQ(c.rects[start + i].y, tf.rects[i].y);
        CHECK_EQ(c.rects[start + i].w, tf.rects[i].w);
        CHECK_EQ(c.rects[start + i].h, tf.rects[i].h);
        CHECK_EQ(c.rectPixels[start + i].size(), tf.expectedPixels[i].size());
        if (c.rectPixels[start + i].size() == tf.expectedPixels[i].size()) {
            CHECK(std::memcmp(c.rectPixels[start + i].data(), tf.expectedPixels[i].data(),
                              tf.expectedPixels[i].size()) == 0);
        }
    }
}

// ---- ProtocolEndpoint 双端 harness（与 protocol_endpoint_test 同构的最小版）----
struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct EndpointSide {
    FakeClock clock;
    std::vector<uint8_t> rx;  // 对端发来、本端待消费
    std::vector<SessionState> states;
    std::vector<CommittedFrame> commits;
    std::vector<RectInfo> rects;
    std::vector<std::vector<uint8_t>> rectPixels;
    std::unique_ptr<ProtocolEndpoint> ep;
    EndpointSide* peer = nullptr;  // 本端 sink 投递目标

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

    // 把本端 rx 全部喂给 decoder（一次全量）。
    void pump() {
        std::vector<uint8_t> data = std::move(rx);
        rx.clear();
        if (!data.empty()) {
            ep->onTransportData(data.data(), data.size());
        }
    }
};

}  // namespace

// ============ 1. FULL 320x240 多 RECT（4 象限覆盖整屏）============
void full_frame_320x240_4rects() {
    const TestFrame tf = buildFrame(
        1, FrameType::kFull,
        {{0, 0, 160, 120}, {160, 0, 160, 120}, {0, 120, 160, 120}, {160, 120, 160, 120}});
    CHECK_EQ(tf.byteCount, 320u * 240u * 2u);

    FramePipeline pl;
    pl.sendFrame(tf);
    pl.pumpAll();

    CHECK_EQ(pl.c.commits.size(), 1u);
    checkCommitted(pl.c, tf, 0);
    CHECK_EQ(pl.c.begins.size(), 1u);
    CHECK_EQ(pl.c.begins[0].frameId, 1u);
    CHECK_EQ(pl.c.begins[0].frameType, FrameType::kFull);
    CHECK_EQ(pl.c.begins[0].width, 320u);
    CHECK_EQ(pl.c.begins[0].height, 240u);
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 2. 320x240 单大 RECT（153600 B → 38 包 CHUNKED）+ 重建 ============
void large_single_rect_320x240_chunked() {
    const TestFrame tf = buildFrame(2, FrameType::kFull, {{0, 0, 320, 240}});
    CHECK_EQ(tf.byteCount, 153600u);
    CHECK_EQ(tf.expectedPixels[0].size(), 153600u);

    // wire 层 CHUNKED 语义：全部包 TYPE=FRAME_RECT，前 n-1 包 CHUNKED=1，
    // 末包 CHUNKED=0，SEQ 连续递增。
    {
        FramePipeline pl;
        const std::vector<std::vector<uint8_t>> packets = pl.encodePackets(tf.messages[1]);
        CHECK_EQ(packets.size(), 38u);  // (8 + 153600) / 4096 向上取整
        PacketHeader h;
        uint16_t prevSeq = 0;
        for (size_t i = 0; i < packets.size(); ++i) {
            CHECK_EQ(decodeHeader(packets[i].data(), packets[i].size(), &h), PacketError::kNone);
            CHECK_EQ(h.type, static_cast<uint8_t>(MessageType::kFrameRect));
            CHECK_EQ(h.seq, i == 0 ? 0u : static_cast<uint16_t>(prevSeq + 1u));
            prevSeq = h.seq;
            const bool expectChunked = (i + 1 < packets.size());
            CHECK_EQ((h.flags & espview::proto::kFlagChunked) != 0, expectChunked);
            CHECK_EQ((h.flags & ~espview::proto::kFlagChunked), 0u);
        }
    }

    // 随机分块重建：1 个 RECT 消息 → 1 个 rect，payload 逐字节一致。
    FramePipeline pl;
    pl.sendFrame(tf);
    pl.pumpRandom(0x9E3779B9u, 1, 512);
    CHECK_EQ(pl.c.commits.size(), 1u);
    checkCommitted(pl.c, tf, 0);
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.rects[0].w, 320u);
    CHECK_EQ(pl.c.rects[0].h, 240u);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 3. 固定 chunk 尺寸（1/2/3/7/20/4096）============
void fixed_chunk_sizes() {
    const TestFrame tf = buildFrame(3, FrameType::kFull,
                                    {{0, 0, 32, 32}, {40, 10, 24, 16}, {100, 60, 48, 8}});

    // 编码一次，复用字节流（每次喂给全新 decoder/assembler）。
    FramePipeline base;
    base.sendFrame(tf);
    const std::vector<uint8_t> bytes = base.pipe.bytes();

    const std::vector<std::vector<size_t>> patterns = {
        {1}, {2}, {3}, {7}, {20}, {4096}, {3, 5, 7}, {20, 4096, 2},
    };
    for (const auto& pattern : patterns) {
        FramePipeline pl;
        feedChunks(pl.decoder, bytes, pattern);
        CHECK_EQ(pl.c.commits.size(), 1u);
        checkCommitted(pl.c, tf, 0);
        CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    }
}

// ============ 4. 完整 Packet / 多个 Packet（粘包）============
void whole_packet_and_group_feeds() {
    const TestFrame tf = buildFrame(4, FrameType::kFull,
                                    {{0, 0, 32, 32}, {40, 10, 24, 16}, {100, 60, 48, 8}});

    FramePipeline base;
    base.sendFrame(tf);
    const std::vector<uint8_t> bytes = base.pipe.bytes();

    // group=1：一个完整 Packet 一次 feed。
    {
        FramePipeline pl;
        feedWholePackets(pl.decoder, bytes, 1);
        CHECK_EQ(pl.c.commits.size(), 1u);
        checkCommitted(pl.c, tf, 0);
        CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    }
    // group=2/3：多个完整 Packet 一次 feed（粘包）。
    for (size_t group = 2; group <= 3; ++group) {
        FramePipeline pl;
        feedWholePackets(pl.decoder, bytes, group);
        CHECK_EQ(pl.c.commits.size(), 1u);
        checkCommitted(pl.c, tf, 0);
        CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    }
}

// ============ 5. 100 组固定种子随机分块（320x240 FULL）============
void random_chunk_feeds_100() {
    const TestFrame tf = buildFrame(
        5, FrameType::kFull,
        {{0, 0, 160, 120}, {160, 0, 160, 120}, {0, 120, 160, 120}, {160, 120, 160, 120}});

    FramePipeline base;
    base.sendFrame(tf);
    const std::vector<uint8_t> bytes = base.pipe.bytes();

    for (uint32_t seed = 0; seed < 100; ++seed) {
        const size_t minChunk = (seed % 4) + 1;              // 1..4
        const size_t maxChunk = 300 + (seed % 5) * 200;      // 300..1100
        FramePipeline pl;
        feedRandomChunks(pl.decoder, bytes, seed, minChunk, maxChunk);
        CHECK_EQ(pl.c.commits.size(), 1u);
        checkCommitted(pl.c, tf, 0);
        CHECK_EQ(pl.c.decoderErrors.size(), 0u);
        CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
    }
}

// ============ 6. 1-byte feed（整帧逐字节）============
void one_byte_feed_320x240() {
    const TestFrame tf = buildFrame(
        6, FrameType::kFull,
        {{0, 0, 160, 120}, {160, 0, 160, 120}, {0, 120, 160, 120}, {160, 120, 160, 120}});

    FramePipeline pl;
    pl.sendFrame(tf);
    pl.pumpChunks({1});

    CHECK_EQ(pl.c.commits.size(), 1u);
    checkCommitted(pl.c, tf, 0);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 7. 三帧粘包：一次 feed 全部字节 → 3 次独立 commit ============
void sticky_three_frames() {
    const std::vector<RectSpec> rects = {{0, 0, 16, 16}, {100, 50, 32, 16}};
    const TestFrame tf1 = buildFrame(1, FrameType::kFull, rects);
    const TestFrame tf2 = buildFrame(2, FrameType::kFull, rects);
    const TestFrame tf3 = buildFrame(3, FrameType::kFull, rects);

    FramePipeline pl;
    pl.sendFrame(tf1);
    pl.sendFrame(tf2);
    pl.sendFrame(tf3);
    pl.pumpAll();

    CHECK_EQ(pl.c.commits.size(), 3u);
    checkCommitted(pl.c, tf1, 0);
    checkCommitted(pl.c, tf2, 1);
    checkCommitted(pl.c, tf3, 2);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 8. CRC 损坏 → 当前帧作废 → 下一个 FULL 恢复 ============
void crc_corruption_recovers() {
    const TestFrame tf1 = buildFrame(1, FrameType::kFull, {{0, 0, 10, 10}});
    const TestFrame tf3 = buildFrame(3, FrameType::kFull, {{0, 0, 10, 10}});

    FramePipeline pl;
    pl.sendFrame(tf1);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 1u);

    // frame#2：BEGIN + RECT（翻转 RECT payload 末字节 → CRC 必失败）+ END
    {
        const TestFrame tf2 = buildFrame(2, FrameType::kFull, {{0, 0, 10, 10}});
        pl.send(tf2.messages[0]);  // BEGIN
        std::vector<std::vector<uint8_t>> rectPackets = pl.encodePackets(tf2.messages[1]);
        CHECK_EQ(rectPackets.size(), 1u);
        rectPackets[0].back() ^= 0xFF;  // 损坏 payload 末字节
        pl.writePackets(rectPackets);
        pl.send(tf2.messages[2]);  // END
    }

    pl.sendFrame(tf3);
    pl.pumpAll();

    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[1].frameId, 3u);
    checkCommitted(pl.c, tf3, 1);
    CHECK(hasDecoderError(pl.c, DecoderError::kCrcMismatch));
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kStreamError));
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 9. SEQ 跳变（丢 RECT 包）→ 帧作废 → 下一个 FULL 恢复 ============
void seq_gap_recovers() {
    const TestFrame tf1 = buildFrame(1, FrameType::kFull, {{0, 0, 10, 10}});
    const TestFrame tf3 = buildFrame(3, FrameType::kFull, {{0, 0, 10, 10}});

    FramePipeline pl;
    pl.sendFrame(tf1);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 1u);

    // frame#2：先依次编码 BEGIN/RECT/END（各消耗一个 SEQ），再只写 BEGIN 与 END、
    // 跳过 RECT → END 的 seq = BEGIN+2，decoder 期望 BEGIN+1 → seq gap。
    {
        const TestFrame tf2 = buildFrame(2, FrameType::kFull, {{0, 0, 10, 10}});
        const auto pktsBegin = pl.encodePackets(tf2.messages[0]);  // seq = N
        const auto pktsRect = pl.encodePackets(tf2.messages[1]);   // seq = N+1（故意不写）
        const auto pktsEnd = pl.encodePackets(tf2.messages[2]);    // seq = N+2
        pl.writePackets(pktsBegin);
        (void)pktsRect;  // RECT 包故意不写入
        pl.writePackets(pktsEnd);
    }

    pl.sendFrame(tf3);
    pl.pumpAll();

    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[1].frameId, 3u);
    checkCommitted(pl.c, tf3, 1);
    CHECK(hasDecoderError(pl.c, DecoderError::kSequenceGap));
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kStreamError));
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 10. PARTIAL 无基准不得提交；之后 FULL 建立基准 ============
void partial_without_base() {
    FramePipeline pl;  // 全新，无任何已提交基准
    const TestFrame p1 = buildFrame(1, FrameType::kPartial, {{0, 0, 16, 16}});
    pl.sendFrame(p1);
    pl.pumpAll();

    CHECK_EQ(pl.c.commits.size(), 0u);
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kPartialWithoutBase));
    // 丢弃后进入 DISCARD_FRAME：忽略非 BEGIN 消息，直到下一个合法 BEGIN 重同步
    // （DESIGN.md：继续等待 FULL frame 进行重同步）。
    CHECK_EQ(pl.assembler.state(), FrameState::kDiscardFrame);

    // 之后 FULL 可提交并建立基准。
    const TestFrame f2 = buildFrame(2, FrameType::kFull, {{0, 0, 16, 16}});
    pl.sendFrame(f2);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 2u);
    checkCommitted(pl.c, f2, 0);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
}

// ============ 11. FULL 提交后 PARTIAL 可提交；PARTIAL 提交后基准更新 ============
void partial_after_full_base_chain() {
    FramePipeline pl;
    const TestFrame full = buildFrame(100, FrameType::kFull, {{0, 0, 16, 16}});
    const TestFrame p1 = buildFrame(101, FrameType::kPartial, {{20, 20, 8, 8}});
    const TestFrame p2 = buildFrame(102, FrameType::kPartial, {{50, 50, 8, 8}});

    pl.sendFrame(full);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 1u);
    checkCommitted(pl.c, full, 0);

    pl.sendFrame(p1);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 2u);
    checkCommitted(pl.c, p1, 1);
    CHECK_EQ(pl.c.commits[1].frameType, FrameType::kPartial);

    // 基准已更新为 PARTIAL#101：再一个 PARTIAL 仍可提交。
    pl.sendFrame(p2);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 3u);
    checkCommitted(pl.c, p2, 2);
    CHECK_EQ(pl.c.commits[2].frameType, FrameType::kPartial);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.c.discards.size(), 0u);
}

// ============ 12. 控制消息穿插（消息之间，允许）============
void control_between_messages() {
    const TestFrame tf = buildFrame(9, FrameType::kFull, {{0, 0, 20, 20}, {40, 40, 30, 30}});

    FramePipeline pl;
    pl.send(tf.messages[0]);  // FRAME_BEGIN
    pl.send(tf.messages[1]);  // RECT#1
    pl.send(makePing(1234));  // PING：位于两个 RECT 消息之间（允许）
    pl.send(tf.messages[2]);  // RECT#2
    pl.send(tf.messages[3]);  // FRAME_END
    pl.pumpAll();

    CHECK_EQ(pl.c.commits.size(), 1u);
    checkCommitted(pl.c, tf, 0);
    CHECK_EQ(pl.c.commits[0].rectCount, 2u);
    CHECK_EQ(pl.c.commits[0].byteCount, tf.byteCount);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 13. FULL resync：reset 后新 FULL 不残留半帧 ============
void full_resync_after_reset() {
    const TestFrame tf1 = buildFrame(1, FrameType::kFull, {{0, 0, 10, 10}});
    const TestFrame tf2 = buildFrame(2, FrameType::kFull, {{0, 0, 10, 10}});
    const TestFrame tf4 = buildFrame(4, FrameType::kFull, {{0, 0, 10, 10}});

    FramePipeline pl;
    pl.sendFrame(tf1);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 1u);

    // 断线重连语义：双方 seq 清零 + decoder/assembler reset。
    pl.seq.reset(0);
    pl.decoder.reset();
    pl.assembler.reset();
    pl.sendFrame(tf2);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[1].frameId, 2u);
    checkCommitted(pl.c, tf2, 1);
    CHECK_EQ(pl.c.discards.size(), 0u);  // reset 时 idle，无 discard

    // 收帧中途 reset：BEGIN 已派发（IN_FRAME）后 reset → 旧帧作废（kReset），不残留。
    pl.send(tf2.messages[0]);  // BEGIN（只发帧头）
    pl.pumpAll();
    CHECK_EQ(pl.assembler.state(), FrameState::kInFrame);
    pl.seq.reset(0);
    pl.decoder.reset();
    pl.assembler.reset();  // IN_FRAME → onDiscard(kReset)
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kReset));
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);

    // 新 FULL 正常提交。
    pl.seq.reset(0);
    pl.sendFrame(tf4);
    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 3u);
    CHECK_EQ(pl.c.commits[2].frameId, 4u);
    checkCommitted(pl.c, tf4, 2);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// ============ 14. ProtocolEndpoint 集成：HELLO 握手后 FULL frame 提交 ============
void endpoint_integrated_pipeline() {
    EndpointSide a, b;
    a.init(&b);
    b.init(&a);
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    a.pump();  // a 收 b 的 HELLO
    b.pump();  // b 收 a 的 HELLO
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(a.ep->stats().rxHello, 1u);
    CHECK_EQ(b.ep->stats().rxHello, 1u);

    // A（ESP32）经 ProtocolEndpoint 发送 FULL 帧：一个大 RECT（CHUNKED）+ 一个小 RECT。
    const TestFrame tf = buildFrame(5, FrameType::kFull, {{0, 0, 200, 120}, {200, 120, 64, 32}});
    for (const auto& m : tf.messages) {
        CHECK_EQ(a.ep->sendMessage(m), SendResult::kOk);
    }
    b.pump();  // b 消费 A 的帧字节

    CHECK_EQ(b.commits.size(), 1u);
    if (b.commits.size() == 1u) {
        const CommittedFrame& f = b.commits[0];
        CHECK_EQ(f.frameId, 5u);
        CHECK_EQ(f.frameType, FrameType::kFull);
        CHECK_EQ(f.pixelFormat, PixelFormat::kRgb565);
        CHECK_EQ(f.width, 320u);
        CHECK_EQ(f.height, 240u);
        CHECK_EQ(f.rectCount, 2u);
        CHECK_EQ(f.byteCount, tf.byteCount);
        CHECK_EQ(b.rects.size(), 2u);
        CHECK_EQ(b.rectPixels.size(), tf.expectedPixels.size());
        for (size_t i = 0; i < tf.expectedPixels.size() && i < b.rectPixels.size(); ++i) {
            CHECK_EQ(b.rects[i].x, tf.rects[i].x);
            CHECK_EQ(b.rects[i].y, tf.rects[i].y);
            CHECK_EQ(b.rects[i].w, tf.rects[i].w);
            CHECK_EQ(b.rects[i].h, tf.rects[i].h);
            CHECK_EQ(b.rectPixels[i].size(), tf.expectedPixels[i].size());
            if (b.rectPixels[i].size() == tf.expectedPixels[i].size()) {
                CHECK(std::memcmp(b.rectPixels[i].data(), tf.expectedPixels[i].data(),
                                  tf.expectedPixels[i].size()) == 0);
            }
        }
    }

    // 会话保持 CONNECTED，未被帧流量干扰。
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->stats().decoderErrors, 0u);
    CHECK_EQ(a.ep->stats().decoderErrors, 0u);
}

void runFramePipelineTests() {
    std::printf("  full_frame_320x240_4rects\n");
    full_frame_320x240_4rects();
    std::printf("  large_single_rect_320x240_chunked\n");
    large_single_rect_320x240_chunked();
    std::printf("  fixed_chunk_sizes\n");
    fixed_chunk_sizes();
    std::printf("  whole_packet_and_group_feeds\n");
    whole_packet_and_group_feeds();
    std::printf("  random_chunk_feeds_100\n");
    random_chunk_feeds_100();
    std::printf("  one_byte_feed_320x240\n");
    one_byte_feed_320x240();
    std::printf("  sticky_three_frames\n");
    sticky_three_frames();
    std::printf("  crc_corruption_recovers\n");
    crc_corruption_recovers();
    std::printf("  seq_gap_recovers\n");
    seq_gap_recovers();
    std::printf("  partial_without_base\n");
    partial_without_base();
    std::printf("  partial_after_full_base_chain\n");
    partial_after_full_base_chain();
    std::printf("  control_between_messages\n");
    control_between_messages();
    std::printf("  full_resync_after_reset\n");
    full_resync_after_reset();
    std::printf("  endpoint_integrated_pipeline\n");
    endpoint_integrated_pipeline();
}
