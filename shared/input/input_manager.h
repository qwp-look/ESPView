// ESPView M3 — InputManager（ESP32 侧输入汇聚/校验/状态，纯 C++17）。
//
// 规范来源：docs/DESIGN.md B.4（InputManager / IInputListener）+ G 节 + spec §16-§20。
// 职责：
//   1. 接收 wire 解码后的 InputEvent（KeyDown/KeyUp 已定型；INPUT_MOUSE 解为
//      Move/Wheel + 按钮掩码）；
//   2. 校验（坐标 / buttons / modifiers / keycode / flags —— 防御性重复校验）；
//   3. 鼠标：INPUT_MOUSE 不带事件类型，本层按 buttons 掩码相对上一状态的
//      「变化位」推导 MouseDown / MouseUp（spec §14 状态一致性语义）；
//   4. 维护「当前输入状态」（pressed keys 集合 + 鼠标按钮掩码），用于
//      resetState() 断线安全恢复（spec §18/§19）；
//   5. 统计（eventsReceived / invalid / unsupported / reset / stuck 恢复数）。
//
// InputManager 不知道：Qt / Windows / COM3 / USB / LVGL（spec §16）。
// 断线/重连语义：Transport 断开或会话重置时由上层调用 resetState() —— 对所有
// 按下中的键补发本地 KeyUp、对按下中的鼠标按钮补发 MouseUp(buttons=0)，
// 绝不把 release 回发给 PC；HELLO 完成后状态即清空（与显示 FULL resync 独立）。
// 线程安全：feed() / resetState() / stats() 内部互斥（RX 任务与会话任务并发）。
// 约束：IInputListener 回调在锁内执行，不得重入 feed()/resetState()（仅做
// 日志/消费，如 LVGL input_read_cb 的收包）。

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "input_event.h"

namespace espview {
namespace input {

class IInputListener {
public:
    virtual ~IInputListener() = default;
    virtual void onInputEvent(const InputEvent& e) = 0;
};

// 输入统计（计数器不回绕；供 debug channel / ERROR 消息上报）。
struct InputStats {
    uint64_t eventsReceived = 0;       // feed() 收到的合法 wire 事件总数
    uint64_t validEvents = 0;          // 校验通过并转发给 listener 的事件数
    uint64_t invalidEvents = 0;        // 校验拒绝（坐标越界/非法字段）
    uint64_t unsupportedEvents = 0;    // Touch* 等保留类型（M3 不消费）
    uint64_t resetCount = 0;           // resetState() 调用次数（断线恢复）
    uint64_t stuckKeysReleased = 0;    // resetState() 本地补发的 KeyUp 数
    uint64_t stuckButtonsReleased = 0; // resetState() 本地补发的 MouseUp 数
    uint32_t pressedKeys = 0;          // 当前按下键数
    uint8_t pressedButtons = 0;        // 当前按钮掩码
    uint16_t lastX = 0;                // 最近一次鼠标位置
    uint16_t lastY = 0;
};

class InputManager {
public:
    explicit InputManager(uint16_t displayWidth = 320, uint16_t displayHeight = 240);

    void registerListener(IInputListener* l);
    // 显示分辨率（来自对端 HELLO / 本地配置；坐标校验上界）。
    void setDisplaySize(uint16_t width, uint16_t height);

    // 输入统一入口：校验 → 状态维护 → 转发 listener。非法/保留事件只计统计。
    // 线程安全（RX 任务 feed / 会话任务 resetState / 统计上报可并发）。
    void feed(const InputEvent& e);

    // 记录一次被 wire 解码拒绝的输入消息（payload/flags/坐标非法，spec §20）。
    void noteInvalidInput();

    // 断线安全恢复：本地补发 release（KeyUp / MouseUp），清空状态，不发送到 PC。
    void resetState();

    // 统计快照（线程安全，返回副本）。
    InputStats stats() const { std::lock_guard<std::mutex> lk(mutex_); return stats_; }

private:
    void feedLocked(const InputEvent& e);  // 已持锁
    bool validate(const InputEvent& e) const;
    void forward(const InputEvent& e);
    void applyMouseState(const InputEvent& raw);  // Move/Wheel → Down/Up 推导
    void setButtons(uint8_t buttons, uint16_t x, uint16_t y);

    uint16_t width_;
    uint16_t height_;
    IInputListener* listener_ = nullptr;
    InputStats stats_;
    std::vector<uint32_t> pressedKeys_;  // 当前按下键（HID usage，含修饰键）
    uint8_t buttons_ = 0;                // 当前鼠标按钮掩码
    mutable std::mutex mutex_;
};

}  // namespace input
}  // namespace espview
