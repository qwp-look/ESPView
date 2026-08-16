// ESPView M7-A — OledI2c 实现（ESP-IDF v6.0.2 esp_driver_i2c 新 API）。
//
// 依据实际头文件 C:\esp\v6.0.2\esp-idf\components\esp_driver_i2c\include\driver\i2c_master.h：
//   - i2c_master_bus_config_t：i2c_port(-1 自动)、sda/scl、clk_source、
//     glitch_ignore_cnt、intr_priority、trans_queue_depth、flags.enable_internal_pullup
//     （bus 级没有 clk_speed_hz，速率在设备级 scl_speed_hz）；
//   - i2c_device_config_t：dev_addr_length(I2C_ADDR_BIT_LEN_7)、device_address
//     （7-bit raw）、scl_speed_hz、scl_wait_us、flags.disable_ack_check；
//   - i2c_master_probe(bus, address, timeout_ms) 扫描；i2c_master_transmit(dev,...)；
//   - 本版本无 i2c_master_bus_remove_device：设备随 i2c_del_master_bus 一并释放。

#include "oled_i2c.h"

#include <algorithm>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace espview {
namespace oled {

namespace {
constexpr char kTag[] = "espview_oled_i2c";
constexpr uint16_t kProbeMin = 0x08;
constexpr uint16_t kProbeMax = 0x77;
constexpr uint8_t kPreferredA = 0x3C;
constexpr uint8_t kPreferredB = 0x3D;

// 探测超时单独收紧（< i2cTimeoutMs）：全扫描最坏 ~1.1s（0x70 个地址 × 10ms），
// 正常无设备时 NACK 立即返回。防止启动阶段被坏总线拖死。
uint32_t probeTimeoutMs(const OledConfig& cfg) {
    return std::min<uint32_t>(cfg.i2cTimeoutMs, 10u);
}
}  // namespace

OledI2c::OledI2c(const OledConfig& cfg) : cfg_(cfg) {}

OledI2c::~OledI2c() {
    deinit();
}

esp_err_t OledI2c::init() {
    if (bus_ != nullptr) {
        return ESP_OK;  // 幂等
    }
    i2c_master_bus_config_t busCfg = {};
    busCfg.i2c_port = static_cast<i2c_port_num_t>(-1);  // 自动选择端口
    busCfg.sda_io_num = static_cast<gpio_num_t>(cfg_.sdaGpio);
    busCfg.scl_io_num = static_cast<gpio_num_t>(cfg_.sclGpio);
    busCfg.clk_source = I2C_CLK_SRC_DEFAULT;
    busCfg.glitch_ignore_cnt = 7;
    busCfg.intr_priority = 0;  // 驱动默认优先级
    // 必须保持同步模式（trans_queue_depth=0）：异步模式与 i2c_master_probe 不兼容
    // （驱动 i2c_master.c 明确 warning），且本任务只用阻塞式 transmit + 超时。
    busCfg.trans_queue_depth = 0;
    busCfg.flags.enable_internal_pullup = 1;  // 板级无外部上拉时兜底

    const esp_err_t err = i2c_new_master_bus(&busCfg, &bus_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        bus_ = nullptr;
        return err;
    }
    ESP_LOGI(kTag, "bus created: sda=%d scl=%d clk=%lu", cfg_.sdaGpio, cfg_.sclGpio,
             static_cast<unsigned long>(cfg_.clkHz));
    return ESP_OK;
}

void OledI2c::setAbortPredicate(std::function<bool()> pred) {
    abort_ = std::move(pred);
}

uint8_t OledI2c::probe() {
    if (bus_ == nullptr) {
        return 0;
    }
    uint8_t preferred = 0;
    uint8_t first = 0;
    for (uint16_t a = kProbeMin; a <= kProbeMax; ++a) {
        // M7-F：挂起/停止置位时立即中止探测（挂起窗口内 I2C 流量≈0）。
        if (abort_ && abort_()) {
            return 0;
        }
        const esp_err_t r = i2c_master_probe(bus_, a,
                                             static_cast<int>(probeTimeoutMs(cfg_)));
        if (r != ESP_OK) {
            continue;  // NACK/超时：无设备
        }
        if (first == 0) {
            first = static_cast<uint8_t>(a);
        }
        if ((a == kPreferredA || a == kPreferredB) && preferred == 0) {
            preferred = static_cast<uint8_t>(a);  // 扫描升序：3C 优先于 3D
        }
    }
    const uint8_t chosen = preferred != 0 ? preferred : first;
    if (chosen != 0) {
        ESP_LOGI(kTag, "probe: found device at 0x%02X", static_cast<unsigned>(chosen));
    } else {
        ESP_LOGW(kTag, "probe: no I2C device responded in 0x08..0x77");
    }
    return chosen;
}

esp_err_t OledI2c::addDevice(uint8_t addr) {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dev_ != nullptr) {
        return ESP_OK;  // 幂等
    }
    i2c_device_config_t devCfg = {};
    devCfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    devCfg.device_address = addr;                 // 7-bit raw（无 R/W 位）
    devCfg.scl_speed_hz = cfg_.clkHz;
    devCfg.scl_wait_us = 0;                       // 驱动默认
    devCfg.flags.disable_ack_check = 0;           // 启用 ACK 检查

    const esp_err_t err = i2c_master_bus_add_device(bus_, &devCfg, &dev_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_master_bus_add_device(0x%02X) failed: %s",
                 static_cast<unsigned>(addr), esp_err_to_name(err));
        dev_ = nullptr;
        return err;
    }
    address_ = addr;
    ESP_LOGI(kTag, "device attached: addr=0x%02X speed=%lu",
             static_cast<unsigned>(addr), static_cast<unsigned long>(cfg_.clkHz));
    return ESP_OK;
}

esp_err_t OledI2c::transmit(const uint8_t* data, size_t len) {
    if (dev_ == nullptr || data == nullptr || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit(dev_, data, len,
                               static_cast<int>(cfg_.i2cTimeoutMs));
}

esp_err_t OledI2c::resetBus() {
    if (bus_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = i2c_master_bus_reset(bus_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c_master_bus_reset failed: %s", esp_err_to_name(err));
    }
    return err;
}

void OledI2c::deinit() {
    if (bus_ != nullptr) {
        i2c_del_master_bus(bus_);  // 设备随 bus 删除（本版本无 remove_device API）
        bus_ = nullptr;
    }
    dev_ = nullptr;
    address_ = 0;
}

}  // namespace oled
}  // namespace espview
