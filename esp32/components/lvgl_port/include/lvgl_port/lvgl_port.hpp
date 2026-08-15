// ESPView M5-A / M7-C2 — LVGL Port（display driver + flush_cb + TX 任务 + 统计）。
//
// 架构（M5-A 任务书 §四/§六–§十七；M7-C2 增加 DisplayRouter 路由）：
//   LVGL Application → lv_timer_handler() → flush_cb(area, px_map)
//     → DisplayRouter::writeRect()/flush()  → VirtualSink（RemoteDisplay，PC 虚拟）
//                                        └→ PhysicalDisplaySink（OLED，经 Router attach）
//     → TX 任务（pump）→ EndpointSink → ProtocolEndpoint::sendMessageStreaming
//     → UartTransport → COM3 → PC VirtualScreenWidget
//
// flush_cb 生命周期（§七；M7-C2 保留全部既有契约）：
//   px_map 只在 flush_cb 内有效 → 经 Router 同步扇出（VirtualOnly 纯透传零回归；
//   Mirror/Split 下 virtual 背压仍走既有有界等待 + dropPendingFrame→FULL resync）
//   → 有界等待（队列满/上一帧未结束；FULL 帧允许长等=节流，PARTIAL 短等超时=
//   丢弃整帧+FULL resync）→ 无论成败 lv_disp_flush_ready() → 之后不得再访问 px_map。
// 帧边界（§六）：一个 LVGL rendering cycle = 一个 Frame；最后一个 flush
//   （lv_disp_flush_is_last）后调用 Router::flush() → TX 发 FRAME_END。
// M7-C2 背压/降级：物理侧 I2C 失败绝不影响 Virtual sink —— PhysicalDisplaySink
// 只做同步渲染（I2C 上传在 OLED 任务），Router 按 isAvailable() 跳过不可用 sink，
// flush_cb 不被物理侧阻塞（kQueueFull/kFrameBusy 等待只源于 Virtual 路径背压）。
//
// 内存模型（§八/§九）：LVGL draw buffer = 1/10 屏（320x24 RGB565 = 15360B）；
// RemoteDisplay TX 队列 = 2 槽 x 15360B（≈30KB）；packet staging = 4096B
// （encoder 内部）。无整屏 framebuffer。
//
// 依赖：LVGL v8.3（lv_disp_drv_t / lv_disp_flush_ready / lv_disp_flush_is_last）。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "display_manager.h"
#include "display_router.h"     // M7-C2：DisplayRouter（flush_cb 路由 / VirtualSink 接入）
#include "lvgl_adapter.h"
#include "remote_display.h"
#include "protocol_endpoint.h"  // proto::SendResult / Message / MessageHeader / IMessagePayloadSource

namespace espview {

class LvglPort {
public:
    // 与 TestPattern 同构的发送回调（main.cpp 接到 g_endpoint）。
    using Sender = std::function<proto::SendResult(const proto::Message&)>;
    using StreamingSender = std::function<proto::SendResult(
        const proto::MessageHeader&, proto::IMessagePayloadSource&)>;

    LvglPort(Sender sender, StreamingSender streamingSender,
                  std::shared_ptr<display::DisplayRouter> router);
    ~LvglPort();

    // 初始化 LVGL + 注册 display driver + 创建 demo UI + 启动任务（app_main 调用一次）。
    void start();

    // 会话状态：CONNECTED → RemoteDisplay.onConnected() + 全屏置脏（下一帧 FULL）；
    // DISCONNECTED → RemoteDisplay.onDisconnected()（清队列 + 下一帧 FULL）。
    void onSessionState(proto::SessionState s);

    // M7-C2：请求 UI 任务全屏置脏（DisplayRouter full-resync 钩子；模式切换后
    // 下一 LVGL cycle 全量重绘 → 所有启用 sink 收新帧）。
    void requestFullInvalidate() { fullInvalidatePending_.store(true); }

    // 统计上报（ERROR 文本通道：disp / disp2 两行 ≤64B；由 main statsLoop 调用）。
    void reportStats();

    // M5-B：InputManager listener（RX 任务 feed → adapter 状态；LVGL 任务 read_cb 消费）。
    input::IInputListener* inputListener() { return inputAdapter_.get(); }

    // M6-C：flush 等待预算运行时调整（按当前 Transport capabilities 换算；
    // 由 main.cpp 在连接/切换时调用）。
    void setFlushWaitMs(uint64_t fullMs, uint64_t partialMs);

    display::DisplayStats statsSnapshot() const { return remote_->statsSnapshot(); }
    // M6-D 诊断：任务活性 + RemoteDisplay 状态机快照（ERROR 文本通道，非 wire 格式）。
    uint64_t lastUiLoopMs() const { return uiLoopMs_.load(); }
    uint64_t lastFlushCbMs() const { return flushCbMs_.load(); }
    uint64_t lastPumpMs() const { return pumpMs_.load(); }
    uint64_t lastFlushCbExitMs() const { return flushCbExitMs_.load(); }
    uint64_t lastPumpEntryMs() const { return pumpEntryMs_.load(); }
    bool flushCbInProgress() const { return flushCbInProgress_.load(); }
    // M6-D 诊断：任务状态（eTaskGetState：0=Running 1=Ready 2=Blocked 3=Suspended）
    uint8_t uiTaskState() const { return uiTask_ != nullptr ? static_cast<uint8_t>(eTaskGetState(uiTask_)) : 0xFFu; }
    uint8_t txTaskState() const { return txTask_ != nullptr ? static_cast<uint8_t>(eTaskGetState(txTask_)) : 0xFFu; }
    display::RemoteDisplay::DebugState debugState() const { return remote_->debugState(); }

    // ---- Heap 验收（§24）----
    size_t heapBeforeLvgl() const { return heapBefore_; }      // LVGL 初始化前 free heap
    size_t heapAfterLvgl() const { return heapAfter_; }        // LVGL 初始化后 free heap
    size_t heapMin() const { return heapMin_.load(); }         // 运行期最低 free heap
    size_t largestBlockBefore() const { return largestBefore_; }
    size_t largestBlockAfter() const { return largestAfter_; }
    size_t drawBufferBytes() const { return kDrawBufferBytes; }
    size_t queueBytes() const { return kQueueSlots * kSlotPixelBytes; }

    // ---- 内存模型常量（公共：静态 draw buffer 与报告使用）----
public:
    static constexpr uint16_t kWidth = 320;
    static constexpr uint16_t kHeight = 240;
    static constexpr size_t kDrawBufPixels = kWidth * 24u;         // 1/10 屏
    static constexpr size_t kDrawBufferBytes = kDrawBufPixels * 2u;
    static constexpr size_t kQueueSlots = 2;
    static constexpr size_t kSlotPixelBytes = kDrawBufferBytes;
    // M6-C §十二：flush 等待预算默认值（UART 115200 节流基线）。
    // TCP（unpaced）运行时由 main.cpp 按 Transport capabilities 换算为 250ms
    // 并经 setFlushWaitMs() 调整（LAN 大帧远快于此；超时=整帧丢弃+FULL resync）。
    static constexpr uint64_t kFullFlushWaitMsDefault = 3000;
    static constexpr uint64_t kPartialFlushWaitMsDefault = 3000;

private:
    static void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p);
    static void uiTaskEntry(void* arg);
    static void txTaskEntry(void* arg);
    void uiLoop();
    void txLoop();
    uint64_t nowMs() const;
    uint64_t flushWaitMs() const;

    Sender sender_;
    StreamingSender streamingSender_;
    // EndpointSink 必须保持存活：RemoteDisplay 保存对它的引用（
    // 构造函数局部 shared_ptr 会被销毁→ dangling reference）
    std::shared_ptr<display::IFrameSink> sink_;
    std::unique_ptr<display::DisplayManager> displayMgr_;
    std::shared_ptr<display::RemoteDisplay> remote_;
    // M7-C2：DisplayRouter（main 组装并 attach physical sink；本类 attach virtual）。
    std::shared_ptr<display::DisplayRouter> router_;
    // VirtualSink 适配器（IDisplaySink 包装 RemoteDisplay；定义在 lvgl_port.cpp）。
    std::shared_ptr<display::IDisplaySink> virtualSink_;

    lv_disp_draw_buf_t drawBuf_{};
    lv_disp_drv_t dispDrv_{};
    std::unique_ptr<input::LvglInputAdapter> inputAdapter_;
    class LvglDemoApp* demo_ = nullptr;

    SemaphoreHandle_t slotFreeSem_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> fullInvalidatePending_{false};  // CONNECTED 后由 UI 任务全屏置脏
    TaskHandle_t uiTask_ = nullptr;
    TaskHandle_t txTask_ = nullptr;

    // reportStats FPS/间隔统计与 LVGL tick 增量
    uint64_t lastStatsMs_ = 0;
    uint64_t lastFramesFull_ = 0;
    uint64_t lastFramesPartial_ = 0;
    uint64_t lastTickMs_ = 0;

    // Heap 验收
    size_t heapBefore_ = 0;
    size_t heapAfter_ = 0;
    size_t largestBefore_ = 0;
    size_t largestAfter_ = 0;
    std::atomic<size_t> heapMin_{0};

    // flush 等待预算（原子：UI 任务读，main 任务写）。
    std::atomic<uint64_t> fullFlushWaitMs_{kFullFlushWaitMsDefault};
    // M6-D 诊断活性（ERROR 文本通道观测；原子自增/时间戳）。
    std::atomic<uint64_t> uiLoopMs_{0};
    std::atomic<uint64_t> flushCbMs_{0};
    std::atomic<uint64_t> pumpMs_{0};
    std::atomic<uint64_t> flushCbExitMs_{0};
    std::atomic<uint64_t> pumpEntryMs_{0};
    std::atomic<bool> flushCbInProgress_{false};
    std::atomic<uint64_t> partialFlushWaitMs_{kPartialFlushWaitMsDefault};
};

}  // namespace espview
