// ESPView M7-B — OLED 组件配置（纯头文件，零平台依赖）。
//
// 默认值对齐 Kconfig CONFIG_ESPVIEW_OLED_*；refresh 500ms（host/PC 侧刷新
// 周期；ESP32 侧刷新周期由 esp32/components/oled/Kconfig 单独控制）。
// 校验规则与 M7-A esp32/components/oled/src/oled_display.cpp start() 兼容，
// 并补充上限（对应 Kconfig range 端点）。
#pragma once

#include <cstdint>

#include "oled_cmd.h"  // espview::oled::ControllerType（共享库自带）

namespace espview {
namespace oled {

// M7-B：OLED 组件配置（默认值对齐 Kconfig CONFIG_ESPVIEW_OLED_*；refresh 500ms）。
struct OledConfig {
    int sdaGpio = 21;
    int sclGpio = 22;
    uint32_t clkHz = 400000;
    bool addrAuto = true;
    uint8_t address = 0x3C;
    ControllerType controller = ControllerType::kAuto;
    uint32_t refreshMs = 500;
    uint32_t taskStack = 4096;
    uint32_t taskPriority = 2;
    uint32_t i2cTimeoutMs = 50;
    uint32_t maxReinit = 3;
};

// 配置校验（必须与 M7-A oled_display.cpp start() 规则兼容并补上限）：
//   sdaGpio>=0 && sclGpio>=0 && 100000<=clkHz<=1000000 &&
//   refreshMs 100..60000 && taskStack>=2048 && 1<=taskPriority<=maxTaskPriority &&
//   5<=i2cTimeoutMs<=1000 && 1<=maxReinit<=100
inline bool validateOledConfig(const OledConfig& cfg, uint32_t maxTaskPriority = 2) {
    if (cfg.sdaGpio < 0 || cfg.sclGpio < 0) {
        return false;
    }
    if (cfg.clkHz < 100000 || cfg.clkHz > 1000000) {
        return false;
    }
    if (cfg.refreshMs < 100 || cfg.refreshMs > 60000) {
        return false;
    }
    if (cfg.taskStack < 2048) {
        return false;
    }
    if (cfg.taskPriority < 1 || cfg.taskPriority > maxTaskPriority) {
        return false;
    }
    if (cfg.i2cTimeoutMs < 5 || cfg.i2cTimeoutMs > 1000) {
        return false;
    }
    if (cfg.maxReinit < 1 || cfg.maxReinit > 100) {
        return false;
    }
    return true;
}

}  // namespace oled
}  // namespace espview