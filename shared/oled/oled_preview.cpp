// ESPView M7-D2 — OledPreviewSlot 实现（seqlock 无锁快照槽 + AE.2 payload 编码）。
#include "oled_preview.h"

#include <cstdint>

namespace espview {
namespace oled {

namespace {
// 读者有界自旋上限：写者连续发布时的防饿死兜底。正常 2–10Hz 发布率下
// 单次稳定读几乎从不重试（写窗口仅 ~µs 级）。
constexpr int kSnapshotMaxRetries = 1000;
}  // namespace

OledPreviewSlot::OledPreviewSlot() { reset(); }

void OledPreviewSlot::store(const uint8_t* src1024) {
    if (src1024 == nullptr) {
        return;  // 非法指针：no-op
    }
    publish(src1024);
    valid_.store(true, std::memory_order_release);
}

bool OledPreviewSlot::snapshot(uint8_t* out1024, uint16_t& outFrameId) {
    if (!copySlot(out1024)) {
        return false;
    }
    outFrameId = frameId_++;
    return true;
}

void OledPreviewSlot::reset() {
    valid_.store(false, std::memory_order_release);
    frameId_ = 0;      // frameId 归零（AE.3：握手重置 frameId/清槽）
    publish(nullptr);  // 全零覆盖（与 store 同一发布协议，读者看不到半清状态）
}

std::vector<uint8_t> OledPreviewSlot::makePhysicalPreviewPayload(
    uint16_t width, uint16_t height, uint8_t pixelFormat, uint8_t flags) {
    std::vector<uint8_t> payload(kPayloadSizeBytes, 0);
    if (!copySlot(payload.data() + 8)) {
        return {};  // 槽无效：空向量 = 本帧无内容，调用方跳过发送
    }
    const uint16_t frameId = frameId_++;  // 取最新 frameId（发送侧独占）
    // AE.2：多字节字段一律小端（LE）。
    payload[0] = static_cast<uint8_t>(frameId & 0xFFu);
    payload[1] = static_cast<uint8_t>((frameId >> 8) & 0xFFu);
    payload[2] = static_cast<uint8_t>(width & 0xFFu);
    payload[3] = static_cast<uint8_t>((width >> 8) & 0xFFu);
    payload[4] = static_cast<uint8_t>(height & 0xFFu);
    payload[5] = static_cast<uint8_t>((height >> 8) & 0xFFu);
    payload[6] = pixelFormat;
    payload[7] = flags;
    return payload;
}

bool OledPreviewSlot::valid() const {
    return valid_.load(std::memory_order_acquire);
}

bool OledPreviewSlot::copySlot(uint8_t* out1024) const {
    if (out1024 == nullptr) {
        return false;
    }
    for (int attempt = 0; attempt < kSnapshotMaxRetries; ++attempt) {
        const uint32_t s0 = seq_.load(std::memory_order_acquire);
        if ((s0 & 1u) != 0u) {
            continue;  // 写者进行中：下一轮
        }
        for (size_t i = 0; i < kSizeBytes; ++i) {
            out1024[i] = slot_[i].load(std::memory_order_acquire);
        }
        // 复制期间无新发布（seq 未变）且槽有效 → 本次副本是某次完整发布的
        // 稳定帧。若复制观察到了未发布批次的字节，happens-before 链会强制
        // 本次 seq 读看到奇数标记 → s1 != s0 → 重试（详见头文件线程模型）。
        if (seq_.load(std::memory_order_acquire) == s0 &&
            valid_.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;  // 重试超限：丢弃本帧（fire-and-forget，背压整帧丢弃语义）
}

void OledPreviewSlot::publish(const uint8_t* src1024) {
    seq_.fetch_add(1, std::memory_order_release);  // 置奇：写入进行中
    for (size_t i = 0; i < kSizeBytes; ++i) {
        const uint8_t v = src1024 != nullptr ? src1024[i] : 0;
        slot_[i].store(v, std::memory_order_release);
    }
    seq_.fetch_add(1, std::memory_order_release);  // 置偶：发布完成
}

}  // namespace oled
}  // namespace espview
