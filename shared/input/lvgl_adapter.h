// ESPView M5-B — LVGL Input Adapter（纯 C++17，**不包含 lvgl.h**）。
//
// 规范来源：M5-B 任务书 §4/§5/§6/§7/§8/§9/§10/§11/§14/§15/§16/§17/§23。
// 职责：InputManager（RX 任务）→ 本 Adapter（LVGL 无关状态）→ LVGL read_cb
//（LVGL 任务轮询消费）。
//
// 线程边界（§7，关键）：
//   - RX 任务（InputManager::feed() 在自身锁内调用 listener）→ onInputEvent()：
//     只在本 Adapter 内部互斥下更新状态，**绝不调用任何 lv_* API**；
//   - LVGL 任务 → pointerState() / nextKeyEvent() / consumeWheelDiff()：同样
//     互斥、非阻塞，供 read_cb 轮询消费。
//   两侧都持有短临界区，不持有协议锁、不阻塞 LVGL / TX / Display。
//
// Pointer（§8）：LEFT → LVGL pointer PRESSED/RELEASED；RIGHT/MIDDLE 由
// InputManager 保留并统计，本 Adapter 记录但不消费（LVGL v8 pointer 只有单一
// primary pressed 语义）。
//
// Wheel（§9）：wheelDelta 累积 → consumeWheelDiff()（LVGL ENCODER indev 的
// enc_diff）；+1/-1/+2/-2 必须原样到达。
//
// Keyboard（§10/§11）：HID usage → HidToLvglKeyMapper → LVGL key code，进入
// 有界 ring buffer（32 项）。LVGL keypad read_cb 每次消费一项并置
// continue_reading（同一 timer cycle 内按序处理），Down/Up 快速序列不会因
// 轮询 race 丢失。ring 满时丢弃**最新**项（丢 KeyDown 不会造成 stuck key：
// 后续 KeyUp 在 LVGL 中 prev_state 为 RELEASED 时被跳过）。
//
// Reset（§15）：断线/会话重置时 reset() 清空 pointer/key/wheel，避免 LVGL
// 继续看到旧 pressed 状态（InputManager::resetState() 先补发本地 release，
// 本 Adapter 随后整体清空，两者顺序无关）。
//
// 第二层校验（§23）：坐标/字段越界 → invalidEvents 计数并忽略，不 crash LVGL。

#pragma once

#include <array>
#include <cstdint>
#include <mutex>

#include "input_event.h"
#include "input_manager.h"  // IInputListener

namespace espview {
namespace input {

// LVGL POINTER read_cb 消费的瞬时状态。
struct LvglPointerState {
    int16_t x = 0;
    int16_t y = 0;
    bool leftPressed = false;  // LV_INDEV_STATE_PRESSED / RELEASED
};

// LVGL KEYPAD read_cb 消费的单条键事件（ring buffer 项）。
struct LvglKeyEvent {
    uint32_t key = 0;      // LVGL key code（HidToLvglKeyMapper 输出）
    bool pressed = false;  // true=PRESSED / false=RELEASED
};

// 统计（供 ERROR 文本通道上报 / host 测试断言；计数器不回绕）。
struct LvglAdapterStats {
    uint64_t moves = 0;           // MouseMove 接收（含按下状态携带的 Move）
    uint64_t downEvents = 0;      // LEFT MouseDown 接收
    uint64_t upEvents = 0;        // LEFT MouseUp 接收
    uint64_t wheelEvents = 0;     // MouseWheel 接收
    int64_t wheelSteps = 0;       // 累计滚轮格数（consumed 后保留累计）
    uint64_t ignoredButtons = 0;  // RIGHT/MIDDLE：记录但不消费
    uint64_t keyDowns = 0;        // 已映射 KeyDown
    uint64_t keyUps = 0;          // 已映射 KeyUp
    uint64_t unmappedKeys = 0;    // HID 无 LVGL 映射（F1..F12/PageUp/修饰键…）
    uint64_t invalidEvents = 0;   // 第二层校验拒绝（坐标/字段非法）
    uint64_t keyQueueDropped = 0; // ring 满丢弃（丢最新，避免 stuck）
    uint64_t consumedKeys = 0;    // LVGL 已消费的键事件数
    uint64_t resets = 0;          // reset() 调用次数
};

class LvglInputAdapter : public IInputListener {
public:
    // 点击保持窗口长度（LVGL read_cb 次数）：Up 后保持 PRESSED 这么多次读，
    // 再释放。必须 ≥1，保证 LVGL ~30ms 轮询至少观察到一次 PRESSED。
    static constexpr uint8_t kPointerClickHoldReads = 2;

    explicit LvglInputAdapter(uint16_t displayWidth = 320, uint16_t displayHeight = 240);

    // 显示分辨率（与 InputManager::setDisplaySize 同步；坐标校验上界）。
    void setDisplaySize(uint16_t width, uint16_t height);

    // ---- RX 任务（InputManager listener；非阻塞，绝不调用 lv_*）----
    void onInputEvent(const InputEvent& e) override;

    // ---- LVGL 任务（read_cb 轮询消费；互斥、非阻塞）----
    LvglPointerState pointerState();
    LvglPointerState peekPointer();  // 非消费读（UI 标签等只读用途，不推进保持窗口）
    bool hasPendingKey();
    bool nextKeyEvent(LvglKeyEvent& out);  // 弹出下一项；false = 空
    int16_t pendingWheelDiff();            // 当前累积（不消费）
    int16_t consumeWheelDiff();            // 取走全部累积（clamp int16，余量保留）

    // ---- 会话（断线/重连）----
    void reset();  // 清空 pointer/key/wheel

    LvglAdapterStats stats();
    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }

private:
    void feedPointer(const InputEvent& e);
    void feedKey(const InputEvent& e);
    void pushKey(uint32_t lvglKey, bool pressed);
    int16_t clampWheel();  // 已持锁：取走 wheelAccum_（clamp int16，余量保留）

    uint16_t width_;
    uint16_t height_;
    mutable std::mutex mutex_;

    // Pointer 状态（display 坐标系，已由 InputManager 校验）
    int16_t x_ = 0;
    int16_t y_ = 0;
    bool leftPressed_ = false;
    int32_t wheelAccum_ = 0;  // int8 增量累积（enc_diff int16 分次交付）

    // Pointer 点击保持（§8 补充）：LVGL read_cb 以 ~30ms 周期轮询，若
    // Down+Up 在一次轮询间隔内到达，LVGL 永远观察不到 PRESSED → 点击丢失。
    // Up 到达后保持 PRESSED 若干 read 周期（冻结点击点），确保 LVGL 至少
    // 观察到一次 PRESSED 再释放，点击命中稳定。peekPointer() 不消费保持。
    bool releasePending_ = false;  // Up 已到，保持 PRESSED 中
    uint8_t holdReadsLeft_ = 0;    // 剩余保持 read 次数
    int16_t holdX_ = 0;            // 冻结点击点（抬起位置）
    int16_t holdY_ = 0;

    // Keyboard ring buffer
    static constexpr size_t kKeyQueueCapacity = 32;
    std::array<LvglKeyEvent, kKeyQueueCapacity> keyQueue_{};
    size_t keyHead_ = 0;  // 队首（读）
    size_t keyCount_ = 0;

    LvglAdapterStats stats_;
};

}  // namespace input
}  // namespace espview
