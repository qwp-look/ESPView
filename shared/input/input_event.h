// ESPView M3 — InputEvent：PC 与 ESP32 统一的内部输入模型（纯 C++17）。
//
// 规范来源：docs/DESIGN.md B.4（InputManager）+ G 节（输入事件架构）。
// PC 与 ESP32 业务层都不传播 Qt::Key / Windows VK_* / mouse flags，
// 内部统一使用本结构；INPUT_KEY / INPUT_MOUSE 只是它的 wire 编码。
//
// 字段语义（与 DESIGN.md INPUT_KEY / INPUT_MOUSE layout 对齐）：
//   - buttons:   bit0=LEFT, bit1=RIGHT, bit2=MIDDLE（当前按下状态掩码）
//   - modifiers: bit0=Ctrl, bit1=Shift, bit2=Alt, bit3=GUI
//   - keycode:   USB HID keyboard usage（0x04..0x65 / 修饰键 0xE0..0xE7）
//   - x/y:       显示坐标系（0..width-1, 0..height-1）
//   - wheelDelta: 有符号滚轮格数（-128..127）
//   - timestampMs: 本地时间戳，不上 wire（DESIGN.md 未给 INPUT_* 加时间戳，
//     M3 不修改 wire format；PC 发送时间与 ESP32 接收时间分别记录）。

#pragma once

#include <cstdint>

namespace espview {
namespace input {

// 事件类型。Touch* 保留 enum/API，M3 不发送。
enum class InputType : uint8_t {
    kMouseMove = 0,
    kMouseDown = 1,
    kMouseUp = 2,
    kMouseWheel = 3,
    kKeyDown = 4,
    kKeyUp = 5,
    kTouchDown = 6,  // 保留
    kTouchMove = 7,  // 保留
    kTouchUp = 8,    // 保留
};

// ---- 鼠标按键位掩码（DESIGN.md INPUT_MOUSE.buttons）----
inline constexpr uint8_t kMouseLeft = 0x01;
inline constexpr uint8_t kMouseRight = 0x02;
inline constexpr uint8_t kMouseMiddle = 0x04;
inline constexpr uint8_t kMouseButtonMask = 0x07;

// ---- 修饰键位掩码（DESIGN.md INPUT_KEY.modifiers）----
inline constexpr uint16_t kModCtrl = 0x0001;
inline constexpr uint16_t kModShift = 0x0002;
inline constexpr uint16_t kModAlt = 0x0004;
inline constexpr uint16_t kModGui = 0x0008;
inline constexpr uint16_t kModifierMask = 0x000F;

// ---- USB HID keyboard usage 范围（DESIGN.md INPUT_KEY.keycode）----
inline constexpr uint32_t kHidKeyboardFirst = 0x04;  // A
inline constexpr uint32_t kHidKeyboardLast = 0x65;   // Keypad =
inline constexpr uint32_t kHidModifierFirst = 0xE0;  // Left Ctrl
inline constexpr uint32_t kHidModifierLast = 0xE7;   // Right GUI
inline constexpr uint32_t kHidUsageMax = 0xFFFF;

inline constexpr uint16_t kHidUsageLeftCtrl = 0xE0;
inline constexpr uint16_t kHidUsageLeftShift = 0xE1;
inline constexpr uint16_t kHidUsageLeftAlt = 0xE2;
inline constexpr uint16_t kHidUsageLeftGui = 0xE3;
inline constexpr uint16_t kHidUsageRightCtrl = 0xE4;
inline constexpr uint16_t kHidUsageRightShift = 0xE5;
inline constexpr uint16_t kHidUsageRightAlt = 0xE6;
inline constexpr uint16_t kHidUsageRightGui = 0xE7;

struct InputEvent {
    InputType type = InputType::kMouseMove;
    uint16_t x = 0;          // 显示坐标系（mouse 事件）
    uint16_t y = 0;
    uint8_t buttons = 0;     // 位掩码（当前按下状态）
    int8_t wheelDelta = 0;   // 滚轮格数
    uint32_t keycode = 0;    // USB HID usage（key 事件）
    uint16_t modifiers = 0;  // 位掩码
    uint64_t timestampMs = 0;

    bool isKey() const {
        return type == InputType::kKeyDown || type == InputType::kKeyUp;
    }
    bool isMouse() const {
        return type == InputType::kMouseMove || type == InputType::kMouseDown ||
               type == InputType::kMouseUp || type == InputType::kMouseWheel;
    }
};

// ---- 便捷构造（timestampMs 由发送/接收方本地赋值）----
inline InputEvent makeKeyEvent(InputType type, uint32_t keycode, uint16_t modifiers,
                               uint64_t timestampMs) {
    InputEvent e;
    e.type = type;
    e.keycode = keycode;
    e.modifiers = modifiers;
    e.timestampMs = timestampMs;
    return e;
}

inline InputEvent makeMouseMove(uint16_t x, uint16_t y, uint8_t buttons, uint64_t timestampMs) {
    InputEvent e;
    e.type = InputType::kMouseMove;
    e.x = x;
    e.y = y;
    e.buttons = buttons;
    e.timestampMs = timestampMs;
    return e;
}

inline InputEvent makeMouseButton(InputType type, uint16_t x, uint16_t y, uint8_t buttons,
                                  uint64_t timestampMs) {
    InputEvent e;
    e.type = type;  // kMouseDown / kMouseUp
    e.x = x;
    e.y = y;
    e.buttons = buttons;
    e.timestampMs = timestampMs;
    return e;
}

inline InputEvent makeMouseWheel(uint16_t x, uint16_t y, int8_t wheelDelta, uint8_t buttons,
                                 uint64_t timestampMs) {
    InputEvent e;
    e.type = InputType::kMouseWheel;
    e.x = x;
    e.y = y;
    e.wheelDelta = wheelDelta;
    e.buttons = buttons;
    e.timestampMs = timestampMs;
    return e;
}

}  // namespace input
}  // namespace espview
