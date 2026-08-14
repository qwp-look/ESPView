// ESPView M3 — InputController 实现（见 input_controller.h）。

#include "input_controller.h"

#include <chrono>
#include <utility>

#include "keyboard_mapper.h"
#include "qt_key_adapter.h"

namespace espview {
namespace pc {

using espview::input::InputEvent;

namespace {

uint64_t steadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

InputController::InputController(QObject* parent) : QObject(parent) {
    // MouseMove 限频兜底：每 16ms 补发被 coalesce 的最新一笔（spec §13）。
    flushTimer_ = new QTimer(this);
    flushTimer_->setInterval(
        static_cast<int>(espview::input::InputPolicy::kMinMouseMoveIntervalMs));
    connect(flushTimer_, &QTimer::timeout, this, &InputController::flushPendingMoves);
    flushTimer_->start();
}

void InputController::setSendFn(SendFn fn) {
    sendFn_ = std::move(fn);
}

void InputController::setDisplaySize(uint16_t width, uint16_t height) {
    if (width >= 1 && height >= 1) {
        width_ = width;
        height_ = height;
    }
}

uint16_t InputController::clampX(int x) const {
    return x <= 0 ? 0u : (static_cast<uint32_t>(x) >= width_ ? static_cast<uint16_t>(width_ - 1u)
                                                             : static_cast<uint16_t>(x));
}

uint16_t InputController::clampY(int y) const {
    return y <= 0 ? 0u : (static_cast<uint32_t>(y) >= height_ ? static_cast<uint16_t>(height_ - 1u)
                                                              : static_cast<uint16_t>(y));
}

void InputController::send(const espview::input::InputEvent& e) {
    if (sendFn_) {
        sendFn_(e);
    }
}

void InputController::onMouseMove(int x, int y, uint8_t buttonsMask) {
    lastButtons_ = buttonsMask & espview::input::kMouseButtonMask;
    uint16_t ox = 0, oy = 0;
    uint8_t ob = 0;
    if (moveThrottle_.acceptMove(steadyMs(), clampX(x), clampY(y), lastButtons_, ox, oy, ob)) {
        InputEvent e = espview::input::makeMouseMove(ox, oy, ob, steadyMs());
        ++stats_.mouseSent;
        send(e);
    } else {
        ++stats_.moveThrottled;  // 已 coalesce，16ms 定时 flush 补发
    }
}

void InputController::flushPendingMoves() {
    uint16_t ox = 0, oy = 0;
    uint8_t ob = 0;
    if (moveThrottle_.flushPending(steadyMs(), ox, oy, ob)) {
        InputEvent e = espview::input::makeMouseMove(ox, oy, ob, steadyMs());
        ++stats_.mouseSent;
        send(e);
    }
}

void InputController::onMousePress(uint8_t buttonBit, int x, int y, uint16_t modifiersMask) {
    (void)modifiersMask;  // v0.1：鼠标事件 modifiers 不上 wire（DESIGN.md 无字段）
    if (buttonBit == 0) {
        return;
    }
    lastButtons_ = static_cast<uint8_t>(lastButtons_ | buttonBit);
    InputEvent e = espview::input::makeMouseButton(espview::input::InputType::kMouseDown,
                                                   clampX(x), clampY(y), lastButtons_,
                                                   steadyMs());
    ++stats_.mouseSent;
    send(e);
}

void InputController::onMouseRelease(uint8_t buttonBit, int x, int y, uint16_t modifiersMask) {
    (void)modifiersMask;
    if (buttonBit == 0) {
        return;
    }
    lastButtons_ = static_cast<uint8_t>(lastButtons_ & ~buttonBit);
    InputEvent e = espview::input::makeMouseButton(espview::input::InputType::kMouseUp,
                                                   clampX(x), clampY(y), lastButtons_,
                                                   steadyMs());
    ++stats_.mouseSent;
    send(e);
}

void InputController::onWheel(int angleDeltaY, int x, int y, uint8_t buttonsMask,
                              uint16_t modifiersMask) {
    (void)modifiersMask;
    lastButtons_ = buttonsMask & espview::input::kMouseButtonMask;
    const int8_t steps = espview::input::normalizeWheelDelta(angleDeltaY);
    if (steps == 0) {
        return;  // 水平滚轮 / 增量不足一格：v0.1 不发送（spec §15）
    }
    InputEvent e = espview::input::makeMouseWheel(clampX(x), clampY(y), steps, lastButtons_,
                                                  steadyMs());
    ++stats_.mouseSent;
    send(e);
}

void InputController::onKeyPress(int qtKey, uint16_t modifiersMask, bool autoRepeat) {
    // spec §9：autoRepeat KeyDown 忽略（避免 ESP32 收到重复状态事件）；
    // 普通 KeyDown / KeyUp 均发送。
    if (!espview::input::InputPolicy::acceptKey(true, autoRepeat)) {
        ++stats_.ignoredAutoRepeat;
        return;
    }
    const espview::input::HostKey host = espview::input::toHostKey(qtKey);
    espview::input::KeyMapResult map;
    if (!espview::input::KeyboardMapper::mapKey(host, map)) {
        ++stats_.unsupported;  // 未覆盖键：不发送（spec §7）
        return;
    }
    InputEvent e = espview::input::makeKeyEvent(espview::input::InputType::kKeyDown,
                                                map.hidUsage,
                                                modifiersMask & espview::input::kModifierMask,
                                                steadyMs());
    ++stats_.keysSent;
    send(e);
}

void InputController::onKeyRelease(int qtKey, uint16_t modifiersMask) {
    const espview::input::HostKey host = espview::input::toHostKey(qtKey);
    espview::input::KeyMapResult map;
    if (!espview::input::KeyboardMapper::mapKey(host, map)) {
        ++stats_.unsupported;  // 按下时未发送，抬起也不发送（保持一致）
        return;
    }
    InputEvent e = espview::input::makeKeyEvent(espview::input::InputType::kKeyUp,
                                                map.hidUsage,
                                                modifiersMask & espview::input::kModifierMask,
                                                steadyMs());
    ++stats_.keysSent;
    send(e);
}

}  // namespace pc
}  // namespace espview
