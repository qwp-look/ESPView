// ESPView M3 — input_codec 实现（见 input_codec.h）。

#include "input_codec.h"

#include <cstddef>

namespace espview {
namespace input {

namespace {

uint16_t leU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t leU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool isKeyDown(uint8_t v) {
    return v == 0 || v == 1;
}

// USB HID 键盘 usage 合法性（DESIGN.md INPUT_KEY.keycode 范围）。
bool isKeycodeValid(uint32_t k) {
    if (k >= kHidKeyboardFirst && k <= kHidKeyboardLast) {
        return true;
    }
    return k >= kHidModifierFirst && k <= kHidModifierLast;
}

}  // namespace

std::optional<proto::Message> encodeInputEvent(const InputEvent& e, uint16_t maxX,
                                               uint16_t maxY) {
    switch (e.type) {
        case InputType::kKeyDown:
        case InputType::kKeyUp: {
            if (!isKeycodeValid(e.keycode)) {
                return std::nullopt;
            }
            if ((e.modifiers & ~kModifierMask) != 0) {
                return std::nullopt;
            }
            return proto::makeInputKey(e.keycode, e.modifiers, e.type == InputType::kKeyDown);
        }
        case InputType::kMouseMove:
        case InputType::kMouseDown:
        case InputType::kMouseUp:
        case InputType::kMouseWheel: {
            if ((e.buttons & ~kMouseButtonMask) != 0) {
                return std::nullopt;
            }
            if (e.x > maxX || e.y > maxY) {
                return std::nullopt;
            }
            // 所有鼠标事件在 wire 上都是同一「指针状态」布局（见 input_codec.h）。
            return proto::makeInputMouse(e.buttons, e.x, e.y, e.wheelDelta, kMouseFlagAbs);
        }
        case InputType::kTouchDown:
        case InputType::kTouchMove:
        case InputType::kTouchUp:
        default:
            return std::nullopt;  // 保留类型 / 未知类型：M3 不发送
    }
}

std::optional<InputEvent> decodeInputMessage(const proto::Message& msg, uint16_t maxX,
                                             uint16_t maxY) {
    const auto type = static_cast<proto::MessageType>(msg.type);
    if (type == proto::MessageType::kInputKey) {
        if (msg.payload.size() != 8u) {
            return std::nullopt;
        }
        const uint8_t* p = msg.payload.data();
        const uint32_t keycode = leU32(p);
        const uint16_t modifiers = leU16(p + 4);
        const uint8_t down = p[6];
        const uint8_t rsvd = p[7];
        if (!isKeycodeValid(keycode)) {
            return std::nullopt;
        }
        if ((modifiers & ~kModifierMask) != 0) {
            return std::nullopt;
        }
        if (!isKeyDown(down)) {
            return std::nullopt;
        }
        if (rsvd != 0) {
            return std::nullopt;
        }
        InputEvent e;
        e.type = down == 1u ? InputType::kKeyDown : InputType::kKeyUp;
        e.keycode = keycode;
        e.modifiers = modifiers;
        e.timestampMs = 0;  // wire 无时间戳；接收方本地赋值
        return e;
    }

    if (type == proto::MessageType::kInputMouse) {
        if (msg.payload.size() != 8u) {
            return std::nullopt;
        }
        const uint8_t* p = msg.payload.data();
        const uint8_t buttons = p[0];
        const uint16_t x = leU16(p + 1);
        const uint16_t y = leU16(p + 3);
        const int8_t wheel = static_cast<int8_t>(p[5]);
        const uint8_t flags = p[6];
        const uint8_t rsvd = p[7];
        if ((buttons & ~kMouseButtonMask) != 0) {
            return std::nullopt;
        }
        if (x > maxX || y > maxY) {
            return std::nullopt;
        }
        if (flags != kMouseFlagAbs) {
            return std::nullopt;
        }
        if (rsvd != 0) {
            return std::nullopt;
        }
        InputEvent e;
        // wire 无事件类型：wheel != 0 → Wheel；否则 Move（Down/Up 由
        // InputManager 按按钮掩码变化推导）。
        e.type = wheel != 0 ? InputType::kMouseWheel : InputType::kMouseMove;
        e.x = x;
        e.y = y;
        e.buttons = buttons;
        e.wheelDelta = wheel;
        e.timestampMs = 0;
        return e;
    }

    return std::nullopt;  // 非输入消息 / 未知类型
}

}  // namespace input
}  // namespace espview
