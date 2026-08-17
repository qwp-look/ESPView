// ESPView M8-A7 — compile-time target traits（classic ESP32 / ESP32-S3）。
//
// 规范来源：docs/DESIGN.md AS 章（A7-5 target abstraction）+ M8-A7 任务书 §13/§14。
// 只解决当前真实差异（GPIO 上限、UART0 默认引脚、native USB、flash 尺寸），
// 不做过度抽象；target 由 CONFIG_IDF_TARGET 决定（sdkconfig.defaults /
// sdkconfig.defaults.esp32s3 注入，见 A7-7）。
//
// 使用约定：
//   - 本头文件只记录芯片规格 / target 事实，不依赖任何业务组件；
//   - espview（UART）与 oled（I2C）组件各自 include 并做初始化期校验；
//   - 不要把 target 知识散落在 main.cpp 的大量 #if 中。

#pragma once

#include "sdkconfig.h"

namespace espview {
namespace target {

enum class ChipFamily { kEsp32, kEsp32S3 };

#if CONFIG_IDF_TARGET_ESP32S3
constexpr ChipFamily kChipFamily = ChipFamily::kEsp32S3;
constexpr const char* kTargetName = "esp32s3";
#elif CONFIG_IDF_TARGET_ESP32
constexpr ChipFamily kChipFamily = ChipFamily::kEsp32;
constexpr const char* kTargetName = "esp32";
#else
// M8-A7：未支持 target fail-closed（经典 ESP32 / ESP32-S3 之外拒绝编译）。
#error "ESPView supports classic ESP32 and ESP32-S3 only (see target_info)"
#endif

// 有效 GPIO 编号上限（含）：
//   classic ESP32 : 0..39（34..39 仅输入，无内部上/下拉）
//   ESP32-S3     : 0..48（46 仅输入；26/27 与 28..32 有复用约束）
constexpr int kMaxGpio = (kChipFamily == ChipFamily::kEsp32S3) ? 48 : 39;

// UART0 默认引脚（-1 = 保持 ESP-IDF driver 默认）。
//   classic ESP32 : TX=1 / RX=3（板载 CH340 路径，M1-1 baseline）
//   ESP32-S3     : TX=43 / RX=44（ESP-IDF UART0 默认）
constexpr int kUart0DefaultTxGpio = (kChipFamily == ChipFamily::kEsp32S3) ? 43 : 1;
constexpr int kUart0DefaultRxGpio = (kChipFamily == ChipFamily::kEsp32S3) ? 44 : 3;

// 目标原生 USB 支持（classic ESP32 无原生 USB；ESP32-S3 有 USB-OTG/CDC）。
// M8-A7 只记录规格事实：UsbTransport 未实现（DESIGN X.16 / A7-7 边界）。
constexpr bool kHasNativeUsb = (kChipFamily == ChipFamily::kEsp32S3);

// 芯片规格 flash 尺寸上限（MB；实际生效值见 sdkconfig.defaults.* / menuconfig）。
//   classic ESP32 : 16 MiB 上限（本项目固定 4 MiB / 2 MiB factory，无 OTA）
//   ESP32-S3 N16R8 : 16 MiB（A7-7 提供 sdkconfig.defaults.esp32s3）
constexpr int kFlashSizeUpperBoundMb = 16;

// GPIO 有效性（OLED 等外设场景：不允许 -1）。
constexpr bool gpioValid(int gpio) {
    return gpio >= 0 && gpio <= kMaxGpio;
}

// UART 场景：-1 = 保持 driver 默认，允许。
constexpr bool uartGpioValid(int gpio) {
    return gpio == -1 || gpioValid(gpio);
}

}  // namespace target
}  // namespace espview
