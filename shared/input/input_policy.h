// ESPView M3 — 输入策略（autoRepeat 忽略 + MouseMove 限频 coalesce，纯 C++17）。
//
// 规范来源：spec §9（重复按键）/ §13（MouseMove 节流）。属于「实现语义」，
// 不影响 wire format；本层纯函数/小状态机，可宿主测试。
//
// autoRepeat：Qt KeyDown + autoRepeat → 忽略（不发送），避免 ESP32 收到无法
//   对应的多次状态事件；KeyUp 始终发送（保证不会 stuck key）。
// MouseMove：限频 ≤60 Hz（最小间隔 16ms）；窗口期内新 Move 只更新「最新坐标」
//   （coalesce），由调用方用 flushPending() 在定时 tick（如 16ms）时补发最后一笔。

#pragma once

#include <cstdint>

namespace espview {
namespace input {

// 滚轮归一化（spec §15）：Qt angleDelta().y() / 120 → 格数（+1/-1），clamp 到
// int8_t 范围（-128..127）。不要把 Qt 120 直接 cast 到 int8_t。
inline int8_t normalizeWheelDelta(int qtAngleDeltaY) {
    long long steps = qtAngleDeltaY / 120;  // 向零截断（Qt 正负语义一致）
    if (steps > 127) {
        steps = 127;
    }
    if (steps < -128) {
        steps = -128;
    }
    return static_cast<int8_t>(steps);
}

class InputPolicy {
public:
    // MouseMove 最小发送间隔：16ms ≈ 62.5Hz（满足 spec「≤60 Hz」语义）。
    static constexpr uint64_t kMinMouseMoveIntervalMs = 16;

    // 是否应转发该按键事件（down + autoRepeat → 忽略）。
    static bool acceptKey(bool down, bool autoRepeat) {
        return !(down && autoRepeat);
    }
};

// MouseMove 限频器（coalesce 到最新坐标；无锁，GUI 线程独占）。
class MouseMoveThrottle {
public:
    explicit MouseMoveThrottle(uint64_t minIntervalMs = InputPolicy::kMinMouseMoveIntervalMs)
        : minIntervalMs_(minIntervalMs) {}

    // 收到一次原始 Move。返回 true = 应立即发送（距上次发送已超间隔）；
    // false = 已 coalesce 到内部「最新坐标」，由后续 flushPending() 补发。
    bool acceptMove(uint64_t nowMs, uint16_t x, uint16_t y, uint8_t buttons,
                    uint16_t& outX, uint16_t& outY, uint8_t& outButtons) {
        pendingX_ = x;
        pendingY_ = y;
        pendingButtons_ = buttons;
        hasPending_ = true;
        if (nowMs - lastSentMs_ >= minIntervalMs_) {
            return flushPending(nowMs, outX, outY, outButtons);
        }
        return false;
    }

    // 返回是否仍有未发送的最新 Move；有则写出并清空。调用方应在定时 tick
    // （如每 16ms）调用一次，保证「移动后停止」的最后一笔也能送达。
    bool flushPending(uint64_t nowMs, uint16_t& outX, uint16_t& outY, uint8_t& outButtons) {
        if (!hasPending_) {
            return false;
        }
        outX = pendingX_;
        outY = pendingY_;
        outButtons = pendingButtons_;
        hasPending_ = false;
        lastSentMs_ = nowMs;
        return true;
    }

    void reset() {
        hasPending_ = false;
        lastSentMs_ = 0;
    }

private:
    uint64_t minIntervalMs_;
    uint64_t lastSentMs_ = 0;
    bool hasPending_ = false;
    uint16_t pendingX_ = 0;
    uint16_t pendingY_ = 0;
    uint8_t pendingButtons_ = 0;
};

}  // namespace input
}  // namespace espview
