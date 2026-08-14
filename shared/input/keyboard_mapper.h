// ESPView M3 — KeyboardMapper（平台无关逻辑键 → USB HID usage，纯 C++17）。
//
// 规范来源：spec §6/§7（HID Keyboard Mapping）。PC 侧 Qt 适配层把 Qt::Key 先
// 转换成 HostKey（见 pc/src/input/qt_key_adapter.*），再经本表映射为 USB HID
// keyboard usage（0x04..0x65 / 修饰键 0xE0..0xE7）；ESP32 端直接消费 usage。
// 不在业务层传播 Qt::Key / Windows VK_*（spec §4/§5）。
//
// 覆盖范围（spec §7 至少要求）：A-Z、0-9、F1-F12、Escape/Enter/Tab/Backspace/
// Space、方向键、Home/End/PageUp/PageDown/Insert/Delete、Ctrl/Shift/Alt/GUI、
// CapsLock/NumLock/ScrollLock。不在表内的键 = unsupported（不发送）。

#pragma once

#include <cstdint>

#include "input_event.h"

namespace espview {
namespace input {

// 平台无关逻辑键（PC Qt 适配层将 Qt::Key → HostKey）。
enum class HostKey : uint16_t {
    kUnknown = 0,
    kA, kB, kC, kD, kE, kF, kG, kH, kI, kJ, kK, kL, kM,
    kN, kO, kP, kQ, kR, kS, kT, kU, kV, kW, kX, kY, kZ,
    k0, k1, k2, k3, k4, k5, k6, k7, k8, k9,
    kF1, kF2, kF3, kF4, kF5, kF6, kF7, kF8, kF9, kF10, kF11, kF12,
    kEscape, kEnter, kTab, kBackspace, kSpace,
    kLeft, kRight, kUp, kDown,
    kHome, kEnd, kPageUp, kPageDown, kInsert, kDelete,
    kLeftCtrl, kLeftShift, kLeftAlt, kLeftGui,
    kRightCtrl, kRightShift, kRightAlt, kRightGui,
    kCapsLock, kNumLock, kScrollLock,
};

// 映射结果：键本身 + （若该键是修饰键）其修饰位。
struct KeyMapResult {
    bool supported = false;
    uint32_t hidUsage = 0;      // USB HID keyboard usage
    uint16_t modifierBit = 0;   // 该键是修饰键时对应位（kModCtrl..kModGui），否则 0
};

class KeyboardMapper {
public:
    // HostKey → HID usage。返回 false = 不支持（kUnknown / 未映射）。
    static bool mapKey(HostKey key, KeyMapResult& out);
};

}  // namespace input
}  // namespace espview
