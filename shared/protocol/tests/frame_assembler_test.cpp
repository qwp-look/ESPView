// FrameAssembler 单元测试（M0-C）。
// 规范来源：docs/DESIGN.md E 节「帧级错误处理 / 三层概念 / 连接状态机 / 帧消息 Payload Layout」。
// 输入优先走 MessageEncoder + StreamDecoder 集成路径；非法载荷用 makeMessage 构造。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
using espview::proto::DisplayMode;
using espview::proto::FrameAssembler;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameState;
using espview::proto::FrameType;
using espview::proto::kFrameEndFlagAborted;
using espview::proto::kMaxFrameRectCount;
using espview::proto::kMaxMessagePayload;
using espview::proto::kMaxPacketPayload;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeFrameRect;
using espview::proto::makeMessage;
using espview::proto::makePing;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PixelFormat;
using espview::proto::RectInfo;
using espview::proto::SequenceCounter;
using espview::proto::StreamDecoder;

// ---- 本地 LE 写入 helper（构造非法载荷用，避免手工拼 wire bytes）----
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

// 任意字段的 FRAME_BEGIN 载荷（可构造非法 pixelFormat 等）。
Message rawBegin(uint16_t frameId, uint8_t type, uint8_t fmt, uint16_t w, uint16_t h,
                 uint32_t byteHint) {
    std::vector<uint8_t> p;
    p.reserve(12);
    putU16(p, frameId);
    p.push_back(type);
    p.push_back(fmt);
    putU16(p, w);
    putU16(p, h);
    putU32(p, byteHint);
    return makeMessage(static_cast<uint8_t>(MessageType::kFrameBegin), 0, std::move(p));
}

// 任意字段的 FRAME_RECT 载荷（可构造越界/长度错误）。
Message rawRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, size_t pixelBytes,
                uint8_t fill = 0x5A) {
    std::vector<uint8_t> p;
    p.reserve(8 + pixelBytes);
    putU16(p, x);
    putU16(p, y);
    putU16(p, w);
    putU16(p, h);
    p.insert(p.end(), pixelBytes, fill);
    return makeMessage(static_cast<uint8_t>(MessageType::kFrameRect), 0, std::move(p));
}

// 任意字段的 FRAME_END 载荷（可构造计数/字节数不匹配、ABORTED、未知 flags）。
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

// ---- 集成管线：Encoder → Decoder → FrameAssembler ----
struct Pipeline {
    SequenceCounter seq;
    AssemblerCollector c;
    FrameAssembler assembler;
    StreamDecoder decoder;

    Pipeline() : assembler(makeAssembler(c)), decoder(makeDecoderCallbacks()) {}

    void feed(const Message& m) {
        MessageEncoder enc(seq);
        std::vector<std::vector<uint8_t>> packets;
        const PacketError err = enc.encode(m, packets);
        CHECK_EQ(err, PacketError::kNone);
        for (const auto& p : packets) {
            decoder.feed(p);
        }
    }

    void feedBytes(const std::vector<uint8_t>& bytes) { decoder.feed(bytes); }

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

// 1. 空输入
void empty_input() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    CHECK_EQ(asm_.state(), FrameState::kIdle);
    CHECK_EQ(c.begins.size(), 0u);
    CHECK_EQ(c.rects.size(), 0u);
    CHECK_EQ(c.commits.size(), 0u);
    CHECK_EQ(c.discards.size(), 0u);
}

// 2. valid FULL frame（集成路径）
void valid_full_frame() {
    Pipeline pl;
    pl.feed(beginMsg(1, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 64, 32));
    pl.feed(makeFrameEnd(1, 1, 64 * 32 * 2, false));
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[0].frameType, FrameType::kFull);
    CHECK_EQ(pl.c.commits[0].width, 320u);
    CHECK_EQ(pl.c.commits[0].height, 240u);
    CHECK_EQ(pl.c.commits[0].rectCount, 1u);
    CHECK_EQ(pl.c.commits[0].byteCount, 64u * 32u * 2u);
    CHECK_EQ(pl.c.begins.size(), 1u);
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.rectPixels[0].size(), 64u * 32u * 2u);
    CHECK_EQ(pl.c.discards.size(), 0u);
    CHECK_EQ(pl.assembler.state(), FrameState::kIdle);
}

// 3. valid PARTIAL frame（需先有已提交 FULL 基准，见 DESIGN.md PARTIAL 提交语义）
void valid_partial_frame() {
    Pipeline pl;
    pl.feed(beginMsg(1, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(makeFrameEnd(1, 1, 200, false));  // FULL 提交 → 建立基准
    pl.feed(beginMsg(2, FrameType::kPartial));
    pl.feed(rectMsg(8, 16, 32, 24));
    pl.feed(makeFrameEnd(2, 1, 32 * 24 * 2, false));
    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[1].frameType, FrameType::kPartial);
    CHECK_EQ(pl.c.commits[1].frameId, 2u);
}

// 4. BEGIN → RECT → END 单帧
void begin_rect_end() {
    Pipeline pl;
    pl.feed(beginMsg(3, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(makeFrameEnd(3, 1, 10 * 10 * 2, false));
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.rects[0].x, 0u);
    CHECK_EQ(pl.c.rects[0].y, 0u);
    CHECK_EQ(pl.c.rects[0].w, 10u);
    CHECK_EQ(pl.c.rects[0].h, 10u);
}

// 5. multiple RECT：3 个矩形累计计数
void multiple_rect() {
    Pipeline pl;
    pl.feed(beginMsg(4, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(rectMsg(10, 0, 20, 20));
    pl.feed(rectMsg(30, 30, 40, 40));
    const uint32_t bytes = 10u * 10u * 2u + 20u * 20u * 2u + 40u * 40u * 2u;
    pl.feed(makeFrameEnd(4, 3, bytes, false));
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].rectCount, 3u);
    CHECK_EQ(pl.c.commits[0].byteCount, bytes);
    CHECK_EQ(pl.c.rects.size(), 3u);
    CHECK_EQ(pl.c.rectPixels[2].size(), 40u * 40u * 2u);
}

// 6. rectCount mismatch → discard
void rect_count_mismatch() {
    Pipeline pl;
    pl.feed(beginMsg(5, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(rawEnd(5, 2, 10 * 10 * 2, 0));  // 声明 2 个 RECT，实际 1 个
    CHECK_EQ(pl.c.commits.size(), 0u);
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kEndRectCountMismatch));
    CHECK_EQ(pl.assembler.state(), FrameState::kDiscardFrame);
}

// 7. byteCount mismatch → discard
void byte_count_mismatch() {
    Pipeline pl;
    pl.feed(beginMsg(6, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(rawEnd(6, 1, 9999, 0));  // 声明 9999 字节，实际 200
    CHECK_EQ(pl.c.commits.size(), 0u);
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kEndByteCountMismatch));
}

// 8. frameId mismatch → discard
void frame_id_mismatch() {
    Pipeline pl;
    pl.feed(beginMsg(7, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(rawEnd(8, 1, 200, 0));  // END.frameId=8 != BEGIN.frameId=7
    CHECK_EQ(pl.c.commits.size(), 0u);
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kEndFrameIdMismatch));
}

// 9. invalid rectangle bounds（x+w > width）→ discard
void invalid_rect_bounds() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(9, FrameType::kFull));          // 320x240
    asm_.onMessage(rawRect(300, 0, 30, 1, 30u * 1u * 2u));  // x+w=330 > 320
    CHECK_EQ(c.rects.size(), 0u);
    CHECK(hasDiscard(c, FrameDiscardReason::kInvalidRect));
    CHECK_EQ(asm_.state(), FrameState::kDiscardFrame);
    CHECK_EQ(c.commits.size(), 0u);
}

// 10. pixel format mismatch（未知像素格式）→ BEGIN 拒绝
void pixel_format_mismatch() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(rawBegin(10, static_cast<uint8_t>(FrameType::kFull), 0x7F, 320, 240, 0));
    CHECK_EQ(c.begins.size(), 0u);
    CHECK(hasDiscard(c, FrameDiscardReason::kInvalidBegin));
    CHECK_EQ(asm_.state(), FrameState::kDiscardFrame);
}

// 11. invalid rectangle byte length（payload != 8 + w*h*bpp）→ discard
void invalid_rect_byte_length() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(11, FrameType::kFull));
    asm_.onMessage(rawRect(0, 0, 2, 1, 3));  // RGB565 应为 8+4 字节，实际 8+3
    CHECK_EQ(c.rects.size(), 0u);
    CHECK(hasDiscard(c, FrameDiscardReason::kInvalidRect));
}

// 12. FRAME_END without BEGIN → 忽略，不提交
void end_without_begin() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(makeFrameEnd(1, 0, 0, false));
    CHECK_EQ(c.commits.size(), 0u);
    CHECK_EQ(c.discards.size(), 0u);
    CHECK_EQ(asm_.state(), FrameState::kIdle);
}

// 13. RECT without BEGIN → 忽略；随后合法帧正常提交
void rect_without_begin() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    CHECK_EQ(c.rects.size(), 0u);
    CHECK_EQ(asm_.state(), FrameState::kIdle);

    asm_.onMessage(beginMsg(12, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(makeFrameEnd(12, 1, 200, false));
    CHECK_EQ(c.commits.size(), 1u);
}

// 14. FRAME_BEGIN while in frame → 旧帧作废，新帧开始
void begin_while_in_frame() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(1, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(beginMsg(2, FrameType::kFull));  // 未 END 又来新 BEGIN
    asm_.onMessage(rectMsg(0, 0, 20, 20));
    asm_.onMessage(makeFrameEnd(2, 1, 800, false));

    CHECK_EQ(c.commits.size(), 1u);
    CHECK_EQ(c.commits[0].frameId, 2u);  // 只提交新帧
    CHECK(hasDiscard(c, FrameDiscardReason::kSupersededByNewBegin));
    CHECK_EQ(c.rects.size(), 2u);  // 两帧的 RECT 都流式到达（sink 依赖 onDiscard 丢弃旧 staging）
}

// 15. 损坏帧后新 BEGIN → 新帧正常开始
void corrupted_frame_then_new_begin() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(1, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(rawRect(0, 0, 2, 1, 3));  // 非法 RECT → 帧作废
    CHECK_EQ(asm_.state(), FrameState::kDiscardFrame);

    asm_.onMessage(beginMsg(2, FrameType::kFull));  // 新帧
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(makeFrameEnd(2, 1, 200, false));
    CHECK_EQ(c.commits.size(), 1u);
    CHECK_EQ(c.commits[0].frameId, 2u);
    CHECK(hasDiscard(c, FrameDiscardReason::kInvalidRect));
}

// 16. ABORTED → 不提交，进入 DISCARD_FRAME；stale RECT 忽略；新 BEGIN 恢复
void aborted_frame() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(3, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(rawEnd(3, 1, 200, kFrameEndFlagAborted));
    CHECK_EQ(c.commits.size(), 0u);
    CHECK(hasDiscard(c, FrameDiscardReason::kAborted));
    CHECK_EQ(asm_.state(), FrameState::kDiscardFrame);

    asm_.onMessage(rectMsg(0, 0, 10, 10));  // stale RECT：忽略
    CHECK_EQ(c.rects.size(), 1u);
    CHECK_EQ(asm_.state(), FrameState::kDiscardFrame);

    asm_.onMessage(beginMsg(4, FrameType::kFull));  // 新 BEGIN 恢复
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(makeFrameEnd(4, 1, 200, false));
    CHECK_EQ(c.commits.size(), 1u);
    CHECK_EQ(c.commits[0].frameId, 4u);
}

// 17. CRC/SEQ 错误（Decoder 集成）：错误后帧不得提交，新帧恢复
void stream_error_from_decoder() {
    Pipeline pl;
    pl.feed(beginMsg(1, FrameType::kFull));       // seq0
    pl.feed(rectMsg(0, 0, 10, 10));               // seq1
    // 大 RECT（320x13 → payload 8328，3 包 seq2,3,4）：破坏包1的 payload → CRC 失败。
    std::vector<uint8_t> pixels(320u * 13u * 2u, 0x22);
    auto big = makeFrameRect(0, 0, 320, 13, pixels.data(), pixels.size());
    CHECK(big.has_value());
    MessageEncoder enc(pl.seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*big, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 3u);
    pl.decoder.feed(packets[0]);
    std::vector<uint8_t> badP1 = packets[1];
    badP1[20 + 5] ^= 0xFF;
    pl.decoder.feed(badP1);                       // CRC 失败
    pl.decoder.feed(packets[2]);                  // 末包 → Decoder 派发为独立 RECT
    pl.feed(makeFrameEnd(1, 1, 200, false));      // seq5 END（被 DISCARD 忽略）

    CHECK_EQ(pl.c.commits.size(), 0u);            // 帧 #1 不得提交
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kStreamError));
    CHECK_EQ(pl.c.decoderErrors.size(), 1u);
    CHECK_EQ(pl.c.decoderErrors[0], DecoderError::kCrcMismatch);

    // 新帧恢复。
    pl.feed(beginMsg(2, FrameType::kFull));       // seq6
    pl.feed(rectMsg(0, 0, 10, 10));               // seq7
    pl.feed(makeFrameEnd(2, 1, 200, false));      // seq8
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 2u);
}

// 18. stale RECT after discarded frame：不得重新接入旧帧
void stale_rect_after_discard() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(1, FrameType::kFull));
    asm_.onMessage(rawRect(0, 0, 2, 1, 3));  // 非法 → 帧作废
    CHECK_EQ(c.rects.size(), 0u);

    asm_.onMessage(rectMsg(0, 0, 10, 10));  // 合法 RECT，但旧帧已作废 → 忽略
    CHECK_EQ(c.rects.size(), 0u);
    CHECK_EQ(asm_.state(), FrameState::kDiscardFrame);

    asm_.onMessage(makeFrameEnd(1, 1, 200, false));  // 旧帧 END → 忽略
    CHECK_EQ(c.commits.size(), 0u);
}

// 19. valid frame after discarded frame
void valid_frame_after_discard() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(1, FrameType::kFull));
    asm_.onMessage(rawRect(0, 0, 2, 1, 3));  // → DISCARD_FRAME
    asm_.onMessage(beginMsg(2, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(makeFrameEnd(2, 1, 200, false));
    CHECK_EQ(c.commits.size(), 1u);
    CHECK_EQ(c.commits[0].frameId, 2u);
}

// 20. frameId 回绕（uint16）：65535 → 0 均合法；跨回绕不匹配拒绝
void frame_id_wraparound() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(65535, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(rawEnd(65535, 1, 200, 0));
    CHECK_EQ(c.commits.size(), 1u);
    CHECK_EQ(c.commits[0].frameId, 65535u);

    asm_.onMessage(beginMsg(0, FrameType::kFull));  // 回绕后新帧
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(rawEnd(0, 1, 200, 0));
    CHECK_EQ(c.commits.size(), 2u);
    CHECK_EQ(c.commits[1].frameId, 0u);

    asm_.onMessage(beginMsg(65535, FrameType::kFull));  // END 跨回绕不匹配
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(rawEnd(0, 1, 200, 0));
    CHECK_EQ(c.commits.size(), 2u);
    CHECK(hasDiscard(c, FrameDiscardReason::kEndFrameIdMismatch));
}

// 21. FULL resync frame（作废后 FULL 重同步帧正常提交）
void full_resync_frame() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(1, FrameType::kPartial));
    asm_.onMessage(rawRect(0, 0, 2, 1, 3));  // 作废
    asm_.onMessage(beginMsg(2, FrameType::kFull));  // FULL 重同步
    asm_.onMessage(rectMsg(0, 0, 320, 240));
    asm_.onMessage(makeFrameEnd(2, 1, 320 * 240 * 2, false));
    CHECK_EQ(c.commits.size(), 1u);
    CHECK_EQ(c.commits[0].frameType, FrameType::kFull);
    CHECK_EQ(c.commits[0].byteCount, 320u * 240u * 2u);
}

// 22. zero-RECT FULL frame（协议允许 0..n 个 RECT）
void zero_rect_full_frame() {
    Pipeline pl;
    pl.feed(beginMsg(1, FrameType::kFull));
    pl.feed(makeFrameEnd(1, 0, 0, false));
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].rectCount, 0u);
    CHECK_EQ(pl.c.commits[0].byteCount, 0u);
    CHECK_EQ(pl.c.rects.size(), 0u);
}

// 附加：rectCount 超出 u16（65535）→ 帧作废
void rect_count_overflow() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(1, FrameType::kFull));
    const Message rect = rectMsg(0, 0, 1, 1);
    for (uint32_t i = 0; i < kMaxFrameRectCount; ++i) {
        asm_.onMessage(rect);
    }
    CHECK_EQ(c.rects.size(), static_cast<size_t>(kMaxFrameRectCount));
    CHECK_EQ(asm_.state(), FrameState::kInFrame);
    asm_.onMessage(rect);  // 第 65536 个 → rectCount 无法表示 → 作废
    CHECK(hasDiscard(c, FrameDiscardReason::kInvalidRect));
    CHECK_EQ(asm_.state(), FrameState::kDiscardFrame);
}

// 附加：控制消息穿插在帧消息之间 → 帧不受影响
void control_messages_interleaved() {
    Pipeline pl;
    pl.feed(beginMsg(1, FrameType::kFull));
    pl.feed(makePing(1));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(makePing(2));
    pl.feed(makeFrameEnd(1, 1, 200, false));
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.rects.size(), 1u);
}

// 附加：reset 时正在收帧 → onDiscard(kReset)，状态回 IDLE
void reset_mid_frame() {
    AssemblerCollector c;
    auto asm_ = makeAssembler(c);
    asm_.onMessage(beginMsg(1, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    CHECK_EQ(asm_.state(), FrameState::kInFrame);
    asm_.reset();
    CHECK_EQ(asm_.state(), FrameState::kIdle);
    CHECK(hasDiscard(c, FrameDiscardReason::kReset));

    // reset 后新帧正常。
    asm_.onMessage(beginMsg(2, FrameType::kFull));
    asm_.onMessage(rectMsg(0, 0, 10, 10));
    asm_.onMessage(makeFrameEnd(2, 1, 200, false));
    CHECK_EQ(c.commits.size(), 1u);
    CHECK_EQ(c.commits[0].frameId, 2u);
}

// 附加：大 RECT（CHUNKED 多包）经 Decoder 组装后正确汇入帧
void chunked_rect_through_decoder() {
    Pipeline pl;
    pl.feed(beginMsg(1, FrameType::kFull));
    // 帧内大 RECT（320x8 → payload 5128，2 包），经 Decoder 组装后汇入帧。
    std::vector<uint8_t> pixels(320u * 8u * 2u);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(i * 3);
    }
    auto big = makeFrameRect(0, 0, 320, 8, pixels.data(), pixels.size());
    CHECK(big.has_value());
    pl.feed(*big);
    pl.feed(makeFrameEnd(1, 1, static_cast<uint32_t>(pixels.size()), false));
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.rects.size(), 1u);
    CHECK_EQ(pl.c.rects[0].w, 320u);
    CHECK_EQ(pl.c.rectPixels[0].size(), pixels.size());
    CHECK(std::memcmp(pl.c.rectPixels[0].data(), pixels.data(), pixels.size()) == 0);
    CHECK_EQ(pl.c.commits[0].byteCount, static_cast<uint32_t>(pixels.size()));
}

// 附加：多帧连续提交（FULL → PARTIAL → FULL）
void multiple_frames_committed() {
    Pipeline pl;
    for (uint16_t i = 1; i <= 3; ++i) {
        pl.feed(beginMsg(i, i == 2 ? FrameType::kPartial : FrameType::kFull));
        pl.feed(rectMsg(0, 0, 10, 10));
        pl.feed(makeFrameEnd(i, 1, 200, false));
    }
    CHECK_EQ(pl.c.commits.size(), 3u);
    CHECK_EQ(pl.c.commits[0].frameId, 1u);
    CHECK_EQ(pl.c.commits[1].frameId, 2u);
    CHECK_EQ(pl.c.commits[2].frameId, 3u);
    CHECK_EQ(pl.c.commits[1].frameType, FrameType::kPartial);
    CHECK_EQ(pl.c.discards.size(), 0u);
}


// 附加：PARTIAL 无已提交基准帧 → 不提交（kPartialWithoutBase），FULL 后恢复
void partial_without_base() {
    Pipeline pl;
    pl.feed(beginMsg(1, FrameType::kPartial));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(makeFrameEnd(1, 1, 200, false));
    CHECK_EQ(pl.c.commits.size(), 0u);
    CHECK(hasDiscard(pl.c, FrameDiscardReason::kPartialWithoutBase));
    CHECK_EQ(pl.assembler.state(), FrameState::kDiscardFrame);

    // 之后 FULL 帧提交 → 建立基准
    pl.feed(beginMsg(2, FrameType::kFull));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(makeFrameEnd(2, 1, 200, false));
    CHECK_EQ(pl.c.commits.size(), 1u);
    CHECK_EQ(pl.c.commits[0].frameId, 2u);
    CHECK_EQ(pl.c.commits[0].frameType, FrameType::kFull);

    // 基准建立后 PARTIAL 再次可用
    pl.feed(beginMsg(3, FrameType::kPartial));
    pl.feed(rectMsg(0, 0, 10, 10));
    pl.feed(makeFrameEnd(3, 1, 200, false));
    CHECK_EQ(pl.c.commits.size(), 2u);
    CHECK_EQ(pl.c.commits[1].frameId, 3u);
    CHECK_EQ(pl.c.commits[1].frameType, FrameType::kPartial);
    CHECK_EQ(pl.c.discards.size(), 1u);  // 只有最初的 kPartialWithoutBase
}

}  // namespace


void runFrameAssemblerTests() {
    std::printf("  empty_input\n");
    empty_input();
    std::printf("  valid_full_frame\n");
    valid_full_frame();
    std::printf("  valid_partial_frame\n");
    valid_partial_frame();
    std::printf("  begin_rect_end\n");
    begin_rect_end();
    std::printf("  multiple_rect\n");
    multiple_rect();
    std::printf("  rect_count_mismatch\n");
    rect_count_mismatch();
    std::printf("  byte_count_mismatch\n");
    byte_count_mismatch();
    std::printf("  frame_id_mismatch\n");
    frame_id_mismatch();
    std::printf("  invalid_rect_bounds\n");
    invalid_rect_bounds();
    std::printf("  pixel_format_mismatch\n");
    pixel_format_mismatch();
    std::printf("  invalid_rect_byte_length\n");
    invalid_rect_byte_length();
    std::printf("  end_without_begin\n");
    end_without_begin();
    std::printf("  rect_without_begin\n");
    rect_without_begin();
    std::printf("  begin_while_in_frame\n");
    begin_while_in_frame();
    std::printf("  corrupted_frame_then_new_begin\n");
    corrupted_frame_then_new_begin();
    std::printf("  aborted_frame\n");
    aborted_frame();
    std::printf("  stream_error_from_decoder\n");
    stream_error_from_decoder();
    std::printf("  stale_rect_after_discard\n");
    stale_rect_after_discard();
    std::printf("  valid_frame_after_discard\n");
    valid_frame_after_discard();
    std::printf("  frame_id_wraparound\n");
    frame_id_wraparound();
    std::printf("  full_resync_frame\n");
    full_resync_frame();
    std::printf("  zero_rect_full_frame\n");
    zero_rect_full_frame();
    std::printf("  rect_count_overflow\n");
    rect_count_overflow();
    std::printf("  control_messages_interleaved\n");
    control_messages_interleaved();
    std::printf("  reset_mid_frame\n");
    reset_mid_frame();
    std::printf("  chunked_rect_through_decoder\n");
    chunked_rect_through_decoder();
    std::printf("  multiple_frames_committed\n");
    multiple_frames_committed();
    std::printf("  partial_without_base\n");
    partial_without_base();
}