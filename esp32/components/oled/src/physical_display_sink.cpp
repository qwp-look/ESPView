// ESPView M7-C2 — PhysicalDisplaySink 实现。语义见 physical_display_sink.hpp。
#include "oled/physical_display_sink.hpp"

#include "oled/oled_display.hpp"

namespace espview {
namespace oled {

namespace {
// display::PhysicalScene → OledDisplay::Scene 显式映射（数值一致，但禁止依赖
// 数值相等；未知值一律落回 Diagnostics）。
OledDisplay::Scene toDisplayScene(display::PhysicalScene scene) {
    return scene == display::PhysicalScene::kApplication
               ? OledDisplay::Scene::kApplication
               : OledDisplay::Scene::kDiagnostics;
}
}  // namespace

PhysicalDisplaySink::PhysicalDisplaySink(OledDisplay* display)
    : display_(display) {}

display::DisplayStatus PhysicalDisplaySink::init(
    const display::DisplayCapabilities& caps) {
    // 生产者能力校验：v0.1 仅 RGB565 源帧；分辨率必须为正。
    // 失败不改变自身状态（display_sink.h 约定）。
    if (caps.format != proto::PixelFormat::kRgb565) {
        return display::DisplayStatus::kNotSupported;
    }
    if (caps.width <= 0 || caps.height <= 0) {
        return display::DisplayStatus::kInvalidParam;
    }
    caps_.width = OledFb::kWidth;    // 128
    caps_.height = OledFb::kHeight;  // 64
    caps_.format = proto::PixelFormat::kRgb565;
    caps_.color = 1;        // SSD1306 1bpp
    caps_.mono = true;
    caps_.canReadback = false;  // OLED 不支持像素读回
    caps_.sinkKind = display::DisplaySinkKind::kPhysical;
    return display::DisplayStatus::kOk;
}

display::DisplayStatus PhysicalDisplaySink::present(const display::Rect& rect,
                                                    const uint8_t* pixels) {
    // M8-B B2：RGB565 缩略渲染已废除 —— 物理应用内容只经
    // presentScene(LogicalScene)（SceneRenderer 语义渲染）。本方法保持
    // IDisplaySink 契约（Router 在 PhysicalOnly/Mirror 仍会扇出像素帧），
    // 像素帧不再进入 OLED 应用 fb（no-op）。
    (void)rect;
    (void)pixels;
    if (!enabled_.load(std::memory_order_relaxed)) {
        lastStatus_.store(static_cast<int32_t>(display::DisplayStatus::kNotEnabled),
                          std::memory_order_relaxed);
        return display::DisplayStatus::kNotEnabled;
    }
    lastStatus_.store(static_cast<int32_t>(display::DisplayStatus::kOk),
                      std::memory_order_relaxed);
    return display::DisplayStatus::kOk;
}

display::DisplayStatus PhysicalDisplaySink::presentScene(
    const display::LogicalScene& scene) {
    // setEnabled 控制应用帧接收（Router 在 setMode 切换窗口 disable 所有 sink）。
    if (!enabled_.load(std::memory_order_relaxed)) {
        lastStatus_.store(static_cast<int32_t>(display::DisplayStatus::kNotEnabled),
                          std::memory_order_relaxed);
        return display::DisplayStatus::kNotEnabled;
    }
    // Diagnostics 场景：内容由 OLED 任务 renderStatus 自绘（独立于 router 持续
    // 刷新）；本 sink 忽略场景投递。
    if (scene() != display::PhysicalScene::kApplication) {
        lastStatus_.store(static_cast<int32_t>(display::DisplayStatus::kOk),
                          std::memory_order_relaxed);
        return display::DisplayStatus::kOk;
    }
    if (display_ == nullptr) {
        lastStatus_.store(static_cast<int32_t>(display::DisplayStatus::kInternal),
                          std::memory_order_relaxed);
        return display::DisplayStatus::kInternal;
    }
    // 同步渲染进共享 1KB 应用 fb（mutex 保护；绝不持有外部指针）。
    display_->presentScene(scene);
    lastStatus_.store(static_cast<int32_t>(display::DisplayStatus::kOk),
                      std::memory_order_relaxed);
    return display::DisplayStatus::kOk;
}

display::DisplayStatus PhysicalDisplaySink::flush() {
    // present 已同步渲染完成；无排队内容（I2C 上传由 OLED 任务异步执行）。
    const display::DisplayStatus s =
        enabled_.load(std::memory_order_relaxed) ? display::DisplayStatus::kOk
                                                 : display::DisplayStatus::kNotEnabled;
    lastStatus_.store(static_cast<int32_t>(s), std::memory_order_relaxed);
    return s;
}

display::DisplayStatus PhysicalDisplaySink::setEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
    if (display_) {
        // 应用帧接收开关（taskLoop 场景分发依据：关闭时回退 renderStatus 路径）。
        display_->setAppFramesEnabled(enabled);
    }
    return display::DisplayStatus::kOk;
}

bool PhysicalDisplaySink::isAvailable() const {
    // 路由可用性 = OLED 生命周期 kReady（I2C 存活）；与 setEnabled 正交。
    // I2C 上传/失败只发生在 OLED 任务内，本类从不因物理失败阻塞调用方。
    if (!display_) {
        return false;
    }
    const OledStatus s = display_->status();
    return s.state == OledState::kReady;
}

display::DisplayStatus PhysicalDisplaySink::status() const {
    return static_cast<display::DisplayStatus>(
        lastStatus_.load(std::memory_order_relaxed));
}

void PhysicalDisplaySink::setScene(display::PhysicalScene scene) {
    scene_.store(static_cast<uint8_t>(scene), std::memory_order_relaxed);
    if (display_) {
        display_->setScene(toDisplayScene(scene));
    }
}

display::PhysicalScene PhysicalDisplaySink::scene() const {
    return static_cast<display::PhysicalScene>(
        scene_.load(std::memory_order_relaxed));
}

}  // namespace oled
}  // namespace espview