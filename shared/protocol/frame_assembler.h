// ESPView — FrameAssembler（M0-C）
//
// 规范来源：docs/DESIGN.md E 节「帧级错误处理 / 三层概念 / 连接状态机」。
//
// 职责：把 Decoder 输出的完整 Message 流组装为「完整提交帧」。
//   FRAME_BEGIN → 0..N × FRAME_RECT → FRAME_END → validate → commit / discard
// 上层（PC 显示层 / FrameBuilder / sink）永远只看到完整提交的 Frame：
//   帧内任何错误（含 Decoder 上报的 CRC/SEQ 错误）→ 本帧作废（DISCARD_FRAME），
//   直到下一个 FRAME_BEGIN 才恢复。
// PARTIAL 提交语义（DESIGN.md E 节，M0 收尾冻结）：PARTIAL 帧只允许应用到最近一次
//   成功提交的帧；无任何已提交基准帧时 PARTIAL 不得提交（kPartialWithoutBase），
//   继续等待 FULL 帧重同步。
//
// 内存模型：本类不持有 framebuffer、不累积像素、不分配固定 1 MiB 缓冲。
//   RECT 通过回调流式透传（像素指针仅在回调期间有效），只保留帧元数据与计数。
//
// 纯 C++17，零平台依赖（无 ESP-IDF / Qt / Windows API / 第三方库）。
// 错误路径不使用异常。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "decoder.h"  // DecoderError（onStreamError 输入）
#include "message.h"
#include "protocol.h"

namespace espview {
namespace proto {

// 一帧内 RECT 数量上限：FRAME_END.rectCount 为 uint16（0..65535），
// 超过该值 rectCount 无法表示 → 帧作废（DESIGN.md 帧消息 Payload Layout）。
constexpr uint16_t kMaxFrameRectCount = 0xFFFF;

// FrameAssembler 状态机。
enum class FrameState : uint8_t {
    kIdle,          // 等待 FRAME_BEGIN
    kInFrame,       // 正在收集当前帧
    kDiscardFrame,  // 当前帧已失效，忽略非 FRAME_BEGIN 消息，直到新的 FRAME_BEGIN
};

// FRAME_BEGIN 的解析结果（只读元数据，不持有像素）。
struct FrameBeginInfo {
    uint16_t frameId = 0;
    FrameType frameType = FrameType::kFull;
    PixelFormat pixelFormat = PixelFormat::kRgb565;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t byteHint = 0;
};

// FRAME_RECT 的解析结果（像素由 RectCallback 单独给出）。
struct RectInfo {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
};

// 完整提交帧（FRAME_END 校验全部通过后产生）。
struct CommittedFrame {
    uint16_t frameId = 0;
    FrameType frameType = FrameType::kFull;
    PixelFormat pixelFormat = PixelFormat::kRgb565;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t rectCount = 0;   // 实际 FRAME_RECT 消息数
    uint32_t byteCount = 0;   // 实际像素总字节数（w*h*bpp 之和）
};

// 帧作废原因（onDiscard 上报）。
enum class FrameDiscardReason : uint8_t {
    kAborted,               // FRAME_END.flags.ABORTED=1：发送端主动作废
    kEndInvalidLayout,      // FRAME_END payload 长度错误或 flags 未知位非零
    kEndFrameIdMismatch,    // FRAME_END.frameId != FRAME_BEGIN.frameId
    kEndRectCountMismatch,  // FRAME_END.rectCount != 实际 RECT 数
    kEndByteCountMismatch,  // FRAME_END.byteCount != 实际像素字节数
    kInvalidBegin,          // FRAME_BEGIN payload 布局/字段非法
    kInvalidRect,           // FRAME_RECT 越界/长度/计数/溢出
    kStreamError,           // Decoder 上报错误（CRC/SEQ/...），帧不可信
    kPartialWithoutBase,    // PARTIAL 帧但当前无已提交基准帧（等待 FULL 重同步）
    kSupersededByNewBegin,  // 未 END 又收到新的 FRAME_BEGIN：旧帧作废
    kReset,                 // reset() 时正在收帧（断线/重连）
};

const char* toString(FrameDiscardReason r);

// 帧统计（M4：commit/discard 分域计数；纯统计，不改变协议/wire）。
struct FrameStats {
    uint64_t commitsFull = 0;
    uint64_t commitsPartial = 0;
    uint64_t discardsTotal = 0;
    uint64_t discardByReason[static_cast<size_t>(FrameDiscardReason::kReset) + 1] = {};

    uint64_t commits() const { return commitsFull + commitsPartial; }
    uint64_t discards(FrameDiscardReason r) const {
        return discardByReason[static_cast<size_t>(r)];
    }
    void onCommit(FrameType t);
    void onDiscard(FrameDiscardReason r);
};

class FrameAssembler {
public:
    // 回调集（全部可选；像素指针仅在 onRect 调用期间有效，调用方不得缓存指针）。
    struct Callbacks {
        std::function<void(const FrameBeginInfo& begin)> onBegin;
        std::function<void(const RectInfo& rect, const uint8_t* pixels, size_t pixelBytes)>
            onRect;
        std::function<void(const CommittedFrame& frame)> onCommit;
        std::function<void(FrameDiscardReason reason)> onDiscard;
    };

    explicit FrameAssembler(Callbacks cb);

    // 输入：Decoder 输出的完整 Message 流（控制消息穿插时自动忽略）。
    void onMessage(const Message& msg);

    // 输入：Decoder 的错误回调转发。正在收帧时 → 当前帧作废（DISCARD_FRAME）。
    void onStreamError(DecoderError e);

    // 复位：断线/重连/握手时调用；正在收帧会触发 onDiscard(kReset)。
    void reset();

    FrameState state() const;
    const FrameStats& stats() const { return stats_; }

private:
    void handleBegin(const Message& msg);
    void handleRect(const Message& msg);
    void handleEnd(const Message& msg);
    void discardTo(FrameDiscardReason reason);  // 进入 DISCARD_FRAME 并上报

    Callbacks cb_;
    FrameState state_ = FrameState::kIdle;

    // 当前帧元数据（IN_FRAME 期间有效）。
    FrameBeginInfo begin_;
    uint32_t rectCount_ = 0;    // 实际 RECT 数（uint32，防 u16 溢出）
    uint64_t byteCount_ = 0;    // 实际像素字节累计（uint64，防 u32 溢出）
    bool hasCommittedBase_ = false;  // 最近一次成功提交的帧是否存在（PARTIAL 提交基准）
    FrameStats stats_;
};

}  // namespace proto
}  // namespace espview