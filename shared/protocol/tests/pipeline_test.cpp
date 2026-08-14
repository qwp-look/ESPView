// ESPView — In-memory Pipeline Test（M0 收尾）
//
// 规范来源：docs/DESIGN.md E 节。
// 纯 host-side 集成测试：
//   MessageEncoder → InMemoryBytePipe → StreamDecoder → FrameAssembler
// 原则：
//   - 所有 packet 都由真实 MessageEncoder 生成，不手工构造 wire bytes；
//   - 故意损坏/丢包仅在已编码的真实包上操作（翻转字节 / 跳过）；
//   - InMemoryBytePipe 仅为测试辅助（字节流缓冲 + 随机分块回放），不是正式 Transport；
//   - 支持 1 byte、随机长度、完整 packet、多个 packet 的分块方式。
// 纯 C++17，零平台依赖。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "test_util.h"

namespace {

using espview::proto::CommittedFrame;
using espview::proto::DecoderError;
using espview::proto::FrameAssembler;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameState;
using espview::proto::FrameType;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeFrameRect;
using espview::proto::makeMessage;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PixelFormat;
using espview::proto::RectInfo;
using espview::proto::SequenceCounter;
using espview::proto::StreamDecoder;

// ---- 局部 LE 写入 helper（构造非法 FRAME_END 载荷用）----
void putU16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(static_cast<uint8_t>(val & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
}

void putU32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>(val & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
}

// 任意字段的 FRAME_END 载荷（可构造计数不匹配）。
Message rawEnd(uint16_t frameId, uint16_t rectCount, uint32_t byteCount, uint8_t flags) {
    std::vector<uint8_t> p;
    p.reserve(9);
    putU16(p, frameId);
    putU16(p, rectCount);
    putU32(p, byteCount);
    p.push_back(flags);
    return makeMessage(static_cast<uint8_t>(MessageType::kFrameEnd), 0, std::move(p));
}

// ---- 回调收集 ----
struct AssemblerCollector {
    std::vector<FrameBeginInfo> begins;
    std::vector<RectInfo> rects;
    std::vector<std::vector<uint8_t>> rectPixels;
    std::vector<CommittedFrame> commits;
    std::vector<FrameDiscardReason> discards;
    std::vector<DecoderError> decoderErrors;

    void onBegin(const FrameBeginInfo& b) { begins.push_back(b); }
    void onRect(const RectInfo& r, const uint8_t* pixels, size_t n) {
        rects.push_back(r);
        rectPixels.emplace_back(pixels, pixels + n);
    }
    void onCommit(const CommittedFrame& f) { commits.push_back(f); }
    void onDiscard(FrameDiscardReason r) { discards.push_back(r); }
};

FrameAssembler makeAssembler(AssemblerCollector& c) {
    FrameAssembler::Callbacks cb;
    cb.onBegin = [&c](const FrameBeginInfo& b) { c.onBegin(b); };
    cb.onRect = [&c](const RectInfo& r, const uint8_t* p, size_t n) { c.onRect(r, p, n); };
    cb.onCommit = [&c](const CommittedFrame& f) { c.onCommit(f); };
    cb.onDiscard = [&c](FrameDiscardReason r) { c.onDiscard(r); };
    return FrameAssembler(std::move(cb));
}

bool hasDiscard(const AssemblerCollector& c, FrameDiscardReason r) {
    return std::find(c.discards.begin(), c.discards.end(), r) != c.discards.end();
}

bool hasDecoderError(const AssemblerCollector& c, DecoderError e) {
    return std::find(c.decoderErrors.begin(), c.decoderErrors.end(), e) != c.decoderErrors.end();
}

// ---- In-memory 字节管道（测试辅助，非正式 Transport）----
class InMemoryBytePipe {
public:
    void write(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
    void write(const std::vector<uint8_t>& v) { write(v.data(), v.size()); }
    const std::vector<uint8_t>& bytes() const { return buf_; }
    void clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

// 分块配置：随机 chunk 大小范围 + 固定种子（可复现）。
struct FeedConfig {
    int minChunk;
    int maxChunk;
    uint32_t seed;
};

// 把字节流按随机 chunk 分块呵给 decoder（模拟字节流断裂/粘包随机切割）。
void feedInChunks(StreamDecoder& dec, const std::vector<uint8_t>& data, const FeedConfig& cfg) {
    std::mt19937 rng(cfg.seed);
    std::uniform_int_distribution<int> dist(cfg.minChunk, cfg.maxChunk);
    size_t pos = 0;
    while (pos < data.size()) {
        const size_t n = std::min(static_cast<size_t>(dist(rng)), data.size() - pos);
        dec.feed(data.data() + pos, n);
        pos += n;
    }
}

// ---- 集成管线：Encoder → Pipe → Decoder → FrameAssembler ----
struct Pipeline {
    SequenceCounter seq;
    AssemblerCollector c;
    FrameAssembler assembler;
    StreamDecoder decoder;
    InMemoryBytePipe pipe;

    Pipeline() : assembler(makeAssembler(c)), decoder(makeDecoderCallbacks()) {}

    // 编码一条 Message，全部 packet 字节依次写入 pipe。
    void encode(const Message& m) {
        MessageEncoder enc(seq);
        std::vector<std::vector<uint8_t>> packets;
        const PacketError err = enc.encode(m, packets);
        CHECK_EQ(err, PacketError::kNone);
        for (const auto& p : packets) {
            pipe.write(p);
        }
    }

    // 将 pipe 中的字节按配置随机分块呵入 decoder，然后清空 pipe。
    void pump(const FeedConfig& cfg) {
        feedInChunks(decoder, pipe.bytes(), cfg);
        pipe.clear();
    }

    // 将 pipe 中所有字节一次性全部呵入（完整 packet / 多个 packet 粘包）。
    void pumpAll() {
        decoder.feed(pipe.bytes());
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

// 标准 320x240 RGB565 帧消息。
Message beginMsg(uint16_t frameId, FrameType type) {
    return *makeFrameBegin(frameId, type, PixelFormat::kRgb565, 320, 240, 0);
}

Message rectMsg(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t fill = 0x11) {
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 2u, fill);
    auto m = makeFrameRect(x, y, w, h, pixels.data(), pixels.size());
    CHECK(m.has_value());
    return *m;
}

// 1. FULL 帧，1-byte chunks：断裂最极端场景
void full_frame_one_byte_chunks() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 16, 16, 0x11));
    pl.encode(makeFrameEnd(1, 1, 16u * 16u * 2u, false));
    pl.pump(FeedConfig{1, 1, 0xBEEF});

    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[0].frameType, FrameType::kFull);
    CHECK_EQ(pl.c.commits[0].pixelFormat, PixelFormat::kRgb565);
    CHECK_EQ(pl.c.commits[0].width, 320u);
    CHECK_EQ(pl.c.commits[0].height, 240u);
    CHECK_EQ(pl.c.commits[0].rectCount, 1u);
    CHECK_EQ(pl.c.commits[0].byteCount, 16u * 16u * 2u);
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.rectPixels[0].size(), 16u * 16u * 2u);
    CHECK(std::all_of(pl.c.rectPixels[0].begin(), pl.c.rectPixels[0].end(),
                      [](uint8_t b) { return b == 0x11; }));
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// 2. 完整 packet / 多个 packet 一次性呵入（粘包）
void full_frame_whole_packets() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 16, 16, 0x22));
    pl.encode(makeFrameEnd(1, 1, 16u * 16u * 2u, false));
    pl.pumpAll();

    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[0].byteCount, 16u * 16u * 2u);
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
}

// 3. FULL 帧，随机 chunk：模拟实际 UART 字节流切割
void full_frame_random_chunks() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 16, 16, 0x33));
    pl.encode(makeFrameEnd(1, 1, 16u * 16u * 2u, false));
    pl.pump(FeedConfig{1, 200, 0x1234});

    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.rectPixels[0].size(), 16u * 16u * 2u);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
}

// 4. 一帧多个 RECT：累计 rectCount / byteCount，所有 rect 像素保真
void multiple_rect() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 10, 10, 0x11));
    pl.encode(rectMsg(10, 0, 20, 20, 0x22));
    pl.encode(rectMsg(30, 30, 40, 40, 0x33));
    const uint32_t bytes = 10u * 10u * 2u + 20u * 20u * 2u + 40u * 40u * 2u;
    pl.encode(makeFrameEnd(1, 3, bytes, false));
    pl.pump(FeedConfig{1, 128, 0xABCD});

    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].rectCount, 3u);
    CHECK_EQ(pl.c.commits[0].byteCount, bytes);
    CHECK_EQ(pl.c.rects.size(), 3u);
    CHECK_EQ(pl.c.rectPixels.size(), 3u);
    CHECK(std::all_of(pl.c.rectPixels[0].begin(), pl.c.rectPixels[0].end(),
                      [](uint8_t b) { return b == 0x11; }));
    CHECK(std::all_of(pl.c.rectPixels[1].begin(), pl.c.rectPixels[1].end(),
                      [](uint8_t b) { return b == 0x22; }));
    CHECK(std::all_of(pl.c.rectPixels[2].begin(), pl.c.rectPixels[2].end(),
                      [](uint8_t b) { return b == 0x33; }));
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
}

// 5. CHUNKED RECT：payload > 4096 被 Encoder 拆为多包，经 pipe 后正确重组
void chunked_rect() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    std::vector<uint8_t> pixels(320u * 8u * 2u);  // 5120 像素字节，payload 5128 > 4096
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(i * 3);
    }
    auto big = makeFrameRect(0, 0, 320, 8, pixels.data(), pixels.size());
    CHECK(big.has_value());
    pl.encode(*big);
    pl.encode(makeFrameEnd(1, 1, static_cast<uint32_t>(pixels.size()), false));
    pl.pump(FeedConfig{1, 300, 0x99});

    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].byteCount, static_cast<uint32_t>(pixels.size()));
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.rects[0].w, 320u);
    CHECK_EQ(pl.c.rects[0].h, 8u);
    CHECK_EQ(pl.c.rectPixels[0].size(), pixels.size());
    CHECK(std::memcmp(pl.c.rectPixels[0].data(), pixels.data(), pixels.size()) == 0);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
}

// 6. FULL 建立基准后 PARTIAL 可提交（对应 DESIGN.md PARTIAL 语义）
void partial_after_full() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 10, 10, 0x11));
    pl.encode(makeFrameEnd(1, 1, 200, false));
    pl.encode(beginMsg(2, FrameType::kPartial));
    pl.encode(rectMsg(8, 16, 32, 24, 0x44));
    pl.encode(makeFrameEnd(2, 1, 32u * 24u * 2u, false));
    pl.pump(FeedConfig{1, 64, 0x55});

    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[1].frameId, 2u);
    CHECK_EQ(pl.c.commits[1].frameType, FrameType::kPartial);
    CHECK_EQ(pl.c.commits[1].byteCount, 32u * 24u * 2u);
    CHECK_EQ(pl.c.discards.size(), 0u);
}

// 7. CRC 错误：翻转真实编码包的像素字节 → 包丢弃、当前帧作废，下一个 FULL 恢复
void crc_error_recovers() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 10, 10, 0x11));
    pl.encode(makeFrameEnd(1, 1, 200, false));

    // 第二帧：BEGIN 正常，RECT 像素区翻转一字节，END 正常
    {
        MessageEncoder enc(pl.seq);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(beginMsg(2, FrameType::kFull), pkts), PacketError::kNone);
        CHECK_EQ(pkts.size(), 1u);
        pl.pipe.write(pkts[0]);

        CHECK_EQ(enc.encode(rectMsg(0, 0, 10, 10, 0x22), pkts), PacketError::kNone);
        CHECK_EQ(pkts.size(), 1u);
        std::vector<uint8_t> bad = pkts[0];
        bad[28] ^= 0xFFu;  // 20B header 之后的像素区第一字节
        pl.pipe.write(bad);

        CHECK_EQ(enc.encode(makeFrameEnd(2, 1, 200, false), pkts), PacketError::kNone);
        CHECK_EQ(pkts.size(), 1u);
        pl.pipe.write(pkts[0]);
    }

    // 第三帧：正常 FULL → 重同步恢复
    pl.encode(beginMsg(3, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 10, 10, 0x33));
    pl.encode(makeFrameEnd(3, 1, 200, false));

    pl.pump(FeedConfig{1, 100, 0x77});
    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[1].frameId, 3u);
    CHECK(hasDecoderError(pl.c, DecoderError::kCrcMismatch));
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kStreamError));
}

// 8. SEQ 跳变：丢一个 RECT 包 → 当前帧作废，下一个 FULL 恢复
void seq_gap_recovers() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 10, 10, 0x11));
    pl.encode(makeFrameEnd(1, 1, 200, false));

    // 第二帧：故意丢掉 RECT 包（模拟丢包）
    {
        MessageEncoder enc(pl.seq);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(beginMsg(2, FrameType::kFull), pkts), PacketError::kNone);
        pl.pipe.write(pkts[0]);
        CHECK_EQ(enc.encode(rectMsg(0, 0, 10, 10, 0x22), pkts), PacketError::kNone);  // 跳过不写
        CHECK_EQ(enc.encode(makeFrameEnd(2, 1, 200, false), pkts), PacketError::kNone);
        pl.pipe.write(pkts[0]);
    }

    // 第三帧：正常 FULL → 恢复
    pl.encode(beginMsg(3, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 10, 10, 0x33));
    pl.encode(makeFrameEnd(3, 1, 200, false));

    pl.pump(FeedConfig{1, 100, 0x88});
    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[1].frameId, 3u);
    CHECK(hasDecoderError(pl.c, DecoderError::kSequenceGap));
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kStreamError));
}

// 9. 一个坏帧（END rectCount 不匹配）后，下一个 FULL 帧可恢复提交
void bad_frame_then_full_recovers() {
    Pipeline pl;
    pl.encode(beginMsg(1, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 10, 10, 0x11));
    pl.encode(rawEnd(1, 2, 200, 0));  // 声明 2 个 RECT，实际 1 个
    pl.encode(beginMsg(2, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 16, 16, 0x22));
    pl.encode(makeFrameEnd(2, 1, 16u * 16u * 2u, false));

    pl.pumpAll();
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 2u);
    CHECK_EQ(pl.c.commits[0].byteCount, 16u * 16u * 2u);
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kEndRectCountMismatch));
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// 10. 最终 committed frame 全部字段 + 所有 rect payload
void committed_frame_fields() {
    Pipeline pl;
    pl.encode(beginMsg(7, FrameType::kFull));
    pl.encode(rectMsg(0, 0, 16, 16, 0x11));      // 512 B
    pl.encode(rectMsg(100, 50, 64, 32, 0x22));   // 4096 B
    std::vector<uint8_t> pixels(320u * 8u * 2u); // 5120 B，CHUNKED 2 包
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(i * 3);
    }
    auto big = makeFrameRect(0, 0, 320, 8, pixels.data(), pixels.size());
    CHECK(big.has_value());
    pl.encode(*big);
    const uint32_t bytes =
        16u * 16u * 2u + 64u * 32u * 2u + static_cast<uint32_t>(pixels.size());
    pl.encode(makeFrameEnd(7, 3, bytes, false));

    pl.pump(FeedConfig{1, 200, 0x7777});
    CHECK_EQ(pl.c.commits.size(), 1u);
    const auto& f = pl.c.commits[0];
    CHECK_EQ(f.frameId, 7u);
    CHECK_EQ(f.frameType, FrameType::kFull);
    CHECK_EQ(f.pixelFormat, PixelFormat::kRgb565);
    CHECK_EQ(f.width, 320u);
    CHECK_EQ(f.height, 240u);
    CHECK_EQ(f.rectCount, 3u);
    CHECK_EQ(f.byteCount, bytes);
    CHECK_EQ(pl.c.rects.size(), 3u);
    CHECK_EQ(pl.c.rectPixels.size(), 3u);
    CHECK(std::all_of(pl.c.rectPixels[0].begin(), pl.c.rectPixels[0].end(),
                      [](uint8_t b) { return b == 0x11; }));
    CHECK(std::all_of(pl.c.rectPixels[1].begin(), pl.c.rectPixels[1].end(),
                      [](uint8_t b) { return b == 0x22; }));
    CHECK_EQ(pl.c.rectPixels[2].size(), pixels.size());
    CHECK(std::memcmp(pl.c.rectPixels[2].data(), pixels.data(), pixels.size()) == 0);
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.c.decoderErrors.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

}  // namespace

void runPipelineTests() {
    std::printf("  full_frame_one_byte_chunks\n");
    full_frame_one_byte_chunks();
    std::printf("  full_frame_whole_packets\n");
    full_frame_whole_packets();
    std::printf("  full_frame_random_chunks\n");
    full_frame_random_chunks();
    std::printf("  multiple_rect\n");
    multiple_rect();
    std::printf("  chunked_rect\n");
    chunked_rect();
    std::printf("  partial_after_full\n");
    partial_after_full();
    std::printf("  crc_error_recovers\n");
    crc_error_recovers();
    std::printf("  seq_gap_recovers\n");
    seq_gap_recovers();
    std::printf("  bad_frame_then_full_recovers\n");
    bad_frame_then_full_recovers();
    std::printf("  committed_frame_fields\n");
    committed_frame_fields();
}
