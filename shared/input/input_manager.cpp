// ESPView M3 — InputManager 实现（见 input_manager.h）。

#include "input_manager.h"

#include <algorithm>
#include <cstddef>

namespace espview {
namespace input {

InputManager::InputManager(uint16_t displayWidth, uint16_t displayHeight)
    : width_(displayWidth), height_(displayHeight) {}

void InputManager::registerListener(IInputListener* l) {
    listener_ = l;
}

void InputManager::setDisplaySize(uint16_t width, uint16_t height) {
    width_ = width;
    height_ = height;
}

bool InputManager::validate(const InputEvent& e) const {
    switch (e.type) {
        case InputType::kKeyDown:
        case InputType::kKeyUp: {
            const bool keyOk =
                (e.keycode >= kHidKeyboardFirst && e.keycode <= kHidKeyboardLast) ||
                (e.keycode >= kHidModifierFirst && e.keycode <= kHidModifierLast);
            return keyOk && (e.modifiers & ~kModifierMask) == 0;
        }
        case InputType::kMouseMove:
        case InputType::kMouseDown:
        case InputType::kMouseUp:
        case InputType::kMouseWheel:
            return (e.buttons & ~kMouseButtonMask) == 0 && e.x < width_ && e.y < height_;
        case InputType::kTouchDown:
        case InputType::kTouchMove:
        case InputType::kTouchUp:
        default:
            return false;  // 保留类型：不算「非法」，按 unsupported 统计
    }
}

void InputManager::forward(const InputEvent& e) {
    ++stats_.validEvents;
    if (listener_ != nullptr) {
        listener_->onInputEvent(e);
    }
}

void InputManager::feed(const InputEvent& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    feedLocked(e);
}

void InputManager::noteInvalidInput() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++stats_.invalidEvents;
}

void InputManager::feedLocked(const InputEvent& e) {
    ++stats_.eventsReceived;

    switch (e.type) {
        case InputType::kTouchDown:
        case InputType::kTouchMove:
        case InputType::kTouchUp:
            ++stats_.unsupportedEvents;  // M3 不消费触摸（保留类型）
            return;
        default:
            break;
    }

    if (!validate(e)) {
        ++stats_.invalidEvents;
        return;
    }

    switch (e.type) {
        case InputType::kKeyDown: {
            // 幂等加入 pressed 集合。
            if (std::find(pressedKeys_.begin(), pressedKeys_.end(), e.keycode) ==
                pressedKeys_.end()) {
                pressedKeys_.push_back(e.keycode);
            }
            stats_.pressedKeys = static_cast<uint32_t>(pressedKeys_.size());
            forward(e);
            return;
        }
        case InputType::kKeyUp: {
            pressedKeys_.erase(std::remove(pressedKeys_.begin(), pressedKeys_.end(), e.keycode),
                               pressedKeys_.end());
            stats_.pressedKeys = static_cast<uint32_t>(pressedKeys_.size());
            forward(e);
            return;
        }
        case InputType::kMouseDown:
        case InputType::kMouseUp: {
            // 显式定型事件（未来路径，防御性支持）：直接转发并同步按钮状态。
            setButtons(e.buttons, e.x, e.y);
            forward(e);
            return;
        }
        case InputType::kMouseMove:
        case InputType::kMouseWheel:
            applyMouseState(e);  // 按掩码变化推导 Down/Up（Move/Wheel 直接转发）
            return;
        default:
            ++stats_.invalidEvents;
            return;
    }
}

void InputManager::applyMouseState(const InputEvent& raw) {
    stats_.lastX = raw.x;
    stats_.lastY = raw.y;

    if (raw.type == InputType::kMouseWheel) {
        // 滚轮事件：转发（携带当前按钮掩码），不同步 Down/Up（掩码照常更新）。
        setButtons(raw.buttons, raw.x, raw.y);
        forward(raw);
        return;
    }

    stats_.pressedButtons = raw.buttons;

    // Move：按掩码相对上一状态的变化位推导 Down/Up（spec §14）。
    const uint8_t gained = static_cast<uint8_t>(raw.buttons & ~buttons_);
    const uint8_t lost = static_cast<uint8_t>(buttons_ & ~raw.buttons);

    InputEvent e = raw;
    e.type = InputType::kMouseMove;
    e.timestampMs = raw.timestampMs;

    for (uint8_t bit = kMouseLeft; bit != 0; bit = static_cast<uint8_t>(bit << 1)) {
        if ((gained & bit) != 0) {
            InputEvent down = e;
            down.type = InputType::kMouseDown;
            forward(down);
        }
        if ((lost & bit) != 0) {
            InputEvent up = e;
            up.type = InputType::kMouseUp;
            forward(up);
        }
    }
    // 无按钮变化（或变化已分别上报）时，转发 Move 本身。
    if (gained == 0 && lost == 0) {
        forward(e);
    }
    buttons_ = raw.buttons;
}

void InputManager::setButtons(uint8_t buttons, uint16_t x, uint16_t y) {
    buttons_ = buttons;
    stats_.pressedButtons = buttons;
    stats_.lastX = x;
    stats_.lastY = y;
}

void InputManager::resetState() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++stats_.resetCount;
    // 本地安全恢复：pressed keys → KeyUp；pressed buttons → MouseUp(buttons=0)。
    // 绝不把 release 回发给 PC（spec §18）。
    for (uint32_t k : pressedKeys_) {
        InputEvent up = makeKeyEvent(InputType::kKeyUp, k, 0, 0);
        ++stats_.stuckKeysReleased;
        if (listener_ != nullptr) {
            listener_->onInputEvent(up);
        }
    }
    pressedKeys_.clear();

    for (uint8_t bit = kMouseLeft; bit != 0; bit = static_cast<uint8_t>(bit << 1)) {
        if ((buttons_ & bit) != 0) {
            InputEvent up =
                makeMouseButton(InputType::kMouseUp, stats_.lastX, stats_.lastY, 0, 0);
            ++stats_.stuckButtonsReleased;
            if (listener_ != nullptr) {
                listener_->onInputEvent(up);
            }
        }
    }
    buttons_ = 0;
    stats_.pressedKeys = 0;
    stats_.pressedButtons = 0;
}

}  // namespace input
}  // namespace espview
