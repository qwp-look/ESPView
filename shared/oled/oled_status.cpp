// ESPView M7-B — 状态渲染实现（与 M7-A esp32/components/oled/src/status_ui.cpp
// 逐行等价：8 行、8x8 字体、128x64；先 fb.clear()）。
#include "oled_status.h"

#include <cstdio>

namespace espview {
namespace oled {

namespace {

const char* sessionName(uint8_t st) {
    switch (st) {
        case 0: return "OFF";
        case 1: return "CONN";
        case 2: return "HAND";
        case 3: return "ON";
        default: return "?";
    }
}

// 计数 clamp：显示位宽有界，避免长数字溢出 128px 行宽。
uint64_t clampDigits(uint64_t v, int digits) {
    uint64_t cap = 1;
    for (int i = 0; i < digits; ++i) {
        cap *= 10;
    }
    return v < cap ? v : cap - 1;
}

}  // namespace

void renderStatus(OledFb& fb, const StatusSnapshot& s) {
    fb.clear();
    // 缓冲 ≥66B：snprintf 全范围输出（%llu 最多 20 位）不触发
    // -Wformat-truncation；实际渲染文本与 status_ui.cpp 逐字节一致。
    char line[80];

    fb.drawText(0, 0, "ESPView");

    // 传输类型 + 会话状态。
    std::snprintf(line, sizeof(line), "%s %s",
                  s.transportType == 1 ? "TCP" : "UART", sessionName(s.sessionState));
    fb.drawText(0, 8, line);

    // IP（仅 TCP 有意义；其余显示 "--"）。
    const char* ip = s.transportType == 1 && s.ip[0] != '\0' ? s.ip : "--";
    std::snprintf(line, sizeof(line), "IP %s", ip);
    fb.drawText(0, 16, line);

    // RSSI / channel（TCP + apInfo 有效时）。
    if (s.transportType == 1 && s.apInfoValid) {
        std::snprintf(line, sizeof(line), "RSSI %d CH %u",
                      static_cast<int>(s.rssi), static_cast<unsigned>(s.channel));
    } else {
        std::snprintf(line, sizeof(line), "RSSI -- CH --");
    }
    fb.drawText(0, 24, line);

    std::snprintf(line, sizeof(line), "FRM %llu",
                  static_cast<unsigned long long>(clampDigits(s.frameCount, 6)));
    fb.drawText(0, 32, line);

    std::snprintf(line, sizeof(line), "ERR %llu",
                  static_cast<unsigned long long>(clampDigits(s.errorCount, 6)));
    fb.drawText(0, 40, line);

    std::snprintf(line, sizeof(line), "HEAP %lu",
                  static_cast<unsigned long>(clampDigits(s.freeHeap, 8)));
    fb.drawText(0, 48, line);

    // Uptime（小时 clamp 到 99，避免溢出行宽；分/秒天然 0..59）。
    const uint64_t totalSec = s.uptimeMs / 1000;
    const uint64_t h = totalSec / 3600 < 100 ? totalSec / 3600 : 99;
    const uint64_t m = (totalSec % 3600) / 60;
    const uint64_t sec = totalSec % 60;
    std::snprintf(line, sizeof(line), "UP %02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(h),
                  static_cast<unsigned long long>(m),
                  static_cast<unsigned long long>(sec));
    fb.drawText(0, 56, line);
}

// M7-E：OLED 生命周期状态 -> 诊断显示串。数值与 esp32 侧 OledState 枚举
// 对齐（0..4 既有值不变，5 = kSuspendedForWifiScan）；shared 侧零平台依赖，
// 不引入 esp32 头，故以 uint8_t 承载。供 statsLoop/诊断页渲染消费；声明
// 位置为集成点：主代理可把声明加入 oled_status.h，或调用侧自行 extern 声明。
const char* oledStateName(uint8_t state) {
    switch (state) {
        case 0: return "DISABLED";
        case 1: return "INIT";
        case 2: return "READY";
        case 3: return "DEGRADED";
        case 4: return "STOPPING";
        case 5: return "SUSPEND";  // kSuspendedForWifiScan
        default: return "?";
    }
}

}  // namespace oled
}  // namespace espview