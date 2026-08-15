// ESPView M7-C4 — PhysicalCapabilitySnapshot 实现（语义见头文件）。
// 纯 C++17，零 Qt / 零平台依赖；只派生自 PhysicalStatus，不触碰协议 wire。

#include "physical_capability_snapshot.h"

namespace espview {
namespace display {

namespace {

// OLED 控制器 → 逻辑分辨率推断（与 ESP32 侧 PhysicalDisplaySink::init 的
// 落定值一致：SSD1306/SH1106 均为 128x64，1-bit 单色，不可回读）。
// 未知控制器 → 0x0（不伪造分辨率）。
void inferGeometry(const PhysicalStatus& status, int& w, int& h) {
    switch (status.oledController) {
        case OledControllerCode::kSsd1306:
        case OledControllerCode::kSh1106:
            w = 128;
            h = 64;
            return;
        default:
            w = 0;
            h = 0;
            return;
    }
}

}  // namespace

PhysicalCapabilitySnapshot makePhysicalCapabilitySnapshot(
    const PhysicalStatus& status, const PhysicalCapabilitySnapshot& prev) {
    PhysicalCapabilitySnapshot out = prev;  // 保留已学习结果（capabilityKnown 只置不清）
    if (status.oledValid) {
        out.provenance = PhysicalCapabilityProvenance::kOledTelemetry;
        out.telemetryFresh = true;
        out.healthy = status.oledOk;
        out.controller = status.oledController;
        out.address = status.oledAddress;
        if (status.oledOk) {
            out.capabilityKnown = true;
        }
        inferGeometry(status, out.width, out.height);
    }
    // scene：mod 行 physicalScene 0=Diagnostics 1=Application；0xFF/越界 → Diagnostics 兜底。
    if (status.physicalScene <= static_cast<uint8_t>(PhysicalScene::kApplication)) {
        out.scene = static_cast<PhysicalScene>(status.physicalScene);
    } else {
        out.scene = PhysicalScene::kDiagnostics;
    }
    return out;
}

PhysicalCapabilitySnapshot resetPhysicalCapability() {
    return PhysicalCapabilitySnapshot{};
}

}  // namespace display
}  // namespace espview