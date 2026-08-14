// ESPView M5-B — LVGL 交互测试 UI 实现（见 lvgl_app.hpp）。

#include "lvgl_port/lvgl_app.hpp"

#include <cstdio>
#include <cstring>

#include "hid_lvgl_keymap.h"

namespace espview {

namespace {

constexpr lv_color_t kBgColor = LV_COLOR_MAKE(0x10, 0x20, 0x30);
constexpr lv_color_t kBtnColor = LV_COLOR_MAKE(0x20, 0x50, 0x90);
constexpr lv_color_t kBtnOnColor = LV_COLOR_MAKE(0x90, 0x50, 0x20);
constexpr lv_color_t kTextColor = LV_COLOR_MAKE(0xE0, 0xE0, 0xE0);
constexpr lv_color_t kDimTextColor = LV_COLOR_MAKE(0x90, 0xA0, 0xB0);
constexpr int32_t kCounterMin = -99;
constexpr int32_t kCounterMax = 999;

}  // namespace

LvglDemoApp::LvglDemoApp(input::LvglInputAdapter* inputAdapter) : input_(inputAdapter) {}

void LvglDemoApp::create() {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, kBgColor, LV_STATE_DEFAULT);

    lv_obj_t* title = lv_label_create(scr);
    lv_obj_set_style_text_color(title, kTextColor, LV_STATE_DEFAULT);
    lv_label_set_text(title, "ESPView LVGL Input Test");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // ---- Button A / Button B（LV_EVENT_CLICKED 同时覆盖鼠标点击与键盘 Enter）----
    btnA_ = lv_btn_create(scr);
    lv_obj_set_size(btnA_, 110, 40);
    lv_obj_align(btnA_, LV_ALIGN_CENTER, -65, -55);
    lv_obj_set_style_bg_color(btnA_, kBtnColor, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btnA_, &LvglDemoApp::onEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(btnA_, &LvglDemoApp::onEvent, LV_EVENT_KEY, this);
    lv_obj_t* la = lv_label_create(btnA_);
    lv_obj_center(la);
    lv_label_set_text(la, "Button A");

    btnB_ = lv_btn_create(scr);
    lv_obj_set_size(btnB_, 110, 40);
    lv_obj_align(btnB_, LV_ALIGN_CENTER, 65, -55);
    lv_obj_set_style_bg_color(btnB_, kBtnColor, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(btnB_, &LvglDemoApp::onEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(btnB_, &LvglDemoApp::onEvent, LV_EVENT_KEY, this);
    lv_obj_t* lb = lv_label_create(btnB_);
    lv_obj_center(lb);
    lv_label_set_text(lb, "Button B");

    // ---- Counter（可点击/可聚焦对象：点击聚焦 → 滚轮/左右键增减）----
    counterObj_ = lv_obj_create(scr);
    lv_obj_set_size(counterObj_, 160, 40);
    lv_obj_align(counterObj_, LV_ALIGN_CENTER, 0, 10);
    lv_obj_clear_flag(counterObj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(counterObj_, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_color(counterObj_, LV_COLOR_MAKE(0x18, 0x28, 0x38), LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(counterObj_, 1, LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(counterObj_, LV_COLOR_MAKE(0x38, 0x50, 0x68), LV_STATE_DEFAULT);
    lv_obj_add_event_cb(counterObj_, &LvglDemoApp::onEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(counterObj_, &LvglDemoApp::onEvent, LV_EVENT_KEY, this);
    counterLabel_ = lv_label_create(counterObj_);
    lv_obj_center(counterLabel_);
    lv_obj_set_style_text_color(counterLabel_, kTextColor, LV_STATE_DEFAULT);
    lv_label_set_text(counterLabel_, "Counter: 0");

    // ---- 状态标签 ----
    keyLabel_ = lv_label_create(scr);
    lv_obj_align(keyLabel_, LV_ALIGN_CENTER, 0, 65);
    lv_obj_set_style_text_color(keyLabel_, kDimTextColor, LV_STATE_DEFAULT);
    lv_label_set_text(keyLabel_, "Keyboard: NONE");

    mouseLabel_ = lv_label_create(scr);
    lv_obj_align(mouseLabel_, LV_ALIGN_CENTER, 0, 90);
    lv_obj_set_style_text_color(mouseLabel_, kDimTextColor, LV_STATE_DEFAULT);
    lv_label_set_text(mouseLabel_, "Mouse: 0,0");

    // ---- Group：键盘/滚轮 focus 导航目标 ----
    group_ = lv_group_create();
    lv_group_set_default(group_);
    lv_group_add_obj(group_, btnA_);
    lv_group_add_obj(group_, btnB_);
    lv_group_add_obj(group_, counterObj_);
    // ENCODER 编辑模式：wheel(enc_diff) → LV_KEY_LEFT/RIGHT 到 focused 对象
    //（导航模式下 enc_diff 只移动 focus，无法驱动 Counter 增减，§9/§12）。
    lv_group_set_editing(group_, true);
    lv_group_focus_obj(btnA_);

    lv_timer_create(&LvglDemoApp::timerCb, 200, this);  // Mouse 标签 5Hz 轮询
}

void LvglDemoApp::timerCb(lv_timer_t* timer) {
    auto* self = static_cast<LvglDemoApp*>(timer->user_data);
    self->update();
}

void LvglDemoApp::update() {
    updateMouseLabel();
}

void LvglDemoApp::updateMouseLabel() {
    if (input_ == nullptr) {
        return;
    }
    // peek：只读状态，不消费点击保持窗口（LVGL read_cb 才消费）。
    const input::LvglPointerState st = input_->peekPointer();
    if (st.x == lastMouseX_ && st.y == lastMouseY_) {
        return;  // 坐标未变：不触发重绘（避免 5Hz 无意义 dirty rect）
    }
    lastMouseX_ = st.x;
    lastMouseY_ = st.y;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Mouse: %d,%d", static_cast<int>(st.x),
                  static_cast<int>(st.y));
    lv_label_set_text(mouseLabel_, buf);
}

void LvglDemoApp::onEvent(lv_event_t* e) {
    auto* self = static_cast<LvglDemoApp*>(lv_event_get_user_data(e));
    if (self == nullptr) {
        return;
    }
    if (e->code == LV_EVENT_CLICKED) {
        self->onPointerClick(lv_event_get_target(e));
    } else if (e->code == LV_EVENT_KEY) {
        self->onKeyEvent(lv_event_get_key(e));
    }
}

void LvglDemoApp::onPointerClick(lv_obj_t* target) {
    // 点击即聚焦（键盘/滚轮随后作用于该对象）。
    lv_group_focus_obj(target);
    if (target == btnA_) {
        aState_ = !aState_;
        updateButtonState(btnA_, aState_);
    } else if (target == btnB_) {
        bState_ = !bState_;
        updateButtonState(btnB_, bState_);
    }
}

void LvglDemoApp::updateButtonState(lv_obj_t* btn, bool on) {
    if (on) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, kBtnOnColor, LV_STATE_DEFAULT);
    } else {
        lv_obj_clear_state(btn, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, kBtnColor, LV_STATE_DEFAULT);
    }
}

void LvglDemoApp::onKeyEvent(uint32_t lvglKey) {
    // 键盘标签：任意键按下都显示（A/B → 'A'/'B'，方向键/Enter → 名称）。
    setKeyLabel(lvglKey);

    // 方向键导航 / Counter 增减：
    //   - focused == Counter：LEFT/RIGHT/UP/DOWN → 计数增减；
    //   - focused == 按钮：方向键 → focus 导航（键盘 arrows 与滚轮 enc_diff
    //     都经 LV_EVENT_KEY 到达这里，§12）。
    const bool isLeft = lvglKey == input::kLvglKeyLeft || lvglKey == input::kLvglKeyUp;
    const bool isRight = lvglKey == input::kLvglKeyRight || lvglKey == input::kLvglKeyDown;
    if (!isLeft && !isRight) {
        return;
    }
    lv_obj_t* focused = lv_group_get_focused(group_);
    if (focused == counterObj_) {
        setCounter(isRight ? 1 : -1);
    } else if (focused != nullptr) {
        if (isRight) {
            lv_group_focus_next(group_);
        } else {
            lv_group_focus_prev(group_);
        }
    }
}

void LvglDemoApp::setCounter(int32_t delta) {
    const int32_t next = counter_ + delta;
    if (next < kCounterMin || next > kCounterMax) {
        return;
    }
    counter_ = next;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Counter: %d", static_cast<int>(counter_));
    lv_label_set_text(counterLabel_, buf);
}

void LvglDemoApp::setKeyLabel(uint32_t lvglKey) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Keyboard: %s",
                  input::HidToLvglKeyMapper::keyName(lvglKey));
    if (std::strcmp(lv_label_get_text(keyLabel_), buf) != 0) {
        lv_label_set_text(keyLabel_, buf);
    }
}

}  // namespace espview
