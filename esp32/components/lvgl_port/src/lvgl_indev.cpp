// ESPView M5-B — LVGL v8.4 Input Device 实现（见 lvgl_indev.hpp）。

#include "lvgl_port/lvgl_indev.hpp"

namespace espview {
namespace {

// POINTER：显示坐标直接映射（InputManager 已做坐标换算/校验，§14），
// LEFT 按下 → PRESSED；RIGHT/MIDDLE 由 Adapter 记录但不消费（§8）。
void pointerReadCb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    auto* adapter = static_cast<input::LvglInputAdapter*>(drv->user_data);
    const input::LvglPointerState st = adapter->pointerState();
    data->point.x = st.x;
    data->point.y = st.y;
    data->state = st.leftPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// KEYPAD：每次 read 消费一条键事件；队列还有内容时置 continue_reading，
// LVGL 在同一 read timer cycle 内立即再次调用本回调（Down/Up 不因轮询丢失）。
void keypadReadCb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    auto* adapter = static_cast<input::LvglInputAdapter*>(drv->user_data);
    input::LvglKeyEvent ev;
    if (adapter->nextKeyEvent(ev)) {
        data->key = ev.key;
        data->state = ev.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = adapter->hasPendingKey();
    } else {
        data->key = 0;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = false;
    }
}

// ENCODER（Wheel）：累积滚轮格数 → enc_diff；state 必须为 RELEASED
// （lv_indev.c indev_encoder_proc 在 PRESSED 时强制 enc_diff=0，§9）。
void encoderReadCb(lv_indev_drv_t* drv, lv_indev_data_t* data) {
    auto* adapter = static_cast<input::LvglInputAdapter*>(drv->user_data);
    data->enc_diff = adapter->consumeWheelDiff();
    data->state = LV_INDEV_STATE_RELEASED;
    data->key = 0;
}

}  // namespace

// 重要：lv_indev_drv_register() 会把 indev->driver 指向调用者的驱动结构，之后
// read timer 每次读回（driver->type/disp/read_cb/user_data）。因此驱动结构必须
// 长期存活 —— 必须用 static，不能用栈局部变量（LVGL v8 常见误区，实测会导致
// 注册后首个 read timer 周期读到悬垂指针崩溃）。
void lvglIndevRegister(lv_disp_t* disp, input::LvglInputAdapter* adapter, lv_group_t* group) {
    static lv_indev_drv_t pointerDrv;
    static lv_indev_drv_t keypadDrv;
    static lv_indev_drv_t encoderDrv;

    lv_indev_drv_init(&pointerDrv);
    pointerDrv.type = LV_INDEV_TYPE_POINTER;
    pointerDrv.disp = disp;
    pointerDrv.read_cb = pointerReadCb;
    pointerDrv.user_data = adapter;
    lv_indev_drv_register(&pointerDrv);

    lv_indev_drv_init(&keypadDrv);
    keypadDrv.type = LV_INDEV_TYPE_KEYPAD;
    keypadDrv.disp = disp;
    keypadDrv.read_cb = keypadReadCb;
    keypadDrv.user_data = adapter;
    lv_indev_t* keypad = lv_indev_drv_register(&keypadDrv);
    if (keypad != nullptr && group != nullptr) {
        lv_indev_set_group(keypad, group);
    }

    lv_indev_drv_init(&encoderDrv);
    encoderDrv.type = LV_INDEV_TYPE_ENCODER;
    encoderDrv.disp = disp;
    encoderDrv.read_cb = encoderReadCb;
    encoderDrv.user_data = adapter;
    lv_indev_t* encoder = lv_indev_drv_register(&encoderDrv);
    if (encoder != nullptr && group != nullptr) {
        lv_indev_set_group(encoder, group);  // enc_diff 需要 group（编辑/导航模式）
    }
}

}  // namespace espview
