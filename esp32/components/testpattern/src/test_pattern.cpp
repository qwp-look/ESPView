// ESPView M1-3C — TestPattern 实现（确定性帧发送脚本，test-only）。

#include "testpattern/test_pattern.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace espview {

namespace {

constexpr size_t kTaskStackWords = 4096;
constexpr UBaseType_t kTaskPriority = 4;
constexpr TickType_t kSignalWaitTicks = pdMS_TO_TICKS(200);
constexpr uint16_t kDisplayWidth = 320;
constexpr uint16_t kDisplayHeight = 240;
// 320x240 RGB565 单 RECT 完整 Message payload = 8B 矩形头 + 153600B 像素。
constexpr uint32_t kLargeRectPayloadBytes = 8u + 153600u;

// M1-3C：确定性单 RECT 流式载荷源（不分配 153608B 连续缓冲）。
// read() 按需产生 8B 矩形头 + 320x240 RGB565 像素；同时采样流式期间的
// 内部堆低水位（heap_caps_get_free_size(MALLOC_CAP_INTERNAL)）。
// 像素公式与 PC 侧 verifyFramePixels 一致：rectId=0，
//   lo=(frameId+rectId+x)&0xFF, hi=(frameId+y+1)&0xFF，小端字节对。
class RectPatternSource : public proto::IMessagePayloadSource {
public:
    RectPatternSource(uint16_t frameId, uint16_t width, uint16_t height)
        : frameId_(frameId), width_(width), height_(height) {
        minHeapDuring_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    }

    size_t read(uint8_t* dst, size_t maxBytes) override {
        const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (freeNow < minHeapDuring_) {
            minHeapDuring_ = freeNow;
        }
        const size_t total = kLargeRectPayloadBytes;
        size_t produced = 0;
        while (produced < maxBytes && offset_ < total) {
            if (offset_ < 8u) {
                dst[produced++] = headerByte(static_cast<uint8_t>(offset_));
            } else {
                const size_t pixelOff = offset_ - 8u;  // 像素区字节偏移
                const size_t idx = pixelOff / 2u;      // 像素索引
                const uint16_t x = static_cast<uint16_t>(idx % width_);
                const uint16_t y = static_cast<uint16_t>(idx / width_);
                const uint8_t lo = static_cast<uint8_t>(frameId_ + x);  // rectId=0
                const uint8_t hi = static_cast<uint8_t>(frameId_ + y + 1u);
                dst[produced++] = (pixelOff & 1u) == 0u ? lo : hi;
            }
            ++offset_;
        }
        return produced;
    }

    size_t minHeapDuring() const { return minHeapDuring_; }

private:
    uint8_t headerByte(uint8_t i) const {
        const uint16_t w = width_;
        const uint16_t h = height_;
        const uint8_t v[8] = {
            0, 0,  // x = 0（LE）
            0, 0,  // y = 0（LE）
            static_cast<uint8_t>(w & 0xFFu), static_cast<uint8_t>(w >> 8),
            static_cast<uint8_t>(h & 0xFFu), static_cast<uint8_t>(h >> 8),
        };
        return v[i];
    }

    uint16_t frameId_;
    uint16_t width_;
    uint16_t height_;
    size_t offset_ = 0;
    size_t minHeapDuring_ = 0;
};

}  // namespace

TestPattern::TestPattern(Sender sender, StreamingSender streamingSender)
    : sender_(std::move(sender)), streamingSender_(std::move(streamingSender)) {
    startSem_ = xSemaphoreCreateBinary();
}

TestPattern::~TestPattern() {
    running_ = false;
    if (startSem_ != nullptr) {
        xSemaphoreGive(startSem_);
    }
    if (task_ != nullptr) {
        vTaskDelete(task_);
        task_ = nullptr;
    }
    if (startSem_ != nullptr) {
        vSemaphoreDelete(startSem_);
        startSem_ = nullptr;
    }
}

void TestPattern::start() {
    if (startSem_ == nullptr) {
        return;
    }
    running_ = true;
    xTaskCreate(taskEntry, "espview_tp", kTaskStackWords, this, kTaskPriority, &task_);
}

void TestPattern::onSessionState(proto::SessionState s) {
    if (s == proto::SessionState::kConnected) {
        connected_ = true;
        if (startSem_ != nullptr) {
            xSemaphoreGive(startSem_);  // 唤醒脚本任务（从 cycle 0 重新开始）
        }
    } else if (s == proto::SessionState::kDisconnected) {
        connected_ = false;  // 脚本在下一个 step/send 处中止
    }
}

void TestPattern::taskEntry(void* arg) {
    auto* self = static_cast<TestPattern*>(arg);
    self->taskLoop();
    vTaskDelete(nullptr);
}

void TestPattern::taskLoop() {
    while (running_) {
        // 等待 CONNECTED 信号；期间每 200ms 检查运行标志。
        if (xSemaphoreTake(startSem_, kSignalWaitTicks) == pdTRUE && running_) {
            runScript();
        }
    }
}

void TestPattern::runScript() {
    while (connected_ && running_) {
        const uint16_t base = static_cast<uint16_t>(cycle_ * 100u);
        if (!sendPartial(static_cast<uint16_t>(base + 1u), 0, 0, 32, 32)) {
            break;  // 无基准 → 接收端拒绝（重连后自动验证）
        }
        vTaskDelay(pdMS_TO_TICKS(400));
        if (!sendPartial(static_cast<uint16_t>(base + 2u), 32, 32, 32, 32)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(400));
        if (!sendSmallFull(static_cast<uint16_t>(base + 10u))) {
            break;  // 建立提交基准
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!sendPartial(static_cast<uint16_t>(base + 11u), 0, 0, 16, 16)) {
            break;  // 有基准 → 提交
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!sendLargeFull(static_cast<uint16_t>(base + 20u))) {
            break;  // 153600B CHUNKED
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (!sendSmallFull(static_cast<uint16_t>(base + 30u))) {
            break;  // 坏帧后重同步恢复目标
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!sendPartial(static_cast<uint16_t>(base + 31u), 64, 64, 32, 32)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        ++cycle_;
    }
}

bool TestPattern::send(const proto::Message& msg) {
    if (!connected_ || !running_) {
        return false;
    }
    const proto::SendResult r = sender_(msg);
    if (r != proto::SendResult::kOk) {
        // 会话断开（kNotConnected）/ 背压（kBackpressure）/ 传输错误：中止当前脚本。
        if (r == proto::SendResult::kNotConnected || r == proto::SendResult::kBackpressure ||
            r == proto::SendResult::kTransportError) {
            connected_ = false;
        }
        return false;
    }
    return true;
}

bool TestPattern::sendSmallFull(uint16_t frameId) {
    const auto begin = proto::makeFrameBegin(frameId, proto::FrameType::kFull,
                                             proto::PixelFormat::kRgb565, kDisplayWidth,
                                             kDisplayHeight, 2048);
    if (!begin.has_value() || !send(*begin)) {
        return false;
    }
    struct R {
        uint16_t x, y, w, h;
    };
    const R rects[4] = {{0, 0, 16, 16}, {304, 0, 16, 16}, {0, 224, 16, 16}, {304, 224, 16, 16}};
    for (uint16_t ri = 0; ri < 4; ++ri) {
        const R& r = rects[ri];
        std::vector<uint8_t> px;
        px.reserve(static_cast<size_t>(r.w) * r.h * 2u);
        for (uint16_t y = 0; y < r.h; ++y) {
            for (uint16_t x = 0; x < r.w; ++x) {
                const uint8_t lo = static_cast<uint8_t>(frameId + ri + x);
                const uint8_t hi = static_cast<uint8_t>(frameId + y + 1u);
                px.push_back(lo);
                px.push_back(hi);
            }
        }
        const auto rect = proto::makeFrameRect(r.x, r.y, r.w, r.h, px.data(), px.size());
        if (!rect.has_value() || !send(*rect)) {
            return false;
        }
    }
    const auto end = proto::makeFrameEnd(frameId, 4, 2048, false);
    return send(end);
}

bool TestPattern::sendLargeFull(uint16_t frameId) {
    const auto begin = proto::makeFrameBegin(frameId, proto::FrameType::kFull,
                                             proto::PixelFormat::kRgb565, kDisplayWidth,
                                             kDisplayHeight, 153600);
    if (!begin.has_value() || !send(*begin)) {
        return false;
    }

    const size_t heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint64_t t0 = static_cast<uint64_t>(esp_timer_get_time() / 1000);

    // M1-3C：单 RECT 320x240，Message payload = 8B 头 + 153600B = 153608B，
    // 经 Streaming Encoder 拆 38 个 Packet（37×4096 + 2056，末包 CHUNKED=0）。
    // 不构造 vector<uint8_t>(153608)：RectPatternSource 按需产生字节，
    // 峰值额外内存 = 4096B staging + 4116B 单包缓冲（encodeStreaming）。
    proto::MessageHeader rectHeader;
    rectHeader.type = static_cast<uint8_t>(proto::MessageType::kFrameRect);
    rectHeader.flags = 0;
    RectPatternSource source(frameId, kDisplayWidth, kDisplayHeight);
    const proto::SendResult r = streamingSender_(rectHeader, source);
    const uint64_t elapsedMs = static_cast<uint64_t>(esp_timer_get_time() / 1000) - t0;
    if (r != proto::SendResult::kOk) {
        if (r == proto::SendResult::kNotConnected || r == proto::SendResult::kBackpressure ||
            r == proto::SendResult::kTransportError) {
            connected_ = false;
        }
        return false;
    }

    const proto::Message end = proto::makeFrameEnd(frameId, 1, 153600, false);
    if (!send(end)) {
        return false;
    }
    const size_t heapAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // 堆/带宽统计经 ERROR(0, text) 上报（控制消息，位于 FRAME_END 之后，合法穿插）：
    //   heap_b = RECT 发送前（BEGIN 后）   heap_d = 流式期间低水位
    //   heap_a = END 发送后                rp = RECT 实际包数（确定性）
    //   ms = 流式发送耗时（近似 UART 排空时间，115200 下理论 ≈ 13.3s）
    constexpr uint32_t kRectPackets =
        (kLargeRectPayloadBytes + proto::kMaxPacketPayload - 1u) / proto::kMaxPacketPayload;
    char report[80];
    const int n = std::snprintf(report, sizeof(report), "heap_b=%u,d=%u,a=%u,rp=%u,ms=%llu",
                                static_cast<unsigned>(heapBefore),
                                static_cast<unsigned>(source.minHeapDuring()),
                                static_cast<unsigned>(heapAfter), static_cast<unsigned>(kRectPackets),
                                static_cast<unsigned long long>(elapsedMs));
    if (n > 0 && n < static_cast<int>(sizeof(report))) {
        const auto err = proto::makeError(proto::ErrorCode::kNone,
                                          std::string_view(report, static_cast<size_t>(n)));
        if (err.has_value()) {
            send(*err);
        }
    }
    return true;
}

bool TestPattern::sendPartial(uint16_t frameId, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    const auto begin = proto::makeFrameBegin(frameId, proto::FrameType::kPartial,
                                             proto::PixelFormat::kRgb565, kDisplayWidth,
                                             kDisplayHeight, 0);
    if (!begin.has_value() || !send(*begin)) {
        return false;
    }
    std::vector<uint8_t> px;
    px.reserve(static_cast<size_t>(w) * h * 2u);
    for (uint16_t yy = 0; yy < h; ++yy) {
        for (uint16_t xx = 0; xx < w; ++xx) {
            const uint8_t lo = static_cast<uint8_t>(frameId + xx);  // rectId = 0
            const uint8_t hi = static_cast<uint8_t>(frameId + yy + 1u);
            px.push_back(lo);
            px.push_back(hi);
        }
    }
    const auto rect = proto::makeFrameRect(x, y, w, h, px.data(), px.size());
    if (!rect.has_value() || !send(*rect)) {
        return false;
    }
    const uint32_t byteCount = static_cast<uint32_t>(px.size());
    const auto end = proto::makeFrameEnd(frameId, 1, byteCount, false);
    return send(end);
}

}  // namespace espview
