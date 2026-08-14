// ESPView M5-B — LVGL Input Adapter 实现（见 lvgl_adapter.h）。

#include "lvgl_adapter.h"

#include <algorithm>
#include <climits>
#include <cstdint>

#include "hid_lvgl_keymap.h"

namespace espview {
namespace input {

LvglInputAdapter::LvglInputAdapter(uint16_t displayWidth, uint16_t displayHeight)
    : width_(displayWidth), height_(displayHeight) {}

void LvglInputAdapter::setDisplaySize(uint16_t width, uint16_t height) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (width >= 1 && height >= 1) {
        width_ = width;
        height_ = height;
    }
}

void LvglInputAdapter::onInputEvent(const InputEvent& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (e.isKey()) {
        feedKey(e);
    } else if (e.isMouse()) {
        feedPointer(e);
    } else {
        ++stats_.invalidEvents;  // Touch* 等保留类型
    }
}

void LvglInputAdapter::feedPointer(const InputEvent& e) {
    // 第二层校验（InputManager 已校验；防御性重复校验，§23）。
    if (e.x >= width_ || e.y >= height_) {
        ++stats_.invalidEvents;
        return;
    }
    x_ = static_cast<int16_t>(e.x);
    y_ = static_cast<int16_t>(e.y);

    switch (e.type) {
        case InputType::kMouseMove:
            ++stats_.moves;
            break;
        case InputType::kMouseDown:
            ++stats_.downEvents;
            break;
        case InputType::kMouseUp:
            ++stats_.upEvents;
            break;
        case InputType::kMouseWheel: {
            ++stats_.wheelEvents;
            const int32_t delta = e.wheelDelta;
            if (delta > 0) {
                wheelAccum_ = (wheelAccum_ > INT32_MAX - delta) ? INT32_MAX
                                                                : wheelAccum_ + delta;
            } else if (delta < 0) {
                wheelAccum_ = (wheelAccum_ < INT32_MIN - delta) ? INT32_MIN
                                                                : wheelAccum_ + delta;
            }
            stats_.wheelSteps += delta;
            break;
        }
        default:
            ++stats_.invalidEvents;
            return;
    }

    // LEFT → LVGL pointer；RIGHT/MIDDLE 记录但不消费（§8）。
    const bool left = (e.buttons & kMouseLeft) != 0;
    if ((e.buttons & (kMouseRight | kMouseMiddle)) != 0) {
        ++stats_.ignoredButtons;
    }
    if (e.type == InputType::kMouseDown) {
        if (left) {
            // 新的 LEFT 按下：取消未决释放，立即进入 PRESSED。
            leftPressed_ = true;
            releasePending_ = false;
            holdReadsLeft_ = 0;
        }
    } else if (e.type == InputType::kMouseUp) {
        if (left) {
            // 防御：掩码仍带 LEFT（异常事件序），保持 PRESSED。
            leftPressed_ = true;
        } else if (leftPressed_) {
            // LEFT 抬起：冻结点击点，保持 PRESSED 若干 read_cb 周期，
            // 确保 LVGL ~30ms 轮询至少观察到一次 PRESSED → 点击不丢
            // （否则 Down+Up 落在两次轮询之间会被完全漏掉）。
            releasePending_ = true;
            holdReadsLeft_ = kPointerClickHoldReads;
            holdX_ = x_;
            holdY_ = y_;
        }
    } else if (e.type == InputType::kMouseMove) {
        if (!releasePending_) {
            leftPressed_ = left;  // Move 携带掩码（InputManager 已推导 Down/Up）
        }
        // 保持期间 Move 只更新坐标；冻结点击点由 pointerState() 返回。
    }
}

void LvglInputAdapter::feedKey(const InputEvent& e) {
    // 第二层校验（keycode 范围 InputManager 已保证；防御性重复校验）。
    const bool keyOk =
        (e.keycode >= kHidKeyboardFirst && e.keycode <= kHidKeyboardLast) ||
        (e.keycode >= kHidModifierFirst && e.keycode <= kHidModifierLast);
    if (!keyOk) {
        ++stats_.invalidEvents;
        return;
    }
    uint32_t lvglKey = 0;
    if (!HidToLvglKeyMapper::mapKey(e.keycode, lvglKey)) {
        ++stats_.unmappedKeys;  // 修饰键/F 键/PageUp…：不合成 LVGL 事件
        return;
    }
    if (e.type == InputType::kKeyDown) {
        ++stats_.keyDowns;
        pushKey(lvglKey, true);
    } else if (e.type == InputType::kKeyUp) {
        ++stats_.keyUps;
        pushKey(lvglKey, false);
    } else {
        ++stats_.invalidEvents;
    }
}

void LvglInputAdapter::pushKey(uint32_t lvglKey, bool pressed) {
    if (keyCount_ == kKeyQueueCapacity) {
        ++stats_.keyQueueDropped;  // 满：丢最新（丢 KeyDown 不会 stuck）
        return;
    }
    const size_t tail = (keyHead_ + keyCount_) % kKeyQueueCapacity;
    keyQueue_[tail] = LvglKeyEvent{lvglKey, pressed};
    ++keyCount_;
}

LvglPointerState LvglInputAdapter::pointerState() {
    std::lock_guard<std::mutex> lk(mutex_);
    LvglPointerState st;
    if (releasePending_) {
        if (holdReadsLeft_ == 0) {
            // 保持窗口结束：真正释放；点击点冻结在抬起位置 → LVGL
            // RELEASED 命中同一对象，LV_EVENT_CLICKED 稳定触发。
            leftPressed_ = false;
            releasePending_ = false;
            st.x = x_;
            st.y = y_;
        } else {
            --holdReadsLeft_;
            st.x = holdX_;
            st.y = holdY_;
            st.leftPressed = true;
        }
    } else {
        st.x = x_;
        st.y = y_;
        st.leftPressed = leftPressed_;
    }
    return st;
}

LvglPointerState LvglInputAdapter::peekPointer() {
    std::lock_guard<std::mutex> lk(mutex_);
    LvglPointerState st;
    if (releasePending_) {
        // 保持期间视作按下，返回冻结点击点；不推进保持窗口。
        st.x = holdX_;
        st.y = holdY_;
        st.leftPressed = true;
    } else {
        st.x = x_;
        st.y = y_;
        st.leftPressed = leftPressed_;
    }
    return st;
}

bool LvglInputAdapter::hasPendingKey() {
    std::lock_guard<std::mutex> lk(mutex_);
    return keyCount_ != 0;
}

bool LvglInputAdapter::nextKeyEvent(LvglKeyEvent& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (keyCount_ == 0) {
        return false;
    }
    out = keyQueue_[keyHead_];
    keyHead_ = (keyHead_ + 1) % kKeyQueueCapacity;
    --keyCount_;
    ++stats_.consumedKeys;
    return true;
}

int16_t LvglInputAdapter::pendingWheelDiff() {
    std::lock_guard<std::mutex> lk(mutex_);
    return clampWheel();
}

int16_t LvglInputAdapter::consumeWheelDiff() {
    std::lock_guard<std::mutex> lk(mutex_);
    return clampWheel();
}

int16_t LvglInputAdapter::clampWheel() {
    int32_t clamped = wheelAccum_;
    if (clamped > INT16_MAX) {
        clamped = INT16_MAX;
    } else if (clamped < INT16_MIN) {
        clamped = INT16_MIN;
    }
    wheelAccum_ -= clamped;  // 余量保留给下一次 read_cb（不丢步数）
    return static_cast<int16_t>(clamped);
}

void LvglInputAdapter::reset() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++stats_.resets;
    x_ = 0;
    y_ = 0;
    leftPressed_ = false;
    releasePending_ = false;
    holdReadsLeft_ = 0;
    wheelAccum_ = 0;
    keyHead_ = 0;
    keyCount_ = 0;
    // keyQueue_ 内容不必清除（head/count 复位即可）。
}

LvglAdapterStats LvglInputAdapter::stats() {
    std::lock_guard<std::mutex> lk(mutex_);
    return stats_;
}

}  // namespace input
}  // namespace espview
