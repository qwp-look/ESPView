// ESPView M7-B — OLED 状态快照与渲染（协议无关，纯 C++17，零平台依赖）。
//
// 布局与 M7-A esp32/components/oled/src/status_ui.cpp 逐行等价
// （8 行、8x8 字体、128x64）：
//   y=0   "ESPView"
//   y=8   "<UART|TCP> <OFF|CONN|HAND|ON>"
//   y=16  "IP <ip|-->"            （仅 TCP 且 ip 非空显示 ip，否则 "--"）
//   y=24  "RSSI <r> CH <c>"       （TCP + apInfoValid 才显示数值）
//   y=32  "FRM <frameCount>"      （6 位 clamp）
//   y=40  "ERR <errorCount>"      （6 位 clamp）
//   y=48  "HEAP <freeHeap>"       （8 位 clamp）
//   y=56  "UP <HH:MM:SS>"         （小时 clamp 99）
#pragma once

#include <cstdint>

#include "oled_fb.h"

namespace espview {
namespace oled {

// 状态快照（值语义；由上层注入，渲染层不理解字段含义）。
struct StatusSnapshot {
    uint8_t transportType = 0;      // 0=UART, 1=TCP
    bool transportConnected = false;
    uint8_t sessionState = 0;       // 0..3（OFF/CONN/HAND/ON）
    char ip[16] = {};
    bool apInfoValid = false;
    int8_t rssi = -128;
    uint8_t channel = 0;
    uint64_t frameCount = 0;
    uint64_t errorCount = 0;
    uint64_t uptimeMs = 0;
    uint32_t freeHeap = 0;
    uint32_t minFreeHeap = 0;
    uint32_t largestBlock = 0;
};

void renderStatus(OledFb& fb, const StatusSnapshot& s);

}  // namespace oled
}  // namespace espview