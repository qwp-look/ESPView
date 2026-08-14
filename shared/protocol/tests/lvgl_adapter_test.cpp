// ESPView M5-B — LVGL Input Adapter 宿主测试（纯 C++17，不依赖 LVGL）。
//
// 规范来源：M5-B 任务书 §19（15 项）+ §8/§9/§10/§11/§14/§15/§16/§23。
// 验证对象是 shared/input 的 LVGL-independent Adapter（LvglInputAdapter）与
// HID→LVGL 映射（HidToLvglKeyMapper）；LVGL 真实 indev 由 ESP32 侧 lvgl_indev
// 薄胶水承载，host 侧不引入 LVGL。

#include <cstdint>
#include <cstdio>

#include "test_util.h"

#include "hid_lvgl_keymap.h"
#include "input_event.h"
#include "input_manager.h"
#include "lvgl_adapter.h"

namespace {

using espview::input::HidToLvglKeyMapper;
using espview::input::InputEvent;
using espview::input::InputManager;
using espview::input::InputType;
using espview::input::LvglAdapterStats;
using espview::input::LvglInputAdapter;
using espview::input::LvglKeyEvent;
using espview::input::LvglPointerState;
using espview::input::kLvglKeyBackspace;
using espview::input::kLvglKeyDel;
using espview::input::kLvglKeyDown;
using espview::input::kLvglKeyEnd;
using espview::input::kLvglKeyEnter;
using espview::input::kLvglKeyEsc;
using espview::input::kLvglKeyHome;
using espview::input::kLvglKeyLeft;
using espview::input::kLvglKeyNext;
using espview::input::kLvglKeyRight;
using espview::input::kLvglKeyUp;
using espview::input::kMouseLeft;
using espview::input::kMouseMiddle;
using espview::input::kMouseRight;
using espview::input::makeKeyEvent;
using espview::input::makeMouseButton;
using espview::input::makeMouseMove;
using espview::input::makeMouseWheel;

constexpr uint16_t kW = 320;
constexpr uint16_t kH = 240;

InputEvent move(uint16_t x, uint16_t y, uint8_t buttons = 0) {
    return makeMouseMove(x, y, buttons, 0);
}

// 消费点击保持窗口剩余次数：保持期内 read_cb 连续返回 PRESSED（冻结点击点），
// 保持窗口结束后返回 RELEASED（点击稳定命中，§8 补充语义）。
// 若调用方已先读过一次，传入 remaining = kPointerClickHoldReads - 1。
void drainClickHold(LvglInputAdapter& a,
                    uint8_t remaining = LvglInputAdapter::kPointerClickHoldReads) {
    for (uint8_t i = 0; i < remaining; ++i) {
        CHECK(a.pointerState().leftPressed);
    }
    CHECK(!a.pointerState().leftPressed);
}

// ---- 1. mouse move ----
void testMouseMove() {
    std::printf("  mouse move\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(move(10, 20));
    a.onInputEvent(move(160, 120, kMouseLeft));  // 按下状态移动
    const LvglPointerState st = a.pointerState();
    CHECK_EQ(st.x, 160);
    CHECK_EQ(st.y, 120);
    CHECK(st.leftPressed);
    const LvglAdapterStats s = a.stats();
    CHECK_EQ(s.moves, 2u);
    CHECK_EQ(s.downEvents, 0u);  // InputManager 负责 Down/Up 推导；Adapter 只消费掩码
}

// ---- 2. left press ----
void testLeftPress() {
    std::printf("  left press\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 5, 6, kMouseLeft, 0));
    const LvglPointerState st = a.pointerState();
    CHECK(st.leftPressed);
    CHECK_EQ(st.x, 5);
    CHECK_EQ(st.y, 6);
    CHECK_EQ(a.stats().downEvents, 1u);
}

// ---- 3. left release（Up 后保持 PRESSED 若干 read 周期 → 再释放）----
void testLeftRelease() {
    std::printf("  left release\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 5, 6, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 5, 6, 0, 0));
    // Up 到达后不立即释放：保持窗口内 read_cb 仍返回 PRESSED（冻结点击点）。
    const LvglPointerState held = a.pointerState();
    CHECK(held.leftPressed);
    CHECK_EQ(held.x, 5);
    CHECK_EQ(held.y, 6);
    drainClickHold(a, LvglInputAdapter::kPointerClickHoldReads - 1);  // 剩余窗口
    const LvglPointerState released = a.pointerState();
    CHECK(!released.leftPressed);
    CHECK_EQ(released.x, 5);
    CHECK_EQ(released.y, 6);
    CHECK_EQ(a.stats().downEvents, 1u);
    CHECK_EQ(a.stats().upEvents, 1u);
}

// ---- 4. wheel ----
void testWheel() {
    std::printf("  wheel\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseWheel(100, 100, 1, 0, 0));
    a.onInputEvent(makeMouseWheel(100, 100, 2, 0, 0));
    CHECK_EQ(a.consumeWheelDiff(), 3);
    CHECK_EQ(a.consumeWheelDiff(), 0);  // 已清空
    a.onInputEvent(makeMouseWheel(100, 100, -1, 0, 0));
    CHECK_EQ(a.consumeWheelDiff(), -1);
    CHECK_EQ(a.stats().wheelEvents, 3u);
    CHECK_EQ(a.stats().wheelSteps, 2);  // 1 + 2 - 1
}

// ---- 5. key down ----
void testKeyDown() {
    std::printf("  key down\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0));  // A
    CHECK(a.hasPendingKey());
    LvglKeyEvent ev;
    CHECK(a.nextKeyEvent(ev));
    CHECK_EQ(ev.key, static_cast<uint32_t>('A'));
    CHECK(ev.pressed);
    CHECK(!a.hasPendingKey());
    CHECK_EQ(a.stats().keyDowns, 1u);
    CHECK_EQ(a.stats().consumedKeys, 1u);
}

// ---- 6. key up ----
void testKeyUp() {
    std::printf("  key up\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0));
    a.onInputEvent(makeKeyEvent(InputType::kKeyUp, 0x04, 0, 0));
    LvglKeyEvent ev;
    CHECK(a.nextKeyEvent(ev));
    CHECK_EQ(ev.key, static_cast<uint32_t>('A'));
    CHECK(ev.pressed);
    CHECK(a.nextKeyEvent(ev));
    CHECK_EQ(ev.key, static_cast<uint32_t>('A'));
    CHECK(!ev.pressed);
    CHECK(!a.hasPendingKey());
}

// ---- 7. rapid down/up（polling race 不丢事件）----
void testRapidDownUp() {
    std::printf("  rapid down/up\n");
    LvglInputAdapter a(kW, kH);
    // 两次 read 之间完成 Down+Up：ring buffer 保留两事件，LVGL 依次看到 PRESS/REL。
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x05, 0, 0));  // B
    a.onInputEvent(makeKeyEvent(InputType::kKeyUp, 0x05, 0, 0));
    LvglKeyEvent ev;
    CHECK(a.nextKeyEvent(ev));
    CHECK(ev.pressed);
    CHECK_EQ(ev.key, static_cast<uint32_t>('B'));
    CHECK(a.nextKeyEvent(ev));
    CHECK(!ev.pressed);
    CHECK_EQ(ev.key, static_cast<uint32_t>('B'));
    CHECK(!a.hasPendingKey());
}

// ---- 8. multiple events（交错鼠标/键盘/滚轮）----
void testMultipleEvents() {
    std::printf("  multiple events\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(move(1, 1));
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 1, 1, kMouseLeft, 0));
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x06, 0, 0));  // C
    a.onInputEvent(makeKeyEvent(InputType::kKeyUp, 0x06, 0, 0));
    a.onInputEvent(makeMouseWheel(1, 1, 1, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 1, 1, 0, 0));

    drainClickHold(a);  // 消费点击保持窗口后 → RELEASED
    CHECK(!a.pointerState().leftPressed);
    LvglKeyEvent ev;
    CHECK(a.nextKeyEvent(ev));
    CHECK(ev.pressed);
    CHECK(a.nextKeyEvent(ev));
    CHECK(!ev.pressed);
    CHECK_EQ(a.consumeWheelDiff(), 1);
    const LvglAdapterStats s = a.stats();
    CHECK_EQ(s.moves, 1u);
    CHECK_EQ(s.downEvents, 1u);
    CHECK_EQ(s.upEvents, 1u);
    CHECK_EQ(s.wheelEvents, 1u);
    CHECK_EQ(s.keyDowns, 1u);
    CHECK_EQ(s.keyUps, 1u);
}

// ---- 9. reset ----
void testReset() {
    std::printf("  reset\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 10, 10, kMouseLeft, 0));
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0));
    a.onInputEvent(makeMouseWheel(10, 10, 3, kMouseLeft, 0));
    a.reset();
    CHECK(!a.pointerState().leftPressed);
    CHECK(!a.hasPendingKey());
    CHECK_EQ(a.consumeWheelDiff(), 0);
    CHECK_EQ(a.stats().resets, 1u);
}

// ---- 10. reconnect（断线恢复：resetState 补发 release + adapter.reset）----
void testReconnect() {
    std::printf("  reconnect\n");
    LvglInputAdapter a(kW, kH);
    InputManager mgr(kW, kH);
    mgr.registerListener(&a);
    // 按住 A + Left
    mgr.feed(makeMouseMove(50, 50, 0, 0));
    mgr.feed(makeMouseMove(50, 50, kMouseLeft, 0));
    mgr.feed(makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0));
    CHECK(a.pointerState().leftPressed);
    // 断线：resetState() 本地补发 KeyUp/MouseUp → adapter 消费；随后 adapter.reset()
    mgr.resetState();
    a.reset();
    CHECK(!a.pointerState().leftPressed);
    CHECK(!a.hasPendingKey());
    CHECK_EQ(a.stats().resets, 1u);
    // InputManager 统计：stuck release 已计数
    const auto ms = mgr.stats();
    CHECK_EQ(ms.stuckKeysReleased, 1u);
    CHECK_EQ(ms.stuckButtonsReleased, 1u);
}

// ---- 11. invalid coordinates（第二层校验，§23）----
void testInvalidCoordinates() {
    std::printf("  invalid coordinates\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(move(kW, 0));          // x == width → 越界
    a.onInputEvent(move(0, kH));          // y == height → 越界
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 0, kH, kMouseLeft, 0));
    CHECK_EQ(a.stats().invalidEvents, 3u);
    CHECK_EQ(a.stats().moves, 0u);
    CHECK_EQ(a.stats().downEvents, 0u);
    const LvglPointerState st = a.pointerState();
    CHECK_EQ(st.x, 0);
    CHECK_EQ(st.y, 0);
    CHECK(!st.leftPressed);
}

// ---- 12. pending event consumption ----
void testPendingConsumption() {
    std::printf("  pending event consumption\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x07, 0, 0));  // D
    CHECK(a.hasPendingKey());
    LvglKeyEvent ev;
    CHECK(a.nextKeyEvent(ev));
    CHECK(!a.hasPendingKey());       // 消费后无 pending
    CHECK(!a.nextKeyEvent(ev));      // 空队列返回 false
    CHECK_EQ(a.stats().consumedKeys, 1u);
}

// ---- 13. HID → LVGL key mapping ----
void testHidToLvglMapping() {
    std::printf("  HID -> LVGL mapping\n");
    uint32_t k = 0;
    // 字母 / 数字 → ASCII
    CHECK(HidToLvglKeyMapper::mapKey(0x04, k) && k == static_cast<uint32_t>('A'));
    CHECK(HidToLvglKeyMapper::mapKey(0x1D, k) && k == static_cast<uint32_t>('Z'));
    CHECK(HidToLvglKeyMapper::mapKey(0x1E, k) && k == static_cast<uint32_t>('1'));
    CHECK(HidToLvglKeyMapper::mapKey(0x27, k) && k == static_cast<uint32_t>('0'));
    CHECK(HidToLvglKeyMapper::mapKey(0x2C, k) && k == static_cast<uint32_t>(' '));
    // 控制键 → LV_KEY_*
    CHECK(HidToLvglKeyMapper::mapKey(0x28, k) && k == kLvglKeyEnter);
    CHECK(HidToLvglKeyMapper::mapKey(0x29, k) && k == kLvglKeyEsc);
    CHECK(HidToLvglKeyMapper::mapKey(0x2A, k) && k == kLvglKeyBackspace);
    CHECK(HidToLvglKeyMapper::mapKey(0x2B, k) && k == kLvglKeyNext);  // Tab
    // 方向键（HID 顺序：Right=0x4F Left=0x50 Down=0x51 Up=0x52）
    CHECK(HidToLvglKeyMapper::mapKey(0x4F, k) && k == kLvglKeyRight);
    CHECK(HidToLvglKeyMapper::mapKey(0x50, k) && k == kLvglKeyLeft);
    CHECK(HidToLvglKeyMapper::mapKey(0x51, k) && k == kLvglKeyDown);
    CHECK(HidToLvglKeyMapper::mapKey(0x52, k) && k == kLvglKeyUp);
    CHECK(HidToLvglKeyMapper::mapKey(0x4A, k) && k == kLvglKeyHome);
    CHECK(HidToLvglKeyMapper::mapKey(0x4D, k) && k == kLvglKeyEnd);
    CHECK(HidToLvglKeyMapper::mapKey(0x4C, k) && k == kLvglKeyDel);
    // 不映射：修饰键 / F 键 / PageUp
    CHECK(!HidToLvglKeyMapper::mapKey(0xE0, k));  // Left Ctrl
    CHECK(!HidToLvglKeyMapper::mapKey(0xE7, k));  // Right GUI
    CHECK(!HidToLvglKeyMapper::mapKey(0x3A, k));  // F1
    CHECK(!HidToLvglKeyMapper::mapKey(0x4B, k));  // PageUp
    // keyName
    CHECK(HidToLvglKeyMapper::keyName(kLvglKeyEnter) != nullptr);
    CHECK(HidToLvglKeyMapper::keyName(static_cast<uint32_t>('A')) != nullptr);
}

// ---- 14. no stuck key（重复 Down/Up 不产生 stuck）----
void testNoStuckKey() {
    std::printf("  no stuck key\n");
    LvglInputAdapter a(kW, kH);
    // 重复 Down（PC 已抑制 autoRepeat，防御）→ 两个 PRESS；重复 Up → 两个 REL。
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0));
    a.onInputEvent(makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0));
    a.onInputEvent(makeKeyEvent(InputType::kKeyUp, 0x04, 0, 0));
    a.onInputEvent(makeKeyEvent(InputType::kKeyUp, 0x04, 0, 0));
    LvglKeyEvent ev;
    int presses = 0, releases = 0;
    while (a.nextKeyEvent(ev)) {
        ev.pressed ? ++presses : ++releases;
    }
    CHECK_EQ(presses, 2);
    CHECK_EQ(releases, 2);
    CHECK(!a.hasPendingKey());
    // LVGL 状态机角度：最后一个事件是 REL → 无 stuck press。
}

// ---- 15. no stuck mouse（重复 Down/Up 掩码推导）----
void testNoStuckMouse() {
    std::printf("  no stuck mouse\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 1, 1, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 1, 1, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 1, 1, 0, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 1, 1, 0, 0));
    drainClickHold(a);  // 消费保持窗口后 → RELEASED（无 stuck press）
    CHECK(!a.pointerState().leftPressed);
    CHECK_EQ(a.stats().downEvents, 2u);
    CHECK_EQ(a.stats().upEvents, 2u);
}

// ---- RIGHT/MIDDLE：记录但不消费（§8）----
void testRightMiddleNotConsumed() {
    std::printf("  right/middle recorded-not-consumed\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 2, 2, kMouseRight, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 2, 2, kMouseMiddle, 0));
    CHECK(!a.pointerState().leftPressed);  // 不映射成 pointer press
    CHECK_EQ(a.stats().ignoredButtons, 2u);
}

// ---- InputManager → Adapter 集成（真实 Down/Up 推导链路）----
void testManagerToAdapter() {
    std::printf("  InputManager -> Adapter integration\n");
    LvglInputAdapter a(kW, kH);
    InputManager mgr(kW, kH);
    mgr.registerListener(&a);
    // 掩码变化 → InputManager 推导 MouseDown / MouseUp → Adapter
    mgr.feed(makeMouseMove(10, 10, 0, 0));
    mgr.feed(makeMouseMove(10, 10, kMouseLeft, 0));  // Down
    mgr.feed(makeMouseMove(20, 20, kMouseLeft, 0));  // Move
    mgr.feed(makeMouseMove(20, 20, 0, 0));           // Up
    mgr.feed(makeMouseWheel(20, 20, 2, 0, 0));       // Wheel
    drainClickHold(a);  // 消费保持窗口后 → RELEASED
    CHECK(!a.pointerState().leftPressed);
    CHECK_EQ(a.stats().downEvents, 1u);
    CHECK_EQ(a.stats().upEvents, 1u);
    CHECK_EQ(a.stats().moves, 2u);
    CHECK_EQ(a.consumeWheelDiff(), 2);
    // 键盘：A down/up 完整到达
    mgr.feed(makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0));
    mgr.feed(makeKeyEvent(InputType::kKeyUp, 0x04, 0, 0));
    LvglKeyEvent ev;
    CHECK(a.nextKeyEvent(ev));
    CHECK(ev.pressed);
    CHECK(a.nextKeyEvent(ev));
    CHECK(!ev.pressed);
    CHECK(!a.hasPendingKey());
    // 非法坐标被 InputManager 拦截（invalidEvents 在 Manager 侧，Adapter 无感知）
    CHECK_EQ(mgr.stats().invalidEvents, 0u);
}

// ---- 16. click hold（核心回归：Down+Up 落在两次轮询之间，点击不能丢）----
void testClickHold() {
    std::printf("  click hold (sub-poll click)\n");
    LvglInputAdapter a(kW, kH);
    // 两次 read_cb 之间完成 Down+Up（真实 GUI 点击的典型时序）。
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 95, 65, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 95, 65, 0, 0));
    // 保持窗口内：LVGL 至少观察到一次 PRESSED（冻结点击点）→ 点击命中。
    const LvglPointerState p1 = a.pointerState();
    CHECK(p1.leftPressed);
    CHECK_EQ(p1.x, 95);
    CHECK_EQ(p1.y, 65);
    // 保持窗口结束后：RELEASED，点击点与按下位置一致 → LV_EVENT_CLICKED。
    drainClickHold(a, LvglInputAdapter::kPointerClickHoldReads - 1);
    const LvglPointerState p2 = a.pointerState();
    CHECK(!p2.leftPressed);
    CHECK_EQ(p2.x, 95);
    CHECK_EQ(p2.y, 65);
}

// ---- 17. 保持期间 Move 不提前释放；点击点冻结 ----
void testClickHoldMoveDuringHold() {
    std::printf("  click hold move during hold\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 50, 50, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 50, 50, 0, 0));
    a.onInputEvent(move(200, 150));  // 保持期间 Move（buttons=0）→ 只更新实时坐标
    const LvglPointerState held = a.pointerState();
    CHECK(held.leftPressed);
    CHECK_EQ(held.x, 50);   // 冻结点击点
    CHECK_EQ(held.y, 50);
    drainClickHold(a, LvglInputAdapter::kPointerClickHoldReads - 1);
    const LvglPointerState released = a.pointerState();
    CHECK(!released.leftPressed);
    CHECK_EQ(released.x, 200);  // 释放后回到实时坐标
    CHECK_EQ(released.y, 150);
    CHECK_EQ(a.stats().moves, 1u);
}

// ---- 18. 保持期间新 Down 取消未决释放（连续点击不卡）----
void testClickHoldNewDown() {
    std::printf("  click hold new down cancels\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 10, 10, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 10, 10, 0, 0));  // 进入保持
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 12, 12, kMouseLeft, 0));
    CHECK(a.pointerState().leftPressed);  // 未提前释放
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 12, 12, 0, 0));
    drainClickHold(a);
    CHECK(!a.pointerState().leftPressed);
}

// ---- 19. 保持期间 reset → 立即 RELEASED（断线不卡）----
void testClickHoldReset() {
    std::printf("  click hold reset\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 10, 10, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 10, 10, 0, 0));  // 保持中
    a.reset();
    CHECK(!a.pointerState().leftPressed);
    CHECK(!a.pointerState().leftPressed);  // 后续 read 不再出现幽灵 PRESS
    CHECK_EQ(a.stats().resets, 1u);
}

// ---- 20. peekPointer 不消费保持窗口（UI 标签只读）----
void testPeekPointer() {
    std::printf("  peek pointer no consume\n");
    LvglInputAdapter a(kW, kH);
    a.onInputEvent(makeMouseButton(InputType::kMouseDown, 30, 40, kMouseLeft, 0));
    a.onInputEvent(makeMouseButton(InputType::kMouseUp, 30, 40, 0, 0));
    // 多次 peek（UI 5Hz 标签）不推进保持窗口。
    for (int i = 0; i < 8; ++i) {
        const LvglPointerState st = a.peekPointer();
        CHECK(st.leftPressed);
        CHECK_EQ(st.x, 30);
        CHECK_EQ(st.y, 40);
    }
    // LVGL read_cb 仍获得完整保持窗口 → 点击不丢。
    const LvglPointerState p1 = a.pointerState();
    CHECK(p1.leftPressed);
    CHECK_EQ(p1.x, 30);
    CHECK_EQ(p1.y, 40);
    drainClickHold(a, LvglInputAdapter::kPointerClickHoldReads - 1);
    CHECK(!a.pointerState().leftPressed);
}

}  // namespace

void runLvglAdapterTests() {
    std::printf("[lvgl_adapter]\n");
    testMouseMove();
    testLeftPress();
    testLeftRelease();
    testWheel();
    testKeyDown();
    testKeyUp();
    testRapidDownUp();
    testMultipleEvents();
    testReset();
    testReconnect();
    testInvalidCoordinates();
    testPendingConsumption();
    testHidToLvglMapping();
    testNoStuckKey();
    testNoStuckMouse();
    testRightMiddleNotConsumed();
    testManagerToAdapter();
    testClickHold();
    testClickHoldMoveDuringHold();
    testClickHoldNewDown();
    testClickHoldReset();
    testPeekPointer();
}
