// ESPView M7-C1 — Display 能力描述（sink 侧值类型；纯 C++17，零平台依赖）。
//
// 定位：IDisplaySink（display_sink.h）的配套类型。DisplayRouter 用
// capabilities() 判断 sink 能承接什么内容（分辨率/格式/单色/回读），
// 不引入任何平台类型（无 ESP-IDF / Qt / Windows API）。
#pragma once

#include <cstdint>

#include "protocol.h"  // proto::PixelFormat

namespace espview {
namespace display {

// 显示 sink 类别（M7-C 抽象）：
//   kVirtual    —— PC 虚拟显示（LVGL → RemoteDisplay → PC 窗口）
//   kPhysical   —— ESP32 物理显示（128x64 SSD1306 1-bit；渲染是 C2）
//   kDiagnostic —— 诊断显示（M7-A/B OLED 状态页；不进入路由扇出）
enum class DisplaySinkKind : uint8_t {
    kVirtual = 0,
    kPhysical = 1,
    kDiagnostic = 2,
};

// sink 能力快照（值语义）。由 sink 在 init 后落定并返回；Router 只读。
// 字段说明：
//   width/height —— 逻辑分辨率（协议约束 1..4096；物理 OLED 为 128x64）
//   format       —— 像素格式（proto::PixelFormat；v0.1 仅 kRgb565）
//   color        —— 颜色深度（bpp）：RGB565=16；SSD1306 单色=1
//   mono         —— 1-bit 单色显示（物理 OLED）
//   canReadback  —— 是否支持像素读回（虚拟类后端可；OLED 不支持）
//   sinkKind     —— 本 sink 的类别
struct DisplayCapabilities {
    int width = 0;
    int height = 0;
    proto::PixelFormat format = proto::PixelFormat::kRgb565;
    uint8_t color = 16;      // 颜色深度（bpp）
    bool mono = false;
    bool canReadback = false;
    DisplaySinkKind sinkKind = DisplaySinkKind::kVirtual;
};

}  // namespace display
}  // namespace espview