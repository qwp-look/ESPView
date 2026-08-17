// ESPView M6-A — Wi-Fi STA 管理器实现（见 wifi_sta.hpp）。

#include "espview/wifi_sta.hpp"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "nvs_flash.h"

namespace espview {

namespace {

const char* kTag = "espview_wifi";

// esp_event 默认任务优先级由 ESP-IDF 决定（esp_task.h ESP_TASKD_EVENT_PRIO =
// configMAX_PRIORITIES - 5；经典 ESP32 configMAX_PRIORITIES=25 → 优先级 20）。
// 本常量仅供文档参考，未用于创建任务（事件处理运行在 esp_event 系统任务内）。
constexpr UBaseType_t kWifiEventTaskPriority = 20;

// M8-A5（WIFI-01）：事件 handler 内瞬态断开的连续自动重连上限（有界防风暴）。
// 超过后置 kFailBit —— 重连收敛到 link task 的 ReconnectPolicy 退避调度。
constexpr uint32_t kMaxAutoReconnectAttempts = 3;
// M8-A5（WIFI-03）：STA_CONNECTED 后等待 GOT_IP 的 DHCP 上限（毫秒）。
// 关联成功但 DHCP 无 IP 时 waitForIp 不再无限等待（timeoutMs=0 也有限）。
constexpr uint64_t kDhcpTimeoutMs = 20000;

// M8-A5（WIFI-02）：断开 reason 终态分类 —— 凭据/AP 级失败重试不会自愈，
// 立即置 kFailBit（waitForIp 返回 false），不再事件内自动重连。
bool isTerminalWifiReason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        case WIFI_REASON_GROUP_CIPHER_INVALID:
        case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        case WIFI_REASON_AKMP_INVALID:
        case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        case WIFI_REASON_INVALID_RSN_IE_CAP:
        case WIFI_REASON_802_1X_AUTH_FAILED:
        case WIFI_REASON_CIPHER_SUITE_REJECTED:
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_CONNECTION_FAIL:
            return true;
        default:
            return false;
    }
}

}  // namespace

esp_err_t WifiSta::init(const WifiStaConfig& cfg) {
    if (initDone_) {
        return ESP_ERR_INVALID_STATE;  // 幂等保护：不隐式重开
    }
    // M8-A5（WIFI-05）：全新 init 允许自动重连（stop 是 teardown 窗口语义）。
    stopped_.store(false);
    lastFailTerminal_.store(false);
    reconnectAttempts_.store(0);
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
    // M8-A5（TCP-ESP-01）：不再以 initDone_ 早退 —— init() 任一步失败后的
    // 半初始化状态也必须在 deinit() 中释放（esp_wifi_stop/deinit 未初始化时
    // 返回错误码但无害；所有资源均带 null 检查，可幂等重入）。
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
    if (stopped_.load()) {
        // M8-A5（WIFI-05）：stopReconnect() 后不再发起连接（link task 将退出/退避）。
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(eventGroup_, kGotIpBit | kFailBit);
    reconnectAttempts_.store(0);  // M8-A5（WIFI-01）：外部显式 connect 重置风暴计数
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
    return err;
}

void WifiSta::stopReconnect() {
    // M8-A5（WIFI-05）：关闭 stop 窗口 —— 事件 handler 不再自动 connect；
    // 置 kFailBit 让阻塞中的 waitForIp 立即返回（close 不必等 connect_timeout）。
    stopped_.store(true);
    if (eventGroup_ != nullptr) {
        xEventGroupSetBits(eventGroup_, kFailBit);
        xEventGroupClearBits(eventGroup_, kGotIpBit);
    }
}

bool WifiSta::waitForIp(uint32_t timeoutMs) {
    if (!initDone_ || eventGroup_ == nullptr) {
        return false;
    }
    // M8-A5（WIFI-03）：分块等待（≤1000ms/次）。timeoutMs=0 也不得无限等 ——
    // STA_CONNECTED 后超过 kDhcpTimeoutMs 无 GOT_IP → 置 kFailBit 返回 false。
    const uint64_t startMs = static_cast<uint64_t>(esp_timer_get_time() / 1000);
    while (true) {
        const uint64_t nowMs = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        uint32_t waitTicks;
        if (timeoutMs == 0) {
            waitTicks = pdMS_TO_TICKS(1000);
        } else {
            const uint64_t elapsed = nowMs - startMs;
            if (elapsed >= timeoutMs) {
                return false;
            }
            const uint64_t remaining = timeoutMs - elapsed;
            waitTicks = pdMS_TO_TICKS(remaining < 1000 ? remaining : 1000);
        }
        const EventBits_t bits = xEventGroupWaitBits(eventGroup_, kGotIpBit | kFailBit,
                                                    pdFALSE, pdFALSE, waitTicks);
        if ((bits & kGotIpBit) != 0) {
            return true;
        }
        if ((bits & kFailBit) != 0) {
            return false;
        }
        const uint64_t connectedAt = staConnectedMs_.load();
        if (wifiConnected_.load() && connectedAt != 0 &&
            nowMs > connectedAt + kDhcpTimeoutMs) {
            lastFailTerminal_.store(false);
            xEventGroupSetBits(eventGroup_, kFailBit);
            return false;
        }
    }
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
                gotIp_ = false;
                staConnectedMs_.store(static_cast<uint64_t>(esp_timer_get_time() / 1000));
                reconnectAttempts_.store(0);  // M8-A5（WIFI-01）：关联成功重置风暴计数
                xEventGroupClearBits(eventGroup_, kFailBit);  // 新一次关联尝试：允许等 GOT_IP
                ESP_LOGI(kTag, "WIFI CONNECTED");
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifiConnected_ = false;
                gotIp_ = false;
                // 不打印 reason 数值外的敏感信息（§二十六）。
                const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
                const int reason = ev == nullptr ? -1 : ev->reason;
                ESP_LOGW(kTag, "WIFI DISCONNECTED (reason=%d)", reason);
                xEventGroupClearBits(eventGroup_, kGotIpBit);
                if (stopped_.load()) {
                    return;  // M8-A5（WIFI-05）：teardown 窗口，不再自动重连
                }
                if (reason > 0 && isTerminalWifiReason(static_cast<uint8_t>(reason))) {
                    // M8-A5（WIFI-02）：终态原因（凭据错误/AP 未找到等）→ 立即失败
                    // waitForIp；由 link task 以慢退避重试（等待配置/AP 恢复）。
                    lastFailTerminal_.store(true);
                    xEventGroupSetBits(eventGroup_, kFailBit);
                    return;
                }
                lastFailTerminal_.store(false);
                // M8-A5（WIFI-01）：瞬态断开 → 有界自动重连（≤kMaxAutoReconnectAttempts）；
                // 连续失败超限 → 置 kFailBit，重连收敛到 link task 退避调度（防风暴）。
                const uint32_t n = reconnectAttempts_.load();
                if (n >= kMaxAutoReconnectAttempts) {
                    ESP_LOGW(kTag, "auto-reconnect exhausted (%u); handing off to link task",
                             static_cast<unsigned>(n));
                    xEventGroupSetBits(eventGroup_, kFailBit);
                    return;
                }
                reconnectAttempts_.store(n + 1);
                const esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
                    ESP_LOGW(kTag, "auto-reconnect esp_wifi_connect failed: %s",
                             esp_err_to_name(err));
                }
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
        xEventGroupClearBits(eventGroup_, kFailBit);
    }
}

}  // namespace espview
