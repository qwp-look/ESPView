// ESPView M7-A — 内部 I2C 总线封装（esp_driver_i2c 新 API）。
// 不进公共头（仅 oled_display.cpp 使用）。
#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "oled/oled_display.hpp"

namespace espview {
namespace oled {

class OledI2c {
public:
    explicit OledI2c(const OledConfig& cfg);
    ~OledI2c();

    OledI2c(const OledI2c&) = delete;
    OledI2c& operator=(const OledI2c&) = delete;

    // 创建 master bus（i2c_port=-1 自动；enable_internal_pullup=1）。幂等。
    esp_err_t init();
    // 扫描 0x08..0x77；优先 0x3C/0x3D，否则首个响应；无响应返回 0。
    uint8_t probe();
    // 挂载设备（7-bit raw 地址 + scl_speed_hz）。需先 init()。
    esp_err_t addDevice(uint8_t addr);
    // 单次 transmit（带超时）。未挂设备返回 ESP_ERR_INVALID_STATE。
    esp_err_t transmit(const uint8_t* data, size_t len);
    // 硬件级 bus reset（恢复路径第一步）。
    esp_err_t resetBus();
    // 释放 bus（设备随 bus 一并删除）。幂等。
    void deinit();

    bool valid() const { return dev_ != nullptr; }
    uint8_t address() const { return address_; }

private:
    OledConfig cfg_;
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    uint8_t address_ = 0;
};

}  // namespace oled
}  // namespace espview
