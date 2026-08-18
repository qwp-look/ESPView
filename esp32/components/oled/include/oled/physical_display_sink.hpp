// ESPView M7-C2 — PhysicalDisplaySink：SSD1306 物理显示 sink（IDisplaySink 实现）。
//
// 职责（M7-C2 Physical Display Sink）：
//   - init(caps)：校验生产者能力（v0.1 仅 RGB565 源帧）并落定自身能力
//     （128x64 / 1bpp mono / 不可读回 / kPhysical）；记录源帧分辨率；
//   - present(rect, pixels)：M8-B B2 起为 no-op —— RGB565 缩略渲染已废除，
//     物理应用内容只经 presentScene(LogicalScene)（SceneRenderer 语义渲染
//     进共享 1KB 应用 fb，mutex 保护，绝不持有外部指针）；Diagnostics
//     场景为 no-op（诊断页由 OLED 任务 renderStatus 自绘，独立于 router）；
//   - flush：present 已同步完成，无排队内容；
//   - setEnabled：应用帧接收开关（转发 OledDisplay::setAppFramesEnabled）；
//   - isAvailable：OLED 任务生命周期 kReady（I2C 存活）——与 setEnabled
//     正交；I2C 上传只发生在 OLED 任务内，本类不做任何 I2C 操作，因此
//     I2C 失败绝不影响 LVGL flush_cb / Virtual sink；
//   - setScene(display::PhysicalScene)：main 在 SET_MODE 后按模式映射调用
//     （Mirror/PhysicalOnly→Application；Split/VirtualOnly→Diagnostics）。

#pragma once

#include <atomic>
#include <cstdint>

#include "display_capabilities.h"  // display::DisplayCapabilities / DisplaySinkKind
#include "display_router.h"        // display::PhysicalScene
#include "display_sink.h"          // display::IDisplaySink / Rect / DisplayStatus

namespace espview {
namespace oled {

class OledDisplay;

class PhysicalDisplaySink : public display::IDisplaySink {
public:
    // 非拥有引用：OledDisplay 由 main 持有（g_oled 全局），生命周期覆盖本 sink
    //（全局销毁序：g_physicalSink 先于 g_oled）。
    explicit PhysicalDisplaySink(OledDisplay* display);
    ~PhysicalDisplaySink() override = default;

    // ---- IDisplaySink ----
    display::DisplayStatus init(const display::DisplayCapabilities& caps) override;
    const display::DisplayCapabilities& capabilities() const override { return caps_; }
    display::DisplayStatus present(const display::Rect& rect, const uint8_t* pixels) override;
    // M8-B B2：语义场景投递（Router 在 PhysicalOnly/Mirror 调用；SceneRenderer 渲染）。
    display::DisplayStatus presentScene(const display::LogicalScene& logicalScene) override;
    display::DisplayStatus flush() override;
    display::DisplayStatus setEnabled(bool enabled) override;
    bool isAvailable() const override;
    display::DisplayStatus status() const override;

    // ---- M7-C2 场景控制（main 在 SET_MODE 后调用）----
    void setScene(display::PhysicalScene scene);
    display::PhysicalScene scene() const;

private:
    OledDisplay* display_ = nullptr;  // 非拥有（main 保证 g_oled 生命周期覆盖）
    display::DisplayCapabilities caps_;
    std::atomic<uint8_t> scene_{static_cast<uint8_t>(display::PhysicalScene::kDiagnostics)};
    std::atomic<bool> enabled_{false};
    std::atomic<int32_t> lastStatus_{static_cast<int32_t>(display::DisplayStatus::kOk)};
};

}  // namespace oled
}  // namespace espview