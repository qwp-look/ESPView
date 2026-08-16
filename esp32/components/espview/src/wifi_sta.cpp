// ESPView M6-A — Wi-Fi STA 管理器实现（见 wifi_sta.hpp）。

#include "espview/wifi_sta.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace espview {

namespace {

const char* kTag = "espview_wifi";

constexpr UBaseType_t kWifiEventTaskPriority = 8;  // esp_event 默认任务优先级

}  // namespace

esp_err_t WifiSta::init(const WifiStaConfig& cfg) {
    if (initDone_) {
        return ESP_ERR_INVALID_STATE;  // 幂等保护：不隐式重开
    }
    if (cfg.ssid == nullptr || cfg.ssid[0] == '\0') {
        ESP_LOGE(kTag, "init: empty SSID (credentials from local sdkconfig/menuconfig)");
        return ESP_ERR_INVALID_ARG;
    }
    psNone_ = cfg.ps_none;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    netif_ = esp_netif_create_default_wifi_sta();
    if (netif_ == nullptr) {
        ESP_LOGE(kTag, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    // Wi-Fi 驱动依赖 NVS（PHY 校准数据）。凭据本身仍走 WIFI_STORAGE_RAM（§二十六）。
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "nvs_flash_init: erase and retry");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t wifiCfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifiCfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    eventGroup_ = xEventGroupCreate();
    if (eventGroup_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiSta::eventHandler,
                                              this, &wifiAny_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "wifi event handler register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiSta::eventHandler,
                                              this, &ipGotIp_);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ip event handler register failed: %s", esp_err_to_name(err));
        return err;
    }

    // RAM 存储：凭据只存在于本进程内存，不写 flash（不依赖 NVS）。
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_storage(RAM) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t sta = {};
    // SSID/密码长度由 Kconfig 输入保证；防御性截断到协议上限。
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), cfg.ssid, sizeof(sta.sta.ssid) - 1);
    if (cfg.password != nullptr) {
        std::strncpy(reinterpret_cast<char*>(sta.sta.password), cfg.password,
                     sizeof(sta.sta.password) - 1);
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 实验性延迟优化（默认关闭；生产默认保持 ESP-IDF power save）。
    if (psNone_) {
        const esp_err_t psErr = esp_wifi_set_ps(WIFI_PS_NONE);
        if (psErr != ESP_OK) {
            ESP_LOGW(kTag, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(psErr));
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    initDone_ = true;
    // 日志只打印 SSID 长度，不打印凭据本身（§二十六/§三十七）。
    ESP_LOGI(kTag, "STA ready (ssid_len=%d, ps=%s)", static_cast<int>(std::strlen(cfg.ssid)),
             psNone_ ? "NONE" : "default");
    return ESP_OK;
}

void WifiSta::deinit() {
    if (!initDone_) {
        return;
    }
    if (wifiAny_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiAny_);
        wifiAny_ = nullptr;
    }
    if (ipGotIp_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ipGotIp_);
        ipGotIp_ = nullptr;
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    if (netif_ != nullptr) {
        esp_netif_destroy_default_wifi(netif_);
        netif_ = nullptr;
    }
    if (eventGroup_ != nullptr) {
        vEventGroupDelete(eventGroup_);
        eventGroup_ = nullptr;
    }
    wifiConnected_ = false;
    gotIp_ = false;
    initDone_ = false;
    ESP_LOGI(kTag, "STA deinit");
}

bool WifiSta::apInfo(int8_t* rssi, uint8_t* channel) const {
    if (rssi == nullptr || channel == nullptr) {
        return false;
    }
    // 以驱动实际关联状态为准（未关联时 esp_wifi_sta_get_ap_info 返回失败）：
    // 不再依赖本地 wifiConnected_ 标志——M7-G adopt 模式（驱动由 provisioning
    // 持有、本类未 init）下诊断仍能取到 rssi/channel。
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *rssi = ap.rssi;
    *channel = ap.primary;
    return true;
}

esp_err_t WifiSta::startConnect() {
    if (!initDone_) {
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(eventGroup_, kGotIpBit | kFailBit);
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
    return err;
}

bool WifiSta::waitForIp(uint32_t timeoutMs) {
    if (!initDone_ || eventGroup_ == nullptr) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        eventGroup_, kGotIpBit | kFailBit, pdFALSE, pdFALSE,
        timeoutMs == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs));
    return (bits & kGotIpBit) != 0;
}

void WifiSta::eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<WifiSta*>(arg);
    self->handleEvent(base, id, data);
}

void WifiSta::handleEvent(esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(kTag, "WIFI CONNECTING (STA_START)");
                break;
            case WIFI_EVENT_STA_CONNECTED:
                wifiConnected_ = true;
                ESP_LOGI(kTag, "WIFI CONNECTED");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifiConnected_ = false;
                gotIp_ = false;
                // 不打印 reason 数值外的敏感信息（§二十六）。
                const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
                ESP_LOGW(kTag, "WIFI DISCONNECTED (reason=%d)", ev == nullptr ? -1 : ev->reason);
                // 断开即重连（有界重试：失败后 waitForIp 侧超时，由 link task 退避）。
                const esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
                    ESP_LOGW(kTag, "auto-reconnect esp_wifi_connect failed: %s",
                             esp_err_to_name(err));
                }
                xEventGroupClearBits(eventGroup_, kGotIpBit);
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<ip_event_got_ip_t*>(data);
        if (ev != nullptr) {
            // IP 是公开网络信息（非凭据），允许日志。
            esp_ip4addr_ntoa(&ev->ip_info.ip, ipBuf_, sizeof(ipBuf_));
            ESP_LOGI(kTag, "GOT_IP %s", ipBuf_);
        }
        wifiConnected_ = true;
        gotIp_ = true;
        xEventGroupSetBits(eventGroup_, kGotIpBit);
    }
}

}  // namespace espview
