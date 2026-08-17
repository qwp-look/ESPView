// ESPView — RemoteDisplay 实现（M5-A）。
//
// 状态机摘要（详见 remote_display.h 注释 + docs/DESIGN.md §31）：
//   UI 侧（writeRect/flush/dropPendingFrame）：
//     - 无开放帧时的首个 writeRect 开始新帧（frameId++，FULL/PARTIAL 由
//       needFull_ 决定），矩形拷贝进有界槽队列；
//     - flush() 只置 endQueued_ 标记（END 不需要槽位）；队列排空后由 TX 发 END；
//     - 队列满 → kQueueFull（不改变帧状态）；调用方超时后 dropPendingFrame()
//       作废当前帧（abortPending_）+ 置 needFull_（下一帧 FULL resync）；
//   TX 侧（pump）：
//     - 弹出条目：作废帧条目直接丢弃；正常条目先发 FRAME_BEGIN（isFirst）再
//       流式发 FRAME_RECT（slot buffer 作 IMessagePayloadSource）；
//     - 队列排空后：endQueued_ → FRAME_END；abortPending_ 且 wire 上有 BEGIN
//       → FRAME_END(ABORTED)；abortPending_ 且从未发 BEGIN → 直接清状态。
// 内存模型：槽 buffer 在 init 时预分配；pump 发送期间持有该槽（阻塞发送），
// 发送完成后才释放槽位 —— 全程无整屏 framebuffer。

#include "remote_display.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace espview {
namespace display {

namespace {

// RECT 消息流式载荷源：8B 矩形头（x/y/w/h LE）+ 槽内像素。
// 与 makeFrameRect 的 payload layout 逐字节一致（DESIGN.md E 节 FRAME_RECT）。
class SlotRectSource : public proto::IMessagePayloadSource {
public:
    SlotRectSource(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   const uint8_t* pixels, size_t pixelBytes)
        : x_(x), y_(y), w_(w), h_(h), pixels_(pixels), pixelBytes_(pixelBytes) {}

    size_t read(uint8_t* dst, size_t maxBytes) override {
        size_t produced = 0;
        while (produced < maxBytes && pos_ < total()) {
            if (pos_ < 8u) {
                dst[produced++] = headerByte(static_cast<uint8_t>(pos_));
            } else {
                dst[produced++] = pixels_[pos_ - 8u];
            }
            ++pos_;
        }
        return produced;
    }

private:
    size_t total() const { return 8u + pixelBytes_; }
    uint8_t headerByte(uint8_t i) const {
        const uint8_t v[8] = {
            static_cast<uint8_t>(x_ & 0xFFu), static_cast<uint8_t>(x_ >> 8),
            static_cast<uint8_t>(y_ & 0xFFu), static_cast<uint8_t>(y_ >> 8),
            static_cast<uint8_t>(w_ & 0xFFu), static_cast<uint8_t>(w_ >> 8),
            static_cast<uint8_t>(h_ & 0xFFu), static_cast<uint8_t>(h_ >> 8),
        };
        return v[i];
    }

    uint16_t x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    const uint8_t* pixels_ = nullptr;
    size_t pixelBytes_ = 0;
    size_t pos_ = 0;
};

uint64_t steadyNowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

RemoteDisplay::RemoteDisplay(IFrameSink& sink, Config cfg)
    : sink_(sink), cfg_(cfg), nowMs_(steadyNowMs) {
    init(cfg_);
}

DisplayStatus RemoteDisplay::init(const DisplayConfig& cfg) {
    if (cfg.width < 1 || cfg.width > 4096 || cfg.height < 1 || cfg.height > 4096) {
        return DisplayStatus::kInvalidParam;
    }
    if (cfg.format != proto::PixelFormat::kRgb565) {
        return DisplayStatus::kNotSupported;  // v0.1 只支持 RGB565
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_.width = cfg.width;
    cfg_.height = cfg.height;
    cfg_.format = cfg.format;
    info_.width = cfg.width;
    info_.height = cfg.height;
    info_.format = cfg.format;
    info_.supportsDirtyRect = true;

    // 队列槽预分配（初始化时一次性分配，运行期零分配）。
    const size_t slots = cfg_.queueSlots < 1 ? 1 : cfg_.queueSlots;
    slots_.resize(slots);
    for (auto& s : slots_) {
        s.pixels.resize(cfg_.slotPixelBytes);
    }
    head_ = 0;
    count_ = 0;
    building_ = false;
    endQueued_ = false;
    abortPending_.reset();
    wireOpen_ = false;
    needFull_ = true;  // 初始化后首帧必须 FULL（建立 PC 基准）
    return DisplayStatus::kOk;
}

DisplayStatus RemoteDisplay::writeRect(int x, int y, int w, int h,
                                       const uint8_t* pixels) {
    if (!enabled_) {
        return DisplayStatus::kNotEnabled;
    }
    if (!connected_) {
        return DisplayStatus::kNotConnected;
    }
    if (!valid() || x < 0 || y < 0 || w <= 0 || h <= 0) {
        return DisplayStatus::kInvalidParam;
    }
    if (x + w > info_.width || y + h > info_.height) {
        return DisplayStatus::kInvalidParam;
    }
    if (info_.format != proto::PixelFormat::kRgb565) {
        return DisplayStatus::kNotSupported;
    }
    const size_t pixelBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 2u;
    if (pixelBytes == 0 || pixelBytes > cfg_.slotPixelBytes) {
        return DisplayStatus::kRectTooLarge;
    }
    if (pixels == nullptr) {
        return DisplayStatus::kInvalidParam;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (abortPending_ && frameId_ == *abortPending_) {
        // 当前帧已作废（ABORTED END 待发）：本矩形丢弃。
        ++stats_.rectsDropped;
        return DisplayStatus::kFrameAborted;
    }
    if (!building_) {
        if (endQueued_ || count_ > 0) {
            return DisplayStatus::kFrameBusy;  // 上一帧尚未结束
        }
        ++frameId_;  // uint16 回绕（0xFFFF → 0）
        building_ = true;
        frameType_ = needFull_ ? proto::FrameType::kFull : proto::FrameType::kPartial;
        needFull_ = false;
        rectCount_ = 0;
        byteCount_ = 0;
    }
    if (!hasFreeSlotLocked()) {
        ++stats_.queueFullEvents;
        return DisplayStatus::kQueueFull;
    }
    // 入队（尾部槽）。
    const size_t tail = (head_ + count_) % slots_.size();
    Slot& s = slots_[tail];
    s.inUse = true;
    s.isFirst = (rectCount_ == 0);
    s.frameType = frameType_;
    s.frameId = frameId_;
    s.x = static_cast<uint16_t>(x);
    s.y = static_cast<uint16_t>(y);
    s.w = static_cast<uint16_t>(w);
    s.h = static_cast<uint16_t>(h);
    s.pixelBytes = pixelBytes;
    std::copy_n(pixels, pixelBytes, s.pixels.data());
    ++count_;
    if (count_ > stats_.queuePeak) {
        stats_.queuePeak = count_;  // M6-C：队列占用峰值（诊断/背压观测）
    }
    ++rectCount_;
    byteCount_ += pixelBytes;
    return DisplayStatus::kOk;
}

DisplayStatus RemoteDisplay::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!building_) {
        return DisplayStatus::kOk;  // 无开放帧：no-op
    }
    // END 不需要槽位：置标记，由 TX 在队列排空后发送。
    endQueued_ = true;
    building_ = false;
    return DisplayStatus::kOk;
}

DisplayStatus RemoteDisplay::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    if (!enabled) {
        // 关闭后端：丢弃未发送数据，下一帧 FULL。
        freeAllLocked();
        building_ = false;
        endQueued_ = false;
        abortPending_.reset();
        needFull_ = true;
        if (wireOpen_) {
            ++stats_.framesDropped;
            wireOpen_ = false;
        }
    }
    return DisplayStatus::kOk;
}

void RemoteDisplay::onConnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    needFull_ = true;  // 每次连接后首帧 FULL（重同步）
}

void RemoteDisplay::onDisconnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
        return;  // 幂等
    }
    connected_ = false;
    if (building_ || endQueued_ || count_ > 0 || wireOpen_) {
        ++stats_.framesDropped;
    }
    freeAllLocked();
    building_ = false;
    endQueued_ = false;
    abortPending_.reset();
    wireOpen_ = false;
    needFull_ = true;
}

void RemoteDisplay::dropPendingFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!abortPending_) {
        if (building_ || endQueued_ || count_ > 0 || wireOpen_) {
            abortPending_ = frameId_;
            ++stats_.framesDropped;
        } else {
            // 无开放帧：仅标记 FULL resync（防御性）。
            needFull_ = true;
            return;
        }
    }
    building_ = false;
    endQueued_ = false;
    needFull_ = true;
}

bool RemoteDisplay::pump() {
    // ---- Phase 1：队列空时的帧结束 / 作废收尾 ----
    // 说明：END / ABORTED END 发送必须在锁外执行（sink 可能阻塞；锁只保护状态）。
    bool needAbortEnd = false;
    bool needEnd = false;
    uint16_t endId = 0;
    uint32_t endRects = 0;
    uint32_t endBytes = 0;
    uint64_t endBeginMs = 0;
    proto::FrameType endType = proto::FrameType::kFull;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) {
            if (abortPending_ && wireOpen_) {
                endId = wireFrameId_;
                endRects = wireRectCount_;
                endBytes = static_cast<uint32_t>(wireByteCount_);
                needAbortEnd = true;
                wireOpen_ = false;
                endQueued_ = false;
                building_ = false;
                abortPending_.reset();
            } else if (endQueued_ && wireOpen_) {
                endId = wireFrameId_;
                endType = wireType_;
                endRects = wireRectCount_;
                endBytes = static_cast<uint32_t>(wireByteCount_);
                endBeginMs = wireBeginMs_;
                needEnd = true;
                wireOpen_ = false;
                endQueued_ = false;
                building_ = false;
            } else if (abortPending_) {
                // 作废帧从未发 BEGIN（PC 无感知）：直接清状态。
                abortPending_.reset();
                building_ = false;
                endQueued_ = false;
                return true;
            } else {
                return false;  // 空闲
            }
        }
    }
    if (needAbortEnd) {
        const proto::SendResult r = sink_.send(
            proto::makeFrameEnd(endId, endRects, endBytes, true));  // 锁外
        if (r != proto::SendResult::kOk) {
            needFull_ = true;  // ABORTED END 未上 wire：PC 仍见开放帧，下一帧 FULL resync
        }
        return true;
    }
    if (needEnd) {
        const proto::SendResult r = sink_.send(
            proto::makeFrameEnd(endId, endRects, endBytes, false));
        const uint64_t now = nowMs_();
        if (r == proto::SendResult::kOk) {
            std::lock_guard<std::mutex> lock(mutex_);
            updateLastFrameLocked(endType, endRects, endBytes,
                                  now > endBeginMs ? now - endBeginMs : 0);
        } else {
            needFull_ = true;  // END 未上 wire：PC 仍见开放帧，下一帧 FULL resync；不虚增统计
        }
        return true;
    }

    // ---- Phase 2：弹出并发送一个条目 ----
    uint16_t fid = 0;
    bool isFirst = false;
    proto::FrameType ftype = proto::FrameType::kFull;
    uint16_t rx = 0, ry = 0, rw = 0, rh = 0;
    size_t rbytes = 0;
    const uint8_t* pixels = nullptr;
    bool discard = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Slot& s = slots_[head_];
        if (abortPending_ && s.frameId == *abortPending_) {
            ++stats_.rectsDropped;
            popFrontLocked();
            discard = true;
        } else {
            fid = s.frameId;
            isFirst = s.isFirst;
            ftype = s.frameType;
            rx = s.x; ry = s.y; rw = s.w; rh = s.h;
            rbytes = s.pixelBytes;
            pixels = s.pixels.data();
        }
    }
    if (discard) {
        notifySlotFreed();
        return true;
    }

    // 锁外发送（槽 buffer 由本函数独占持有；UI 只写尾部槽）。
    bool wireActive = true;
    if (isFirst) {
        const auto begin = proto::makeFrameBegin(
            fid, ftype, info_.format,
            static_cast<uint16_t>(info_.width), static_cast<uint16_t>(info_.height), 0);
        const proto::SendResult r =
            begin.has_value() ? sink_.send(*begin) : proto::SendResult::kInvalidMessage;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (r == proto::SendResult::kOk) {
                wireOpen_ = true;
                wireFrameId_ = fid;
                wireType_ = ftype;
                wireRectCount_ = 0;
                wireByteCount_ = 0;
                wireBeginMs_ = nowMs_();
            } else {
                wireActive = false;
                needFull_ = true;  // BEGIN 失败：本帧作废，下一帧 FULL
            }
        }
    }
    if (wireActive) {
        proto::MessageHeader hdr;
        hdr.type = static_cast<uint8_t>(proto::MessageType::kFrameRect);
        hdr.flags = 0;
        SlotRectSource src(rx, ry, rw, rh, pixels, rbytes);
        const proto::SendResult r = sink_.sendStreaming(hdr, src);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (r == proto::SendResult::kOk) {
                ++stats_.rectsSent;
                ++wireRectCount_;
                wireByteCount_ += rbytes;
            } else if (wireOpen_) {
                // 部分 RECT 已发出：作废整帧，PC 不会把它当完整画面。
                abortPending_ = wireFrameId_;
                needFull_ = true;
                ++stats_.framesDropped;
            } else {
                needFull_ = true;
            }
        }
    }

    // ---- Phase 3：释放槽 + 排空后 END/ABORT 收尾 ----
    bool queueEmpty = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        popFrontLocked();
        queueEmpty = (count_ == 0);
    }
    notifySlotFreed();
    if (queueEmpty) {
        needAbortEnd = false;
        needEnd = false;
        endId = 0;
        endRects = 0;
        endBytes = 0;
        endBeginMs = 0;
        endType = proto::FrameType::kFull;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (abortPending_ && wireOpen_) {
                endId = wireFrameId_;
                endRects = wireRectCount_;
                endBytes = static_cast<uint32_t>(wireByteCount_);
                needAbortEnd = true;
                wireOpen_ = false;
                endQueued_ = false;
                building_ = false;
                abortPending_.reset();
            } else if (endQueued_ && wireOpen_) {
                endId = wireFrameId_;
                endType = wireType_;
                endRects = wireRectCount_;
                endBytes = static_cast<uint32_t>(wireByteCount_);
                endBeginMs = wireBeginMs_;
                needEnd = true;
                wireOpen_ = false;
                endQueued_ = false;
                building_ = false;
            } else if (abortPending_) {
                abortPending_.reset();
                building_ = false;
                endQueued_ = false;
            }
        }
        if (needAbortEnd) {
            const proto::SendResult r = sink_.send(
                proto::makeFrameEnd(endId, endRects, endBytes, true));
            if (r != proto::SendResult::kOk) {
                needFull_ = true;  // ABORTED END 未上 wire：下一帧 FULL resync
            }
            return true;
        }
        if (needEnd) {
            const proto::SendResult r = sink_.send(
                proto::makeFrameEnd(endId, endRects, endBytes, false));
            const uint64_t now = nowMs_();
            if (r == proto::SendResult::kOk) {
                std::lock_guard<std::mutex> lock(mutex_);
                updateLastFrameLocked(endType, endRects, endBytes,
                                      now > endBeginMs ? now - endBeginMs : 0);
            } else {
                needFull_ = true;  // END 未上 wire：下一帧 FULL resync；不虚增统计
            }
            return true;
        }
    }
    return true;
}

bool RemoteDisplay::hasFreeSlot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasFreeSlotLocked();
}

size_t RemoteDisplay::queuedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

bool RemoteDisplay::frameInFlight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return building_ || endQueued_ || count_ > 0;
}

proto::FrameType RemoteDisplay::nextFrameType() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (building_ || endQueued_) {
        return frameType_;
    }
    return needFull_ ? proto::FrameType::kFull : proto::FrameType::kPartial;
}

RemoteDisplay::DebugState RemoteDisplay::debugState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    DebugState s;
    s.connected = connected_;
    s.building = building_;
    s.endQueued = endQueued_;
    s.frameId = frameId_;
    s.frameType = static_cast<uint8_t>(frameType_);
    s.rectCount = rectCount_;
    s.byteCount = byteCount_;
    s.abortPending = abortPending_.has_value();
    s.abortFrameId = abortPending_.value_or(0);
    s.wireOpen = wireOpen_;
    s.wireFrameId = wireFrameId_;
    s.queued = count_;
    s.queueFullEvents = stats_.queueFullEvents;
    s.framesDropped = stats_.framesDropped;
    return s;
}
DisplayStats RemoteDisplay::statsSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void RemoteDisplay::setSlotFreedCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    slotFreedCb_ = std::move(cb);
}

void RemoteDisplay::setClock(std::function<uint64_t()> nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    nowMs_ = nowMs ? std::move(nowMs) : std::function<uint64_t()>(steadyNowMs);
}

void RemoteDisplay::popFrontLocked() {
    if (count_ == 0) {
        return;
    }
    slots_[head_].inUse = false;
    head_ = (head_ + 1) % slots_.size();
    --count_;
}

void RemoteDisplay::freeAllLocked() {
    for (auto& s : slots_) {
        s.inUse = false;
    }
    head_ = 0;
    count_ = 0;
}

void RemoteDisplay::notifySlotFreed() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = slotFreedCb_;
    }
    if (cb) {
        cb();
    }
}

void RemoteDisplay::updateLastFrameLocked(proto::FrameType t, uint32_t rectCount,
                                          uint32_t bytes, uint64_t elapsedMs) {
    stats_.lastFrameId = wireFrameId_;
    stats_.lastFrameType = t;
    stats_.lastRectCount = rectCount;
    stats_.lastFrameBytes = bytes;
    stats_.lastFrameElapsedMs = elapsedMs;
    if (t == proto::FrameType::kFull) {
        ++stats_.framesFull;
        stats_.fullPixelBytes += bytes;
    } else {
        ++stats_.framesPartial;
        stats_.partialPixelBytes += bytes;
    }
}

}  // namespace display
}  // namespace espview
