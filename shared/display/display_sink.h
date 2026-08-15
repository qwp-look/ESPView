// ESPView M7-C1 — IDisplaySink：DisplayRouter 的消费侧接口（纯 C++17，零平台依赖）。
//
// 定位（M7-C）：把 Virtual Display（LVGL → RemoteDisplay → PC 窗口）与
// Physical Display（ESP32 OLED，128x64 SSD1306 1-bit）抽象为独立 Display
// Sink；DisplayRouter（display_router.h）把生产者的 writeRect/flush 扇出到
// 启用中的 sink。C1 只定义接口与状态语义；PhysicalDisplay 渲染是 C2。
//
// 约定：
//   - 错误路径全部使用 display::DisplayStatus（display.h），不抛异常；
//   - init(caps) 由 sink 拥有方在 attach 前调用：sink 校验请求的能力
//     （分辨率/格式）并落定自身 capabilities()；失败返回 kInvalidParam /
//     kNotSupported 且不改变状态；
//   - present() 的矩形为逻辑坐标系（生产者保证不越界）；实现必须同步完成
//     必要 staging，不得持有 pixels 指针；
//   - isAvailable() 是路由可用性（如 virtual=transport connected、
//     physical=I2C 存活），与 setEnabled()（路由开关）正交。
#pragma once

#include <cstdint>

#include "display.h"                // DisplayStatus
#include "display_capabilities.h"   // DisplayCapabilities

namespace espview {
namespace display {

// 逻辑矩形（生产者坐标系；x/y 为左上角，w/h >= 1）。
struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

class IDisplaySink {
public:
    virtual ~IDisplaySink() = default;

    // 初始化（caps = 生产者请求的能力）：校验并落定 capabilities()。
    virtual DisplayStatus init(const DisplayCapabilities& caps) = 0;
    // 本 sink 实际能力（init 成功后有效）。
    virtual const DisplayCapabilities& capabilities() const = 0;

    // 呈现一个逻辑矩形（调用方保证 pixels 在调用期间有效）。
    virtual DisplayStatus present(const Rect& rect, const uint8_t* pixels) = 0;
    // 提交当前帧（生产者渲染 cycle 结束）。
    virtual DisplayStatus flush() = 0;

    // 路由开关（Router 在 setMode 时统一 disable/enable）。
    virtual DisplayStatus setEnabled(bool enabled) = 0;
    // 路由可用性（与 setEnabled 正交；如 transport/I2C 状态）。
    virtual bool isAvailable() const = 0;
    // 最近一次操作状态（kOk = 正常；否则为最近一次错误）。
    virtual DisplayStatus status() const = 0;
};

}  // namespace display
}  // namespace espview