// ESPView M7-C4 — PhysicalCapabilitySnapshot：PC 侧物理能力快照（纯 C++17，零依赖）。
//
// 定位：把 ESP32 物理显示「能力」与「健康」分离，作为 GUI 侧唯一收敛点。
// 背景（M7-C4 Capability Audit）：协议 v0.1 无 capability uplink（HELLO mode_mask
// 是编译期常量；oled 文本遥测是唯一可观测源），M7-C3 的 oled ok=1 一次性闩锁存在
// 无撤销 / 跨会话残留问题。本模型定义：
//   - capabilityKnown —— 本会话已学习到物理 sink 存在（曾见 ok=1）；只管门控；
//   - healthy —— 最近遥测 oledOk（动态健康）；只管降级展示；
//   - telemetryFresh —— 会话内最近收到过 oled 行（stale 判定）。
// provenance 字段为未来协议能力上行（CAPABILITIES 消息）预留扩展点，不修改 wire。
//
// 线程/所有权：纯值类型，无锁无分配；GUI 线程独占消费。派生自 PhysicalStatus
// （physical_status.h），不触碰协议、不持有传输。

#pragma once

#include <cstdint>

#include "display_router.h"   // PhysicalScene（场景枚举）
#include "physical_status.h"  // PhysicalStatus / OledControllerCode

namespace espview {
namespace display {

// 能力来源（当前唯一 = OLED 文本遥测推断；未来可加 kCapabilitiesMessage）。
enum class PhysicalCapabilityProvenance : uint8_t {
    kNotSeen = 0,       // 未收到任何物理证据
    kOledTelemetry = 1, // 由 oled 文本遥测学习
    kCapabilitiesMessage = 2, // 由 CAPABILITIES 协议消息（M7-D1）学习（最高信任）
};

// PC 侧物理能力快照（值语义；单一派生源）。
struct PhysicalCapabilitySnapshot {
    PhysicalCapabilityProvenance provenance = PhysicalCapabilityProvenance::kNotSeen;
    bool capabilityKnown = false;  // 本会话已学习到物理 sink 存在（曾见 ok=1）
    bool healthy = false;          // 最近遥测 oledOk（动态健康，非能力）
    bool telemetryFresh = false;   // 会话内最近收到过 oled 行（stale 判定）
    int width = 0;                 // 物理分辨率：由 controller 推断（SSD1306/SH1106 → 128x64；未知 → 0）
    int height = 0;
    bool mono = true;              // 由 controller 推断（OLED 事实）
    bool canReadback = false;      // 无 wire → OLED 事实常量（false）
    OledControllerCode controller = OledControllerCode::kUnknown;
    uint8_t address = 0;           // I2C 7-bit 地址（0 = 未知）
    PhysicalScene scene = PhysicalScene::kDiagnostics;  // 最近物理场景（mod 行或本地派生）
};

// 从 PhysicalStatus 派生快照（单一推断位置）。不修改 status。
// 规则：
//   - 收到过 oled 行（oledValid）→ provenance=kOledTelemetry、telemetryFresh=true；
//   - oledOk==true → capabilityKnown=true（学习结果，只置位不清除，由 reset 撤销）；
//   - healthy=oledOk（动态）；controller/address 直通；
//   - width/height 按 controller 推断（SSD1306/SH1106 → 128x64；未知 → 0）；
//   - scene 取 status.physicalScene（0xFF → kDiagnostics 兜底）。
PhysicalCapabilitySnapshot makePhysicalCapabilitySnapshot(const PhysicalStatus& status,
                                                          const PhysicalCapabilitySnapshot& prev);

// 会话断开 / 无数据：清空学习结果（修复跨会话残留）。返回全新快照（provenance=kNotSeen）。
PhysicalCapabilitySnapshot resetPhysicalCapability();

}  // namespace display
}  // namespace espview