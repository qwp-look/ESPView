// ESPView M5-B — LVGL v8.4 Input Device 注册（POINTER / KEYPAD / ENCODER）。
//
// 职责：把 shared/input LvglInputAdapter（LVGL 无关状态）接到 LVGL indev 轮询
// read_cb。三个 indev 共享同一 adapter：
//   - POINTER：pointerState() → data.point + data.state（LEFT → PRESSED/RELEASED）；
//   - KEYPAD：nextKeyEvent() 逐条弹出 HID→LVGL key 事件，continue_reading 让
//     Down/Up 快速序列在同一 timer cycle 内按序处理（§16）；
//   - ENCODER：consumeWheelDiff() → data.enc_diff（state 恒 RELEASED：
//     LVGL v8.4 indev_encoder_proc 只在 RELEASED 时处理 enc_diff，§9）。
//
// 线程：read_cb 全部在 LVGL 任务执行；adapter 内部互斥，非阻塞（§7/§17）。
// 本文件只调用 LVGL v8.4 已确认 API（lv_indev_drv_init / lv_indev_drv_register
// / lv_indev_set_group；v8.4 无 lv_indev_register 这个入口）。

#pragma once

#include "lvgl.h"

#include "lvgl_adapter.h"

namespace espview {

// 注册三个 indev 并把 KEYPAD/ENCODER 绑定到 group（demo UI 创建后调用一次，
// 必须在 LVGL 初始化后、UI 任务启动前；由 lvgl_port.cpp start() 调用）。
void lvglIndevRegister(lv_disp_t* disp, input::LvglInputAdapter* adapter, lv_group_t* group);

}  // namespace espview
