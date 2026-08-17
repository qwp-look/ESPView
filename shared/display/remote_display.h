// ESPView — RemoteDisplay（M5-A：LVGL → 协议帧 的 writeRect/dirty-rect 汇入后端）
//
// 规范来源：docs/DESIGN.md D.1/D.2（IDisplay/DisplayManager）、E 节（帧消息
// Layout / PARTIAL 语义）、M5-A 任务书（§五–§十七）。
//
// 职责边界：
//   - 接收 Application/LVGL 的 writeRect()/flush()，维护「帧边界 + FULL/PARTIAL
//     策略 + 有界 staging 队列 + 帧统计」，经 IFrameSink 产出 FRAME_BEGIN /
//     FRAME_RECT / FRAME_END（ABORTED）消息序列；
//   - 不持有整屏 framebuffer：像素只经有界队列槽拷贝（packet-sized staging +
//     bounded TX queue + rect metadata，DESIGN.md J 节）；
//   - 不实现 Transport/UART/编码细节（由 IFrameSink 实现方提供）。
//
// 帧边界（LVGL rendering cycle = 一个 Frame）：
//   writeRect() 在无开放帧时开始新帧（TX 发送 FRAME_BEGIN）；
//   flush() 标记帧结束（TX 在队列排空后发送 FRAME_END）。
// 帧类型策略（M5-A 任务书 §十一/§十五）：
//   - 首次显示 / 断线重连 / 整帧丢弃后 → FULL（语义自包含，建立 PC 基准）；
//   - 其余 → PARTIAL（PC 只更新 dirty rect）；
//   - PARTIAL 帧无基准时由接收端（FrameAssembler）拒绝 —— 本层不重复实现。
// 背压（§十六/§十七）：
//   writeRect() 非阻塞（队列满 → kQueueFull，不改变帧状态）；调用方（LVGL
//   flush_cb）可有界等待后重试，超时后调用 dropPendingFrame() 丢弃整帧并标记
//   FULL resync；TX 以 FRAME_END(ABORTED) 作废已发出 BEGIN 的帧，绝不让 PC
//   把半帧当成完整画面。flush_ready 语义由上层（lvgl_port）保证。
//
// 线程模型：writeRect()/flush()/dropPendingFrame() 由 UI/LVGL 任务调用；
// pump() 由独立 TX 任务调用（阻塞至该条目发送完成）。内部互斥保证一致性；
// 阻塞发送期间不持有锁（槽位数据由槽 buffer 私有持有）。
// 纯 C++17，零平台依赖。错误路径不使用异常。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "display.h"
#include "encoder.h"          // proto::IMessagePayloadSource
#include "message.h"          // proto::Message / makeFrame*
#include "protocol.h"         // proto::MessageType / FrameType / PixelFormat
#include "protocol_endpoint.h"  // proto::SendResult

namespace espview {
namespace display {

// 帧发送适配（M5-A）：RemoteDisplay 不直接依赖 Transport / ProtocolEndpoint。
//  - ESP32 侧实现：ProtocolEndpoint::sendMessage / sendMessageStreaming；
//  - host 测试侧实现：RecordingSink（真实 MessageEncoder 编码并收集 packet bytes）。
class IFrameSink {
public:
    virtual ~IFrameSink() = default;
    virtual proto::SendResult send(const proto::Message& msg) = 0;
    virtual proto::SendResult sendStreaming(const proto::MessageHeader& header,
                                            proto::IMessagePayloadSource& source) = 0;
};

// 帧统计（M5-A 任务书 §23：frameId/frameType/rectCount/payloadBytes/elapsed/FPS）。
// 纯统计，不改变协议。dirty ratio = partialBytes / fullScreenBytes（见 §31）。
struct DisplayStats {
    uint64_t framesFull = 0;         // 已完整发送（END 发出）的 FULL 帧
    uint64_t framesPartial = 0;      // 已完整发送的 PARTIAL 帧
    uint64_t framesDropped = 0;      // 整帧丢弃（背压超时 / 断线 / 作废）
    uint64_t queueFullEvents = 0;    // writeRect/flush 入队背压次数
    uint64_t queuePeak = 0;          // 队列占用峰值（writeRect 入队路径采样；TX 排空后回落）
    uint64_t rectsSent = 0;          // 已入 wire 的 RECT 数
    uint64_t rectsDropped = 0;       // 因帧作废被丢弃的 RECT 数
    uint64_t fullPixelBytes = 0;     // FULL 帧像素字节（wire 实际发送）
    uint64_t partialPixelBytes = 0;  // PARTIAL 帧像素字节（wire 实际发送）
    uint16_t lastFrameId = 0;
    proto::FrameType lastFrameType = proto::FrameType::kFull;
    uint32_t lastRectCount = 0;
    uint32_t lastFrameBytes = 0;     // 最近一帧像素字节
    uint64_t lastFrameElapsedMs = 0; // 最近一帧 BEGIN→END 发送耗时（TX 侧）
};

class RemoteDisplay : public IDisplay {
public:
    // 配置：DisplayConfig（分辨率/格式）+ 有界 staging 队列参数。
    // 默认：320x240 RGB565；2 槽 × 15360B（= 1/10 屏 320x24，一个 LVGL flush 区域）。
    struct Config : public DisplayConfig {
        size_t queueSlots = 2;                 // 矩形槽数量（bounded TX queue）
        size_t slotPixelBytes = 320u * 24u * 2u;  // 单槽像素容量（1/10 屏 band）
    };

    // nowMs：单调毫秒时钟（统计用）。默认 steady_clock；ESP32 注入 esp_timer。
    explicit RemoteDisplay(IFrameSink& sink, Config cfg);
    ~RemoteDisplay() override = default;

    // ---- IDisplay ----
    DisplayStatus init(const DisplayConfig& cfg) override;
    const DisplayInfo& info() const override { return info_; }
    // 非阻塞：成功入队返回 kOk；背压返回 kQueueFull（不改变帧状态，调用方可
    // 有界等待后重试；超时后调用 dropPendingFrame()）。参数非法返回对应错误码。
    DisplayStatus writeRect(int x, int y, int w, int h, const uint8_t* pixels) override;
    // 帧结束（LVGL is_last / rendering cycle 结束）。无开放帧时 no-op。
    DisplayStatus flush() override;
    DisplayStatus setEnabled(bool enabled) override;

    // ---- 会话状态（M4 语义：断线/重连 → 下一帧 FULL）----
    void onConnected();
    void onDisconnected();

    // ---- TX 任务驱动 ----
    // 处理队列中一个条目（可能发送 BEGIN / RECT / END / ABORTED END，或丢弃
    // 已作废帧的条目）。阻塞至该条目发送完成（send/sendStreaming 语义由 sink
    // 决定）。返回 false = 当前无待发数据（队列空且无待发 END）。
    bool pump();

    // ---- 背压 / 帧策略查询（lvgl flush_cb 等待循环用）----
    bool hasFreeSlot() const;
    size_t queuedCount() const;
    // 是否有帧正在构建或尚未结束（wire 上 BEGIN 已发 / END 未发）。
    bool frameInFlight() const;
    // 当前/下一帧类型（决定 flush_cb 等待预算：FULL 允许长等，PARTIAL 短等）。
    proto::FrameType nextFrameType() const;
    // 调用方在 kQueueFull/kFrameBusy 超时后调用：作废当前帧（TX 发 ABORTED
    // END）+ 标记 FULL resync。无开放帧时仅置 FULL resync。幂等。
    void dropPendingFrame();

    // 调试快照（M6-D 故障诊断：状态机内部可见性；纯追加，不影响协议/行为）。
    struct DebugState {
        bool connected = false;
        bool building = false;
        bool endQueued = false;
        uint16_t frameId = 0;
        uint8_t frameType = 0;      // proto::FrameType
        uint32_t rectCount = 0;
        uint64_t byteCount = 0;
        bool abortPending = false;
        uint16_t abortFrameId = 0;
        bool wireOpen = false;
        uint16_t wireFrameId = 0;
        size_t queued = 0;
        uint32_t queueFullEvents = 0;
        uint64_t framesDropped = 0;
    };
    DebugState debugState() const;
    // ---- 统计 ----
    DisplayStats statsSnapshot() const;
    uint32_t fullScreenBytes() const {
        return static_cast<uint32_t>(info_.width) * static_cast<uint32_t>(info_.height) * 2u;
    }

    // 槽位释放通知（可空）：泵/清队列释放槽时回调（锁外），lvgl_port 用它
    // 唤醒 flush_cb 等待者（FreeRTOS semaphore）。
    void setSlotFreedCallback(std::function<void()> cb);

    // 测试/诊断：注入时钟。
    void setClock(std::function<uint64_t()> nowMs);

private:
    struct Slot {
        bool inUse = false;
        bool isFirst = false;   // 本帧第一个矩形（TX 发送 FRAME_BEGIN）
        proto::FrameType frameType = proto::FrameType::kFull;  // 本帧类型（入队时固化）
        uint16_t frameId = 0;
        uint16_t x = 0, y = 0, w = 0, h = 0;
        size_t pixelBytes = 0;
        std::vector<uint8_t> pixels;  // 预分配 slotPixelBytes
    };

    bool valid() const { return info_.width > 0 && info_.height > 0; }
    bool hasFreeSlotLocked() const { return count_ < slots_.size(); }
    void enqueueLocked(const Slot& s);
    void popFrontLocked();
    void freeAllLocked();
    void notifySlotFreed();
    void finalizeAbortLocked();  // 清 abort/building/endQueued/wireOpen 状态（锁内调用）
    void updateLastFrameLocked(proto::FrameType t, uint32_t rectCount, uint32_t bytes,
                               uint64_t elapsedMs);

    IFrameSink& sink_;
    Config cfg_;
    DisplayInfo info_;
    bool enabled_ = true;
    bool connected_ = false;
    bool needFull_ = true;      // 下一帧必须 FULL（首次/重连/丢弃后）

    mutable std::mutex mutex_;  // const 查询方法（queuedCount/nextFrameType/stats）也需加锁
    std::vector<Slot> slots_;   // 环形队列（预分配）
    size_t head_ = 0;           // 队列头索引
    size_t count_ = 0;          // 队列中条目数

    // UI 侧帧状态（mutex_ 保护）
    bool building_ = false;     // 正在构建帧（首个 writeRect 之后、flush 之前）
    bool endQueued_ = false;    // flush() 已调用（TX 在队列排空后发 FRAME_END）
    uint16_t frameId_ = 0;      // 当前帧 id（新帧开始时 +1）
    proto::FrameType frameType_ = proto::FrameType::kFull;  // 当前帧类型
    uint32_t rectCount_ = 0;    // 当前帧已入队矩形数
    uint64_t byteCount_ = 0;    // 当前帧已入队像素字节
    std::optional<uint16_t> abortPending_;  // 待作废帧 id（dropPendingFrame 设置）

    // TX 侧 wire 状态（mutex_ 保护；仅 pump 修改）
    bool wireOpen_ = false;
    uint16_t wireFrameId_ = 0;
    proto::FrameType wireType_ = proto::FrameType::kFull;
    uint32_t wireRectCount_ = 0;
    uint64_t wireByteCount_ = 0;
    uint64_t wireBeginMs_ = 0;

    DisplayStats stats_;
    std::function<uint64_t()> nowMs_;
    std::function<void()> slotFreedCb_;
};

}  // namespace display
}  // namespace espview
