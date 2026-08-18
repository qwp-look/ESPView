// ESPView M5-B — LVGL 交互测试 UI（320x240）。
//
// 画面（M5-B 任务书 §12）：
//   ESPView LVGL Input Test
//   [ Button A ] [ Button B ]
//   Counter: 0
//   Keyboard: NONE
//   Mouse: 0,0
//
// 交互：
//   - 鼠标：移动 → "Mouse:" 标签（200ms 轮询 adapter，坐标变化才重绘）；
//     左键点击 Button A/B → 状态切换；点击任意可交互对象 → 获得 group focus；
//   - 键盘：A/B 等键 → "Keyboard:" 标签；Enter → 当前 focused button 触发
//     （LVGL keypad/encoder 默认 click 语义）；方向键 → focus 导航；
//   - 滚轮：经 ENCODER indev（enc_diff）→ group 编辑模式下发送
//     LV_KEY_LEFT/RIGHT 到 focused 对象；Counter 聚焦时滚轮增减计数，
//     按钮聚焦时左右键移动 focus。
//
// 事件一律来自 LVGL 任务（lv_timer_handler / read_cb），无跨线程 lv_* 调用。
// 本文件不依赖协议/传输；输入经 shared/input LvglInputAdapter 汇入。

#pragma once

#include <cstdint>

#include "lvgl.h"

#include "logical_scene.h"  // M8-B B2：共享 LogicalScene（语义镜像）
#include "lvgl_adapter.h"

namespace espview {

class LvglDemoApp {
public:
    explicit LvglDemoApp(input::LvglInputAdapter* inputAdapter);
    void create();  // LVGL 初始化后、UI 任务启动前调用一次
    lv_group_t* group() const { return group_; }

    // ---- M8-B B2：语义场景镜像（Physical OLED 消费；Virtual 侧 = LVGL 自身）----
    // 场景只在状态变化时重建（事件驱动）；LvglPort 在 flush 帧边界消费。
    const display::LogicalScene& scene() const { return scene_; }
    // 消费脏标记：返回 true 时重建 scene_（反映最新 widget 状态）并清除标记。
    // 同一 LVGL 任务调用（无跨线程竞争）。
    bool takeScene();

private:
    static void timerCb(lv_timer_t* timer);
    void update();  // 200ms：Mouse 标签（坐标变化才重绘）
    static void onEvent(lv_event_t* e);
    void onKeyEvent(uint32_t lvglKey);
    void onPointerClick(lv_obj_t* target);
    void updateButtonState(lv_obj_t* btn, bool on);
    void setCounter(int32_t delta);
    void setKeyLabel(uint32_t lvglKey);
    void updateMouseLabel();
    void buildScene();  // M8-B B2：从当前 widget 状态重建 LogicalScene
    void markSceneDirty() { sceneDirty_ = true; }  // M8-B B2：状态变化置脏

    input::LvglInputAdapter* input_ = nullptr;
    lv_obj_t* btnA_ = nullptr;
    lv_obj_t* btnB_ = nullptr;
    lv_obj_t* counterObj_ = nullptr;
    lv_obj_t* counterLabel_ = nullptr;
    lv_obj_t* keyLabel_ = nullptr;
    lv_obj_t* mouseLabel_ = nullptr;
    lv_group_t* group_ = nullptr;
    int32_t counter_ = 0;
    bool aState_ = false;
    bool bState_ = false;
    int16_t lastMouseX_ = -1;  // -1 = 尚未显示
    int16_t lastMouseY_ = -1;
    display::LogicalScene scene_;  // M8-B B2：语义场景镜像
    bool sceneDirty_ = true;       // M8-B B2：初始画面需要首帧推送
};

}  // namespace espview
