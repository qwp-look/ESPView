// ESPView M5-A — LVGL Port 实现（display driver + flush_cb + TX 任务 + 统计）。
//
// 规范来源：docs/DESIGN.md §31（M5 implementation semantics）+ M5-A 任务书。
// 线程模型：
//   - UI 任务（lvgl_ui, prio 4）：周期调用 lv_timer_handler()；渲染 cycle 内
//     flush_cb 在本任务执行（有界等待节流，绝不 busy-wait）；
//   - TX 任务（lvgl_tx, prio 4）：remote_->pump() 阻塞发送（每 RECT 持有
//     endpoint sendMutex_ ~1.4s @115200）；发送完成释放槽位 → 通知 flush_cb；
//   - 会话状态（main sessionLoop / RX 任务）→ onSessionState()：只置原子标志与
//     RemoteDisplay 状态，绝不在 LVGL 任务之外触碰 lv_obj（线程安全）。
// 日志：协议 UART 只承载协议字节；统计经 ERROR 消息文本通道（reportStats）。

#include "lvgl_port/lvgl_port.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "lvgl_port/lvgl_app.hpp"
#include "lvgl_port/lvgl_indev.hpp"
#include "message.h"
#include "protocol.h"

namespace espview {

namespace {

// LVGL 显示配置：RGB565（LV_COLOR_DEPTH=16），320x240。
// draw buffer：1/10 屏（320x24 = 7680 像素 = 15360B），静态 .bss（不占堆，
// 避免经典 ESP32 大连续堆分配）。单缓冲：LVGL 等 flush_ready → 自然节流。
alignas(4) lv_color_t s_drawBuf[LvglPort::kDrawBufPixels] = {};

// EndpointSink：RemoteDisplay → ProtocolEndpoint（sendMessage / sendMessageStreaming）。
class EndpointSink : public display::IFrameSink {
public:
    EndpointSink(LvglPort::Sender sender, LvglPort::StreamingSender streaming)
        : send_(std::move(sender)), stream_(std::move(streaming)) {}

    proto::SendResult send(const proto::Message& msg) override { return send_(msg); }
    proto::SendResult sendStreaming(const proto::MessageHeader& header,
                                    proto::IMessagePayloadSource& source) override {
        return stream_(header, source);
    }

private:
    LvglPort::Sender send_;
    LvglPort::StreamingSender stream_;
};

// M7-C2：VirtualSink —— IDisplaySink 适配 RemoteDisplay（LVGL flush_cb →
// DisplayRouter → VirtualSink → RemoteDisplay，保持既有 writeRect/flush 契约
// 不变）。isAvailable → RemoteDisplay connected（transport 会话）；status →
// debugState 派生。lastPresentStatus() 供 flush_cb 在 Mirror/Split 下识别
// Virtual 路径背压（Router 聚合结果可能被物理侧接受掩盖，见 flushCb）。
class VirtualSink : public display::IDisplaySink {
public:
    explicit VirtualSink(std::shared_ptr<display::RemoteDisplay> remote)
        : remote_(std::move(remote)) {}

    display::DisplayStatus init(const display::DisplayCapabilities& caps) override {
        // 校验生产者能力：v0.1 仅 RGB565；分辨率必须为正。
        if (caps.format != proto::PixelFormat::kRgb565) {
            return display::DisplayStatus::kNotSupported;
        }
        if (caps.width <= 0 || caps.height <= 0) {
            return display::DisplayStatus::kInvalidParam;
        }
        caps_ = caps;
        caps_.canReadback = true;
        caps_.sinkKind = display::DisplaySinkKind::kVirtual;
        return display::DisplayStatus::kOk;
    }

    const display::DisplayCapabilities& capabilities() const override {
        return caps_;
    }

    display::DisplayStatus present(const display::Rect& rect,
                                   const uint8_t* pixels) override {
        const display::DisplayStatus s =
            remote_->writeRect(rect.x, rect.y, rect.w, rect.h, pixels);
        lastPresent_ = s;
        return s;
    }

    display::DisplayStatus flush() override { return remote_->flush(); }
    display::DisplayStatus setEnabled(bool enabled) override {
        return remote_->setEnabled(enabled);
    }
    bool isAvailable() const override { return remote_->debugState().connected; }
    display::DisplayStatus status() const override {
        return remote_->debugState().connected ? display::DisplayStatus::kOk
                                               : display::DisplayStatus::kNotConnected;
    }

    // flush_cb 专用：最近一次 present() 的 Virtual 路径结果（初始 kOk）。
    display::DisplayStatus lastPresentStatus() const { return lastPresent_; }

private:
    std::shared_ptr<display::RemoteDisplay> remote_;
    display::DisplayCapabilities caps_;
    display::DisplayStatus lastPresent_ = display::DisplayStatus::kOk;
};

}  // namespace

LvglPort::LvglPort(Sender sender, StreamingSender streamingSender,
                   std::shared_ptr<display::DisplayRouter> router)
    : sender_(std::move(sender)),
      streamingSender_(std::move(streamingSender)),
      router_(std::move(router)) {
    // RemoteDisplay + DisplayManager（编译期 WINDOW 模式）。
    sink_ = std::make_shared<EndpointSink>(sender_, streamingSender_);
    display::RemoteDisplay::Config cfg;
    cfg.width = kWidth;
    cfg.height = kHeight;
    cfg.format = proto::PixelFormat::kRgb565;
    cfg.queueSlots = kQueueSlots;
    cfg.slotPixelBytes = kSlotPixelBytes;
    remote_ = std::make_shared<display::RemoteDisplay>(*sink_, cfg);
    remote_->setClock([]() -> uint64_t {
        return static_cast<uint64_t>(esp_timer_get_time() / 1000);
    });
    displayMgr_ = std::make_unique<display::DisplayManager>();
    displayMgr_->addBackend(remote_);

    // M7-C2：VirtualSink 适配 RemoteDisplay 并接入 DisplayRouter（main 组装
    // physical sink 后 setMode；VirtualOnly 纯透传零回归）。init 在 attach 前
    // 完成（display_sink.h 约定）。
    virtualSink_ = std::make_shared<VirtualSink>(remote_);
    display::DisplayCapabilities caps;
    caps.width = kWidth;
    caps.height = kHeight;
    caps.format = proto::PixelFormat::kRgb565;
    caps.color = 16;
    caps.mono = false;
    caps.canReadback = true;
    caps.sinkKind = display::DisplaySinkKind::kVirtual;
    (void)virtualSink_->init(caps);
    if (router_) {
        router_->attachVirtual(virtualSink_);
    }

    slotFreeSem_ = xSemaphoreCreateBinary();
    inputAdapter_ = std::make_unique<input::LvglInputAdapter>(kWidth, kHeight);
    demo_ = new LvglDemoApp(inputAdapter_.get());
}

LvglPort::~LvglPort() {
    running_ = false;
    if (uiTask_ != nullptr) {
        vTaskDelete(uiTask_);
        uiTask_ = nullptr;
    }
    if (txTask_ != nullptr) {
        vTaskDelete(txTask_);
        txTask_ = nullptr;
    }
    if (slotFreeSem_ != nullptr) {
        vSemaphoreDelete(slotFreeSem_);
        slotFreeSem_ = nullptr;
    }
    delete demo_;
    demo_ = nullptr;
}

void LvglPort::start() {
    heapBefore_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    largestBefore_ = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    heapMin_.store(heapBefore_);
    lv_init();

    lv_disp_draw_buf_init(&drawBuf_, s_drawBuf, nullptr, kDrawBufPixels);
    lv_disp_drv_init(&dispDrv_);
    dispDrv_.hor_res = kWidth;
    dispDrv_.ver_res = kHeight;
    dispDrv_.flush_cb = &LvglPort::flushCb;
    dispDrv_.draw_buf = &drawBuf_;
    dispDrv_.user_data = this;
    lv_disp_t* disp = lv_disp_drv_register(&dispDrv_);

    demo_->create();

    // M5-B：注册 POINTER / KEYPAD / ENCODER 三个 indev（读同一 inputAdapter_）。
    lvglIndevRegister(disp, inputAdapter_.get(), demo_->group());

    heapAfter_ = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    largestAfter_ = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (heapAfter_ < heapMin_.load()) {
        heapMin_.store(heapAfter_);
    }

    running_ = true;
    xTaskCreate(uiTaskEntry, "lvgl_ui", 4096, this, 4, &uiTask_);
    xTaskCreate(txTaskEntry, "lvgl_tx", 4096, this, 4, &txTask_);
}

void LvglPort::onSessionState(proto::SessionState s) {
    if (s == proto::SessionState::kConnected) {
        remote_->onConnected();
        fullInvalidatePending_.store(true);  // UI 任务内全屏置脏 → 下一帧 FULL
    } else if (s == proto::SessionState::kDisconnected) {
        fullInvalidatePending_.store(false);
        remote_->onDisconnected();
        inputAdapter_->reset();  // M5-B：清空 pointer/key/wheel，避免 LVGL 看到旧 pressed 状态
    }
}

uint64_t LvglPort::nowMs() const {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

uint64_t LvglPort::flushWaitMs() const {
    return remote_->nextFrameType() == proto::FrameType::kFull ? fullFlushWaitMs_.load()
                                                               : partialFlushWaitMs_.load();
}

void LvglPort::setFlushWaitMs(uint64_t fullMs, uint64_t partialMs) {
    fullFlushWaitMs_.store(fullMs);
    partialFlushWaitMs_.store(partialMs);
}

// ---- flush_cb：LVGL rendering cycle 内的每次区域刷新 ----
void LvglPort::flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_p) {
    auto* self = static_cast<LvglPort*>(drv->user_data);
    self->flushCbMs_.store(self->nowMs());  // M6-D 诊断：flush_cb 活性
    self->flushCbInProgress_.store(true);
    const int x = area->x1;
    const int y = area->y1;
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    const uint8_t* px = reinterpret_cast<const uint8_t*>(color_p);

    // 1) 非阻塞入队：经 Router 扇出（同步拷贝 px_map → 槽 buffer / 物理渲染；
    //    VirtualOnly 纯透传零回归）。物理侧只做同步渲染（I2C 上传在 OLED 任务），
    //    flush_cb 不被物理侧阻塞 —— kQueueFull/kFrameBusy 等待只源于 Virtual 路径。
    display::DisplayStatus st;
    if (self->router_) {
        st = self->router_->writeRect(display::Rect{x, y, w, h}, px);
        // Mirror/Split 下 Router 聚合结果可能被物理侧接受掩盖 Virtual 背压：
        // 若 Virtual 路径本轮确实背压，恢复既有等待/丢弃契约（VirtualOnly 时
        // 两者等价；PhysicalOnly 时 Virtual 不在路径内，保持 kOk）。
        if (self->router_->mode() != display::DisplayRouteMode::kPhysicalOnly) {
            auto* vs = static_cast<VirtualSink*>(self->virtualSink_.get());
            if (vs != nullptr) {
                const display::DisplayStatus vsSt = vs->lastPresentStatus();
                if ((vsSt == display::DisplayStatus::kQueueFull ||
                     vsSt == display::DisplayStatus::kFrameBusy) &&
                    st == display::DisplayStatus::kOk) {
                    st = vsSt;
                }
            }
        }
    } else {
        st = self->remote_->writeRect(x, y, w, h, px);
    }

    // 2) 背压：有界等待队列空间/上一帧结束（FULL 帧允许长等 = UART 节流；
    //    PARTIAL 帧短等，超时 → 丢弃整帧 + FULL resync）。
    if (st == display::DisplayStatus::kQueueFull ||
        st == display::DisplayStatus::kFrameBusy) {
        const uint64_t deadline = self->nowMs() + self->flushWaitMs();
        while ((st == display::DisplayStatus::kQueueFull ||
                st == display::DisplayStatus::kFrameBusy) &&
               self->nowMs() < deadline) {
            if (self->slotFreeSem_ != nullptr) {
                xSemaphoreTake(self->slotFreeSem_, pdMS_TO_TICKS(10));
            }
            if (self->router_) {
                st = self->router_->writeRect(display::Rect{x, y, w, h}, px);
            } else {
                st = self->remote_->writeRect(x, y, w, h, px);
            }
        }
        if (st == display::DisplayStatus::kQueueFull ||
            st == display::DisplayStatus::kFrameBusy) {
            self->remote_->dropPendingFrame();  // 整帧丢弃（TX 发 ABORTED END）
            // M6-D 修正：FULL/PARTIAL 整帧丢弃后置 fullInvalidatePending_ —— 静态 UI（无输入）
            // 不会自发重绘，否则 needFull 重同步永远无法触发 → PC 30s 看门狗（UART→TCP 切换
            // 失败后遗症）。下一 UI cycle 全屏置脏 → 重新渲染 FULL resync。
            self->fullInvalidatePending_.store(true);
        }
    }

    // 3) 帧边界：最后一次 flush → flush()（TX 在队列排空后发 FRAME_END）。
    if (st == display::DisplayStatus::kOk && lv_disp_flush_is_last(drv)) {
        if (self->router_) {
            self->router_->flush();
        } else {
            self->remote_->flush();
        }
    }

    // 4) 无论成功/背压/丢弃：必须调用 flush_ready（否则 LVGL 永久停在当前 cycle）。
    lv_disp_flush_ready(drv);
    self->flushCbExitMs_.store(self->nowMs());  // M6-D 诊断：flush_cb 返回时刻
    self->flushCbInProgress_.store(false);
}

// ---- UI 任务 ----
void LvglPort::uiTaskEntry(void* arg) {
    auto* self = static_cast<LvglPort*>(arg);
    self->uiLoop();
    vTaskDelete(nullptr);
}

void LvglPort::uiLoop() {
    const uint64_t startMs = nowMs();
    lastTickMs_ = startMs;
    while (running_.load()) {
        // LVGL tick 推进：实测 delta（计时器/animation 依赖，
        // 不使用 LV_TICK_CUSTOM，避免 ESP-IDF 配置缺失）
        const uint64_t now = nowMs();
        uiLoopMs_.store(now);  // M6-D 诊断：uiLoop 活性
        const uint64_t delta = now >= lastTickMs_ ? now - lastTickMs_ : 1;
        if (delta > 0) {
            lv_tick_inc(static_cast<uint32_t>(delta));
        }
        lastTickMs_ = now;
        if (fullInvalidatePending_.exchange(false)) {
            lv_obj_invalidate(lv_scr_act());  // 重连/连接后全屏置脏 → 下一帧 FULL
        }
        lv_timer_handler();
        // 运行期最低堆采样
        const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t min = heapMin_.load();
        while (freeNow < min && !heapMin_.compare_exchange_weak(min, freeNow)) {
        }
        // CONFIG_FREERTOS_HZ=100 时 pdMS_TO_TICKS(5)=0（忙轮）：用 10ms=1 tick
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---- TX 任务：排空 RemoteDisplay 队列（阻塞发送，节流到 UART 速率）----
void LvglPort::txTaskEntry(void* arg) {
    auto* self = static_cast<LvglPort*>(arg);
    self->txLoop();
    vTaskDelete(nullptr);
}

void LvglPort::txLoop() {
    while (running_.load()) {
        pumpEntryMs_.store(nowMs());  // M6-D 诊断：pump 进入时刻
        if (remote_->pump()) {
            pumpMs_.store(nowMs());  // M6-D 诊断：TX pump 活性
            // 槽位已释放（发送/丢弃完成）→ 唤醒 flush_cb 等待者。
            if (slotFreeSem_ != nullptr) {
                xSemaphoreGive(slotFreeSem_);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));  // 100Hz tick 下 1 tick=10ms
        }
    }
}

// ---- 统计上报（ERROR 文本通道，≤64B/行；wire format 未修改）----
//   disp  id= frameType= rects= bytes= elapsedMs= fps= d=drop q=queueFull
//   disp2 full= partial= dirty%= hb=ha=hm= lb= dw= qb=
void LvglPort::reportStats() {
    const display::DisplayStats st = remote_->statsSnapshot();
    const uint64_t now = nowMs();
    const uint64_t dt = now > lastStatsMs_ ? now - lastStatsMs_ : 1;
    // FPS 固定点 0.01fps（115200 下 PARTIAL ≈0.4fps，整数截断会恒为 0）。
    const uint64_t fpsHundredths = (st.framesFull + st.framesPartial - lastFramesFull_ -
                                    lastFramesPartial_) *
                                   100000u / dt;
    lastStatsMs_ = now;
    lastFramesFull_ = st.framesFull;
    lastFramesPartial_ = st.framesPartial;

    const uint32_t fullScreen = static_cast<uint32_t>(kWidth) * kHeight * 2u;
    const uint32_t dirtyPct = fullScreen == 0 ? 0u
        : static_cast<uint32_t>((st.partialPixelBytes * 10000u) / fullScreen);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "disp id=%u t=%u r=%u b=%u e=%llu f=%u.%02u d=%llu q=%llu",
                  static_cast<unsigned>(st.lastFrameId),
                  static_cast<unsigned>(st.lastFrameType),
                  static_cast<unsigned>(st.lastRectCount),
                  static_cast<unsigned>(st.lastFrameBytes),
                  static_cast<unsigned long long>(st.lastFrameElapsedMs),
                  static_cast<unsigned>(fpsHundredths / 100u),
                  static_cast<unsigned>(fpsHundredths % 100u),
                  static_cast<unsigned long long>(st.framesDropped),
                  static_cast<unsigned long long>(st.queueFullEvents));
    const auto m1 = proto::makeError(proto::ErrorCode::kNone, buf);
    if (m1.has_value()) {
        sender_(*m1);
    }

    // ERROR 文本每行≤64B：值全部 clamp 到 999999（-Werror=format-truncation 算法能界定）
    const auto clampU32 = [](uint64_t v) -> unsigned {
        return v < 999999u ? static_cast<uint32_t>(v) : 999999u;
    };
    std::snprintf(buf, sizeof(buf), "disp2 full=%u part=%u d=%u.%02u",
                  clampU32(st.fullPixelBytes), clampU32(st.partialPixelBytes),
                  static_cast<unsigned>(dirtyPct / 100u), static_cast<unsigned>(dirtyPct % 100u));
    const auto m2 = proto::makeError(proto::ErrorCode::kNone, buf);
    if (m2.has_value()) {
        sender_(*m2);
    }
    // M6-C：加入队列占用峰值 qp=（bounded TX queue 深度观测；≤64B/行）。
    // 移除 lb=/qb=（常量诊断），保持 heap 6 位 clamp 精度。
    std::snprintf(buf, sizeof(buf), "disp3 hb=%u ha=%u hm=%u dw=%u qp=%u",
                  clampU32(heapBefore_), clampU32(heapAfter_), clampU32(heapMin_.load()),
                  static_cast<unsigned>(kDrawBufferBytes),
                  clampU32(st.queuePeak));
    const auto m3 = proto::makeError(proto::ErrorCode::kNone, buf);
    if (m3.has_value()) {
        sender_(*m3);
    }
    // M5-B：输入适配器统计（ERROR 文本通道，wire format 未修改）。
    //   inp3 w=wheelEvents s=wheelSteps(已消费) k=consumedKeys d=keyQueueDropped
    //        u=unmappedKeys b=ignoredButtons r=resets
    const input::LvglAdapterStats in = inputAdapter_->stats();
    const auto clampU16 = [](uint64_t v) -> unsigned {
        return v < 9999u ? static_cast<uint32_t>(v) : 9999u;
    };
    std::snprintf(buf, sizeof(buf), "inp3 w=%u s=%d k=%u d=%u u=%u b=%u r=%u",
                  clampU16(in.wheelEvents),
                  static_cast<int>(in.wheelSteps < 0 ? (in.wheelSteps > -9999 ? in.wheelSteps : -9999)
                                                     : (in.wheelSteps < 9999 ? in.wheelSteps : 9999)),
                  clampU16(in.consumedKeys), clampU16(in.keyQueueDropped),
                  clampU16(in.unmappedKeys), clampU16(in.ignoredButtons), clampU16(in.resets));
    const auto m4 = proto::makeError(proto::ErrorCode::kNone, buf);
    if (m4.has_value()) {
        sender_(*m4);
    }

    // M6-D 诊断：LVGL/TX 任务活性 + RemoteDisplay 状态机（ERROR 文本通道，非 wire 格式）。
    //   dbg u=uiLoop秒 f=flush_cb秒 p=pump秒 q=queued b=building e=endQueued
    //       a=abortPending w=wireOpen c=connected n=nextType(F/P)
    const display::RemoteDisplay::DebugState dbg = remote_->debugState();
    const auto clampDbg = [](uint64_t v) -> unsigned { return v < 999999u ? static_cast<unsigned>(v) : 999999u; };
    std::snprintf(buf, sizeof(buf), "dbg u=%u f=%u p=%u b%d c%d i%d tu%d tt%d",
                  clampDbg(now / 1000), clampDbg(flushCbMs_.load() / 1000),
                  clampDbg(pumpMs_.load() / 1000),
                  dbg.building ? 1 : 0, dbg.connected ? 1 : 0,
                  flushCbInProgress_.load() ? 1 : 0, uiTaskState(), txTaskState());
    const auto m5 = proto::makeError(proto::ErrorCode::kNone, buf);
    if (m5.has_value()) {
        sender_(*m5);
    }

}

}  // namespace espview
