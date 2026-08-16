#include "frame_assembler.h"

#include <utility>

#include "byte_order.h"

namespace espview {
namespace proto {

namespace {

// 小端读取统一走 byte_order.h（M8-A1 内部重构；wire 字节序冻结）。
// （file-local readU16/readU32 已删除，调用点改用 readU16LE/readU32LE。）

// v0.1 仅 RGB565（bpp=2）；未知格式返回 0（BEGIN 校验时拒绝）。
uint32_t bytesPerPixel(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::kRgb565:
            return 2;
    }
    return 0;
}

}  // namespace

const char* toString(FrameDiscardReason r) {
    switch (r) {
        case FrameDiscardReason::kAborted:
            return "kAborted";
        case FrameDiscardReason::kEndInvalidLayout:
            return "kEndInvalidLayout";
        case FrameDiscardReason::kEndFrameIdMismatch:
            return "kEndFrameIdMismatch";
        case FrameDiscardReason::kEndRectCountMismatch:
            return "kEndRectCountMismatch";
        case FrameDiscardReason::kEndByteCountMismatch:
            return "kEndByteCountMismatch";
        case FrameDiscardReason::kInvalidBegin:
            return "kInvalidBegin";
        case FrameDiscardReason::kInvalidRect:
            return "kInvalidRect";
        case FrameDiscardReason::kStreamError:
            return "kStreamError";
        case FrameDiscardReason::kPartialWithoutBase:
            return "kPartialWithoutBase";
        case FrameDiscardReason::kSupersededByNewBegin:
            return "kSupersededByNewBegin";
        case FrameDiscardReason::kReset:
            return "kReset";
    }
    return "kUnknown";
}

void FrameStats::onCommit(FrameType t) {
    if (t == FrameType::kFull) {
        ++commitsFull;
    } else {
        ++commitsPartial;
    }
}

void FrameStats::onDiscard(FrameDiscardReason r) {
    ++discardsTotal;
    ++discardByReason[static_cast<size_t>(r)];
}

FrameAssembler::FrameAssembler(Callbacks cb) : cb_(std::move(cb)) {}

FrameState FrameAssembler::state() const { return state_; }

void FrameAssembler::reset() {
    if (state_ == FrameState::kInFrame) {
        stats_.onDiscard(FrameDiscardReason::kReset);
        if (cb_.onDiscard) {
            cb_.onDiscard(FrameDiscardReason::kReset);
        }
    }
    state_ = FrameState::kIdle;
    begin_ = FrameBeginInfo{};
    rectCount_ = 0;
    byteCount_ = 0;
    hasCommittedBase_ = false;  // 断线/重连：提交基准清空，PARTIAL 需等 FULL 重同步
}

void FrameAssembler::onStreamError(DecoderError /*e*/) {
    // 只有正在收帧时，流级错误才会破坏帧完整性。
    if (state_ == FrameState::kInFrame) {
        discardTo(FrameDiscardReason::kStreamError);
    }
}

void FrameAssembler::onMessage(const Message& msg) {
    switch (static_cast<MessageType>(msg.type)) {
        case MessageType::kFrameBegin:
            handleBegin(msg);
            break;
        case MessageType::kFrameRect:
            handleRect(msg);
            break;
        case MessageType::kFrameEnd:
            handleEnd(msg);
            break;
        default:
            break;  // 控制消息（HELLO/INPUT/PING/...）可穿插在帧之间，忽略
    }
}

void FrameAssembler::discardTo(FrameDiscardReason reason) {
    state_ = FrameState::kDiscardFrame;
    stats_.onDiscard(reason);
    if (cb_.onDiscard) {
        cb_.onDiscard(reason);
    }
}

void FrameAssembler::handleBegin(const Message& msg) {
    // ---- 解析 + 校验（与 DESIGN.md FRAME_BEGIN Payload Layout 完全一致）----
    const uint8_t* p = msg.payload.data();
    const size_t n = msg.payload.size();

    bool ok = (n == 12);
    if (ok) {
        const uint8_t typeByte = p[2];
        const uint8_t fmtByte = p[3];
        const uint16_t width = readU16LE(p + 4);
        const uint16_t height = readU16LE(p + 6);
        const bool knownType = (typeByte == static_cast<uint8_t>(FrameType::kFull) ||
                                typeByte == static_cast<uint8_t>(FrameType::kPartial));
        ok = knownType &&
             bytesPerPixel(static_cast<PixelFormat>(fmtByte)) != 0 &&  // 未知像素格式拒绝
             width >= 1 && width <= 4096 &&                            // DESIGN.md 取值范围
             height >= 1 && height <= 4096;
    }

    if (!ok) {
        // 非法 BEGIN：旧帧（若有）作废，进入 DISCARD_FRAME 等下一个合法 BEGIN。
        if (state_ == FrameState::kInFrame) {
            discardTo(FrameDiscardReason::kInvalidBegin);
        } else if (state_ == FrameState::kIdle) {
            discardTo(FrameDiscardReason::kInvalidBegin);
        }
        // 已在 DISCARD_FRAME：保持，不重复上报。
        return;
    }

    // 合法 BEGIN。
    if (state_ == FrameState::kInFrame) {
        // 未 END 又收到新 BEGIN：旧帧作废，直接开始新帧（DESIGN.md 帧级错误处理）。
        discardTo(FrameDiscardReason::kSupersededByNewBegin);
    }
    state_ = FrameState::kInFrame;
    begin_.frameId = readU16LE(p);
    begin_.frameType = static_cast<FrameType>(p[2]);
    begin_.pixelFormat = static_cast<PixelFormat>(p[3]);
    begin_.width = readU16LE(p + 4);
    begin_.height = readU16LE(p + 6);
    begin_.byteHint = readU32LE(p + 8);
    rectCount_ = 0;
    byteCount_ = 0;
    if (cb_.onBegin) {
        cb_.onBegin(begin_);
    }
}

void FrameAssembler::handleRect(const Message& msg) {
    if (state_ != FrameState::kInFrame) {
        return;  // IDLE：无帧可归属；DISCARD_FRAME：本帧已失效（stale RECT 丢弃）
    }
    const uint8_t* p = msg.payload.data();
    const size_t n = msg.payload.size();

    bool ok = (n >= 8 && n <= kMaxMessagePayload);
    uint16_t x = 0, y = 0, w = 0, h = 0;
    uint64_t pixelBytes = 0;
    if (ok) {
        x = readU16LE(p);
        y = readU16LE(p + 2);
        w = readU16LE(p + 4);
        h = readU16LE(p + 6);
        const uint32_t bpp = bytesPerPixel(begin_.pixelFormat);
        // 边界：w/h >= 1，x+w <= width，y+h <= height（u32 运算防溢出）。
        ok = w >= 1 && h >= 1 && bpp != 0 &&
             static_cast<uint32_t>(x) + w <= begin_.width &&
             static_cast<uint32_t>(y) + h <= begin_.height;
        if (ok) {
            pixelBytes = static_cast<uint64_t>(w) * h * bpp;
            ok = (n == 8u + pixelBytes);  // 像素长度 == w*h*bpp
        }
    }
    if (ok) {
        // 计数/溢出防护：rectCount 须可被 u16 表示；byteCount 累计不得溢出 u32。
        ok = rectCount_ < kMaxFrameRectCount &&
             byteCount_ + pixelBytes <= 0xFFFFFFFFull;
    }

    if (!ok) {
        discardTo(FrameDiscardReason::kInvalidRect);
        return;
    }

    ++rectCount_;
    byteCount_ += pixelBytes;
    if (cb_.onRect) {
        const RectInfo r{x, y, w, h};
        cb_.onRect(r, p + 8, static_cast<size_t>(pixelBytes));
    }
}

void FrameAssembler::handleEnd(const Message& msg) {
    if (state_ != FrameState::kInFrame) {
        return;  // END without BEGIN（IDLE）或已作废帧（DISCARD_FRAME）：忽略
    }
    const uint8_t* p = msg.payload.data();
    const size_t n = msg.payload.size();

    if (n != 9) {
        discardTo(FrameDiscardReason::kEndInvalidLayout);
        return;
    }

    const uint8_t flags = p[8];
    if ((flags & kFrameEndFlagAborted) != 0) {
        // ABORTED：发送端主动作废，丢弃帧内全部数据（DESIGN.md）。
        discardTo(FrameDiscardReason::kAborted);
        return;
    }
    if ((flags & ~kFrameEndFlagAborted) != 0) {
        // DESIGN.md：bit1..7 = 0。
        discardTo(FrameDiscardReason::kEndInvalidLayout);
        return;
    }

    const uint16_t frameId = readU16LE(p);
    const uint16_t rectCount = readU16LE(p + 2);
    const uint32_t byteCount = readU32LE(p + 4);

    if (frameId != begin_.frameId) {
        discardTo(FrameDiscardReason::kEndFrameIdMismatch);
        return;
    }
    if (rectCount != rectCount_) {
        discardTo(FrameDiscardReason::kEndRectCountMismatch);
        return;
    }
    if (byteCount != byteCount_) {
        discardTo(FrameDiscardReason::kEndByteCountMismatch);
        return;
    }

    // ---- PARTIAL 提交语义（DESIGN.md E 节冻结）----
    // 无任何已提交基准帧时，PARTIAL 不得提交，继续等待 FULL 帧重同步。
    if (begin_.frameType == FrameType::kPartial && !hasCommittedBase_) {
        discardTo(FrameDiscardReason::kPartialWithoutBase);
        return;
    }

    // ---- 全部校验通过 → commit ----
    CommittedFrame f;
    f.frameId = begin_.frameId;
    f.frameType = begin_.frameType;
    f.pixelFormat = begin_.pixelFormat;
    f.width = begin_.width;
    f.height = begin_.height;
    f.rectCount = static_cast<uint16_t>(rectCount_);
    f.byteCount = static_cast<uint32_t>(byteCount_);
    state_ = FrameState::kIdle;
    begin_ = FrameBeginInfo{};
    rectCount_ = 0;
    byteCount_ = 0;
    hasCommittedBase_ = true;  // 提交成功 → 成为后续 PARTIAL 的基准
    stats_.onCommit(f.frameType);
    if (cb_.onCommit) {
        cb_.onCommit(f);
    }
}

}  // namespace proto
}  // namespace espview
