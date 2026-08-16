// ESPView M7-D3 — Wi-Fi Provisioning 实现（见 wifi_provisioning.hpp）。

#include "espview/wifi_provisioning.hpp"

#include <cstdlib>
#include <cstring>
#include <utility>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

namespace espview {

namespace {

const char* kTag = "espview_wifi_prov";

constexpr uint8_t kMaxScanRecords = 64;
constexpr uint32_t kDhcpTimeoutMs = 20000;  // 连接后限时无 GOT_IP → kDhcpTimeout
constexpr uint64_t kScanTimeoutMs = 10000;  // M7-E：扫描事务超时（被动扫描 ~2s，10s 余量）
constexpr uint8_t kDefaultMaxEntries = 32;

// ESP-IDF STA_DISCONNECTED reason → 协议错误码（认证类/AP 未找到为终态错误；
// 其余视为瞬态，自动重连）。
uint16_t errorCodeForReason(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:              // 201
            return 7;  // kApNotFound
        case WIFI_REASON_AUTH_FAIL:                // 202
        case WIFI_REASON_HANDSHAKE_TIMEOUT:        // 204
        case WIFI_REASON_CONNECTION_FAIL:          // 205
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:   // 15
            return 6;  // kAuthFailed
        default:
            return 0;  // 瞬态：自动重连，不上报错误
    }
}

}  // namespace

WifiProvisioning::WifiProvisioning(Callbacks cb)
    : cb_(std::move(cb)),
      scanTransaction_(wifi::ScanTransactionCallbacks{
          [this]() -> bool { return suspendDisplay(); },
          [this]() { resumeDisplay(); },
      }) {
    mutex_ = xSemaphoreCreateMutex();
}

WifiProvisioning::~WifiProvisioning() {
    zeroPasswordBuffers();
    if (wifiAny_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiAny_);
        wifiAny_ = nullptr;
    }
    if (ipGotIp_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ipGotIp_);
        ipGotIp_ = nullptr;
    }
    if (mutex_ != nullptr) {
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

void WifiProvisioning::zeroPasswordBuffers() {
    // 清空命令槽 password 副本（RAM-only 生命周期；绝不延迟到析构）。
    std::memset(command_.password, 0, sizeof(command_.password));
}

bool WifiProvisioning::suspendDisplay() {
#if CONFIG_ESPVIEW_SCAN_SUSPEND_OLED
    if (cb_.scanSuspendDisplay) {
        return cb_.scanSuspendDisplay();
    }
#endif  // CONFIG_ESPVIEW_SCAN_SUSPEND_OLED
    // CONFIG=n：等同 M7 前行为（不挂起 OLED）；未注入回调：no-op 成功。
    return true;
}

void WifiProvisioning::resumeDisplay() {
#if CONFIG_ESPVIEW_SCAN_SUSPEND_OLED
    if (cb_.scanResumeDisplay) {
        cb_.scanResumeDisplay();
    }
#endif  // CONFIG_ESPVIEW_SCAN_SUSPEND_OLED
}

void WifiProvisioning::requestScan(uint8_t maxEntries) {
    if (mutex_ == nullptr) {
        return;
    }
    Command cmd;
    cmd.kind = CommandKind::kScan;
    cmd.maxEntries = (maxEntries == 0 || maxEntries > kMaxScanRecords) ? kDefaultMaxEntries
                                                                      : maxEntries;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    command_ = cmd;
    commandPending_ = true;
    xSemaphoreGive(mutex_);
}

void WifiProvisioning::requestConfig(const char* ssid, size_t ssidLen, const char* password,
                                     size_t passLen, uint32_t serverIp, uint16_t serverPort) {
    if (mutex_ == nullptr || ssid == nullptr) {
        return;
    }
    // 输入校验（与 makeWifiConfig 同规则；拒绝时不落任何凭据副本）。
    if (ssidLen == 0 || ssidLen > 32) {
        return;
    }
    if (passLen != 0 && (passLen < 8 || passLen > 63)) {
        return;
    }
    if (serverIp == 0 || serverPort == 0) {
        return;
    }
    Command cmd;
    cmd.kind = CommandKind::kConfig;
    cmd.serverIp = serverIp;
    cmd.serverPort = serverPort;
    std::memcpy(cmd.ssid, ssid, ssidLen);
    if (password != nullptr && passLen > 0) {
        std::memcpy(cmd.password, password, passLen);
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    // 覆盖旧命令时先清零旧 password（防残留）。
    zeroPasswordBuffers();
    command_ = cmd;
    commandPending_ = true;
    xSemaphoreGive(mutex_);
}

void WifiProvisioning::requestClear() {
    if (mutex_ == nullptr) {
        return;
    }
    Command cmd;
    cmd.kind = CommandKind::kClear;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    command_ = cmd;
    commandPending_ = true;
    xSemaphoreGive(mutex_);
}

esp_err_t WifiProvisioning::ensureWifiReady() {
    if (initialized_) {
        return ESP_OK;
    }

    // D6 硬件修正（电源）：USB 供电下 Wi-Fi RF 上电/校准的电流尖峰会导致
    // CH340 USB 掉线 + ESP32 挂死（欠压）。启动 Wi-Fi 前把 CPU 固定到 80MHz，
    // 降低整机电流，尽量让 PHY 上电峰值落在电源余量内（CONFIG_PM_ENABLE=y；
    // 其他 profile 未开 PM 时 esp_pm_configure 返回 NOT_SUPPORTED，忽略即可）。
    const esp_pm_config_t pm = {.max_freq_mhz = 80, .min_freq_mhz = 80,
                                .light_sleep_enable = false};
    if (esp_pm_configure(&pm) != ESP_OK) {
        ESP_LOGW(kTag, "esp_pm_configure(80MHz) failed (PM disabled?)");
    }
    // D6：TX 功率压到最低（2dBm）——RF 校准/扫描期间减少发射电流贡献，
    // 配合 PASSIVE 扫描与 80MHz，尽量压低电源受限板卡的峰值电流。
    if (esp_wifi_set_max_tx_power(8) != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_set_max_tx_power(8) failed");
    }

    // 以下初始化幂等容错：已由其他组件（TcpTransport/WifiSta）完成时忽略。
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "nvs_flash_init: erase and retry");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t wicfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wicfg);
    if (err == ESP_OK) {
        wifiOwned_ = true;
        if (esp_netif_create_default_wifi_sta() == nullptr) {
            ESP_LOGE(kTag, "esp_netif_create_default_wifi_sta failed");
            wifiOwned_ = false;
            return ESP_FAIL;
        }
    } else if (err != ESP_ERR_INVALID_STATE) {
        // 真失败（invalid-state = 外部已初始化，复用驱动）。
        ESP_LOGE(kTag, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // RAM 存储（AF.4：凭据断电即失，不落 NVS）。
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

    if (!handlersRegistered_) {
        err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &WifiProvisioning::eventHandler, this,
                                                  &wifiAny_);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "wifi event register failed: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &WifiProvisioning::eventHandler, this,
                                                  &ipGotIp_);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "ip event register failed: %s", esp_err_to_name(err));
            return err;
        }
        handlersRegistered_ = true;
    }

    initialized_ = true;
    ESP_LOGI(kTag, "Wi-Fi ready (owns=%d)", wifiOwned_ ? 1 : 0);
    return ESP_OK;
}

void WifiProvisioning::eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<WifiProvisioning*>(arg);
    self->handleEvent(base, id, data);
}

void WifiProvisioning::handleEvent(esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_SCAN_DONE: {
                const auto* ev = static_cast<wifi_event_sta_scan_done_t*>(data);
                if (mutex_ == nullptr) {
                    break;
                }
                xSemaphoreTake(mutex_, portMAX_DELAY);
                // M7-F：代际保护。仅在扫描相位记录 SCAN_DONE 并绑定当前代际
                // （scanGen_）；status!=0 表示扫描被 esp_wifi_scan_stop/esp_wifi_stop
                // 终止或失败，其结果无意义。迟到事件由消费端代际+相位双重检查丢弃。
                if (phase_ == 1 /* kScanning */) {
                    scanDonePending_ = true;
                    scanDoneGen_ = scanGen_;
                    scanDoneFailed_ = (ev != nullptr && ev->status != 0);
                }
                xSemaphoreGive(mutex_);
                break;
            }
            case WIFI_EVENT_STA_CONNECTED: {
                wifi_ap_record_t ap = {};
                int8_t rssi = -128;
                uint8_t channel = 0;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                    rssi = ap.rssi;
                    channel = ap.primary;
                }
                if (mutex_ == nullptr) {
                    break;
                }
                xSemaphoreTake(mutex_, portMAX_DELAY);
                phase_ = 4;  // kWifiConnected
                errorCode_ = 0;
                rssi_ = rssi;
                channel_ = channel;
                statusDirty_ = true;
                xSemaphoreGive(mutex_);
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                const auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
                const uint16_t code = errorCodeForReason(ev == nullptr ? 0 : ev->reason);
                bool reconnect = false;
                if (mutex_ == nullptr) {
                    break;
                }
                xSemaphoreTake(mutex_, portMAX_DELAY);
                if (code != 0) {
                    // 终态错误：认证失败/AP 未找到 → kError（不自动重连，等待
                    // 用户重试重新 Apply；AF.4 凭据仍在 RAM，CLEAR/新 CONFIG 覆盖）。
                    phase_ = 8;  // kError
                    errorCode_ = code;
                } else if (configured_) {
                    // 瞬态断开：自动重连，相位回到 CONNECTING，重启 DHCP 计时。
                    phase_ = 3;  // kWifiConnecting
                    errorCode_ = 0;
                    connectStartMs_ = static_cast<uint64_t>(esp_timer_get_time() / 1000);
                    reconnect = true;
                }
                statusDirty_ = true;
                xSemaphoreGive(mutex_);
                if (reconnect) {
                    const esp_err_t err = esp_wifi_connect();
                    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
                        ESP_LOGW(kTag, "auto-reconnect failed: %s", esp_err_to_name(err));
                    }
                }
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* ev = static_cast<ip_event_got_ip_t*>(data);
        if (mutex_ == nullptr) {
            return;
        }
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (ev != nullptr) {
            ip_ = ev->ip_info.ip.addr;  // 网络序
        }
        phase_ = 5;  // kGotIp
        errorCode_ = 0;
        statusDirty_ = true;
        xSemaphoreGive(mutex_);
    }
}

void WifiProvisioning::tick(uint64_t nowMs) {
    if (mutex_ == nullptr) {
        return;
    }

    // M7-E：消费跨任务断线通知（RX/传输任务只置标志）→ 进行中的扫描事务进入
    // Disconnected 终态并恢复 OLED；无活动事务时 no-op（可随后重新 begin）。
    if (sessionDisconnectPending_.exchange(false, std::memory_order_acq_rel)) {
        const wifi::ScanPhase p = scanTransaction_.phase();
        if (p != wifi::ScanPhase::kIdle && p != wifi::ScanPhase::kError &&
            p != wifi::ScanPhase::kDisconnected) {
            scanTransaction_.onDisconnect();
        }
    }

    // 1) SCAN_DONE → 取结果（事件任务只置标志，取数在会话任务）。
    bool scanHandled = false;
    bool scanFailed = false;
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        // M7-F：代际匹配 + 相位匹配。仅当 SCAN_DONE 属于当前扫描（代际一致）
        // 且仍处于扫描相位时消费；被配置/清除终止的扫描的迟到事件在此丢弃，
        // 不会污染下一次扫描或推进中的配置相位。
        scanHandled = scanDonePending_ && scanDoneGen_ == scanGen_ && phase_ == 1;
        scanFailed = scanDoneFailed_;
        scanDonePending_ = false;
        xSemaphoreGive(mutex_);
    }
    if (scanHandled) {
        bool scanOk = false;  // M7-E：结果收集成功标志（onScanDone(ok)）
        if (scanFailed) {
            // M7-F：扫描被终止/驱动失败 → 事务终态 Error（恢复 OLED），不取结果。
            setError(5);  // kScanFailed
        } else {
            uint16_t apNum = 0;
            if (esp_wifi_scan_get_ap_num(&apNum) != ESP_OK) {
                setError(5);  // kScanFailed
            } else {
            const bool truncated = apNum > kMaxScanRecords;
            const uint16_t fetch = truncated ? kMaxScanRecords : apNum;
            auto* aps = static_cast<wifi_ap_record_t*>(std::malloc(fetch * sizeof(wifi_ap_record_t)));
            if (aps == nullptr) {
                setError(12);  // kApiError
            } else {
                uint16_t got = fetch;
                if (esp_wifi_scan_get_ap_records(&got, aps) != ESP_OK) {
                    setError(5);  // kScanFailed
                } else {
                    // 按 RSSI 降序（简单插入排序，n ≤ 64）。
                    for (uint16_t i = 1; i < got; ++i) {
                        const wifi_ap_record_t key = aps[i];
                        int32_t j = static_cast<int32_t>(i) - 1;
                        while (j >= 0 && aps[j].rssi < key.rssi) {
                            aps[j + 1] = aps[j];
                            --j;
                        }
                        aps[j + 1] = key;
                    }
                    uint8_t maxEntries = kDefaultMaxEntries;
                    {
                        xSemaphoreTake(mutex_, portMAX_DELAY);
                        maxEntries = scanMaxEntries_;
                        scanSeq_ = static_cast<uint8_t>(scanSeq_ + 1u);
                        xSemaphoreGive(mutex_);
                    }
                    const size_t n = got < maxEntries ? got : maxEntries;
                    for (size_t i = 0; i < n; ++i) {
                        WifiProvScanRecord& dst = scanRecords_[i];
                        std::memset(&dst, 0, sizeof(dst));
                        std::strncpy(dst.ssid, reinterpret_cast<const char*>(aps[i].ssid),
                                     sizeof(dst.ssid) - 1);
                        std::memcpy(dst.bssid, aps[i].bssid, 6);
                        dst.rssi = aps[i].rssi;
                        dst.channel = aps[i].primary;
                        dst.authmode = static_cast<uint8_t>(aps[i].authmode);
                    }
                    {
                        xSemaphoreTake(mutex_, portMAX_DELAY);
                        scanCount_ = n;
                        scanTruncated_ = truncated || (got > n);
                        scanTotal_ = apNum;
                        scanReady_ = true;
                        // 扫描结束：相位恢复到扫描前。M7-F：仅当仍处于扫描相位时
                        // 恢复——配置/连接可能已推进相位（异步 Wi-Fi 事件），
                        // 无条件回卷会破坏进行中的配置状态机。
                        if (phase_ == 1 /* kScanning */) {
                            phase_ = prevPhase_;
                        }
                        errorCode_ = 0;
                        statusDirty_ = true;
                        xSemaphoreGive(mutex_);
                    }
                    scanOk = true;  // M7-E：结果收集成功
                }
                std::free(aps);
            }
        }
        }
        // M7-E：结果收集完成（成功/失败）→ 事务终态（成功/失败都恢复 OLED）。
        scanTransaction_.onScanDone(scanOk);
    }

    // 2) 派发扫描结果（锁外回调；缓冲只在下次扫描前有效）。
    {
        bool ready = false;
        uint8_t seq = 0;
        bool truncated = false;
        uint16_t total = 0;
        size_t count = 0;
        {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            ready = scanReady_;
            seq = scanSeq_;
            truncated = scanTruncated_;
            total = scanTotal_;
            count = scanCount_;
            if (ready) {
                scanReady_ = false;
            }
            xSemaphoreGive(mutex_);
        }
        if (ready && cb_.onScanResult) {
            cb_.onScanResult(seq, truncated, total, scanRecords_, count);
        }
    }

    // 3) 处理命令槽（单槽串行；v0.1 控制面）。
    Command cmd;
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        if (commandPending_) {
            cmd = command_;
            commandPending_ = false;
            std::memset(command_.password, 0, sizeof(command_.password));
            command_.kind = CommandKind::kNone;
        }
        xSemaphoreGive(mutex_);
    }
    switch (cmd.kind) {
        case CommandKind::kScan: {
            // M7-E：进入扫描事务。仅 Idle/Error/Disconnected 可开启；活动扫描
            // 期间的重复请求忽略（单槽命令语义，避免对进行中事务重复 begin）。
            const wifi::ScanPhase p = scanTransaction_.phase();
            if (p != wifi::ScanPhase::kIdle && p != wifi::ScanPhase::kError &&
                p != wifi::ScanPhase::kDisconnected) {
                break;
            }
            scanTransaction_.begin();
            startScan(cmd.maxEntries);
            break;
        }
        case CommandKind::kConfig:
        case CommandKind::kClear: {
            // M7-F：配置/清除不得打断进行中的扫描事务（esp_wifi_stop/applyClear
            // 会终止扫描、事务停挂 OLED 等看门狗，且终止的扫描结果可能按新 seq
            // 上报）。先显式结束事务、停止扫描并作废滞留 SCAN_DONE（代际推进）。
            const wifi::ScanPhase sp = scanTransaction_.phase();
            if (sp != wifi::ScanPhase::kIdle && sp != wifi::ScanPhase::kError &&
                sp != wifi::ScanPhase::kDisconnected) {
                xSemaphoreTake(mutex_, portMAX_DELAY);
                scanDonePending_ = false;
                scanGen_ = static_cast<uint8_t>(scanGen_ + 1u);  // 作废在途 SCAN_DONE
                xSemaphoreGive(mutex_);
                esp_wifi_scan_stop();  // 终止进行中的扫描（其后 SCAN_DONE 为失败态，被消费端丢弃）
                scanTransaction_.onScanDone(false);  // 立即恢复 OLED；事务回 kError（可重新 begin）
            }
            if (cmd.kind == CommandKind::kConfig) {
                applyConfig(cmd);
            } else {
                applyClear();
            }
            break;
        }
        default:
            break;
    }

    // 4) DHCP 超时检测（连接后限时无 IP → kDhcpTimeout 终态）。
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        // kWifiConnecting（未关联）或 kWifiConnected（已关联但无 IP）都受限时约束。
        const bool expired = (phase_ == 3 || phase_ == 4) && ip_ == 0 &&
                             connectStartMs_ != 0 && nowMs >= connectStartMs_ &&
                             (nowMs - connectStartMs_) >= kDhcpTimeoutMs;
        if (expired) {
            phase_ = 8;  // kError
            errorCode_ = 8;  // kDhcpTimeout
            statusDirty_ = true;
        }
        xSemaphoreGive(mutex_);
    }

    // 5) 状态去重派发。
    publishStatus();

    // 6) M7-E：扫描事务超时驱动（扫描卡死/无 SCAN_DONE 时恢复 OLED）。
    scanTransaction_.tick(nowMs, kScanTimeoutMs);
}

void WifiProvisioning::notifySessionDisconnected() {
    // M7-E：会话断开（onSessionState 回调，可能运行于 RX/传输任务）→ 只置
    // 原子挂起标志；事务终态转换由会话任务 tick() 消费执行（同一任务内对
    // scanTransaction_ 的全部访问，避免跨任务数据竞争）。
    sessionDisconnectPending_.store(true, std::memory_order_release);
}

void WifiProvisioning::startScan(uint8_t maxEntries) {
    const esp_err_t r = ensureWifiReady();
    if (r != ESP_OK) {
        setError(12);  // kApiError
        // M7-E：扫描未启动（init 失败）→ 事务终态 Error（尚未挂起，恢复 no-op）。
        scanTransaction_.onScanStarted(false);
        return;
    }
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        prevPhase_ = (phase_ == 8 /* kError */ || phase_ == 1 /* kScanning */) ? 0 : phase_;
        phase_ = 1;  // kScanning
        errorCode_ = 0;
        scanMaxEntries_ = maxEntries;
        // M7-F：新扫描代际（uint8 回绕安全：0→1；匹配仅需与上一代不同）。
        // 同时清掉上一扫描遗留的 SCAN_DONE 标志（与代际检查互为防御）。
        scanGen_ = static_cast<uint8_t>(scanGen_ + 1u);
        scanDonePending_ = false;
        statusDirty_ = true;
        xSemaphoreGive(mutex_);
    }
    publishStatus();  // 先发 SCANNING

    wifi_scan_config_t sc = {};
    sc.ssid = nullptr;
    sc.bssid = nullptr;
    sc.channel = 0;
    sc.show_hidden = true;
    // D6 硬件修正：ACTIVE 扫描的 probe 发射在 USB 供电下触发 ESP32
    // POWERON_RESET（欠压）；PASSIVE 无探针发射，电流平稳，v0.1 足够
    // 枚举 SSID/RSSI/auth（WPA2 均来自 beacon）。扫描时长略增（逐信道
    // 等 beacon，默认 ~100ms/信道，全频段 ≤3s）。
    sc.scan_type = WIFI_SCAN_TYPE_PASSIVE;
    sc.scan_time.passive = 120;  // 每信道被动侦听 120ms
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err == ESP_ERR_WIFI_NOT_STARTED) {
        // UART bootstrap 首次懒初始化后驱动仅 init（未 start；applyConfig 才会
        // stop→start）：扫描前必须先 esp_wifi_start，启动后重试一次。
        // 已由其他组件（WifiSta/TcpTransport）启动时首次 scan 即成功，不走到这里。
        ESP_LOGI(kTag, "wifi driver not started -> esp_wifi_start then rescan");
        const esp_err_t startErr = esp_wifi_start();
        if (startErr != ESP_OK) {
            ESP_LOGE(kTag, "esp_wifi_start failed: %s", esp_err_to_name(startErr));
            setError(12);  // kApiError
            // M7-E：扫描未启动 → 事务终态 Error（尚未挂起，恢复 no-op）。
            scanTransaction_.onScanStarted(false);
            return;
        }
        err = esp_wifi_scan_start(&sc, false);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        setError(5);  // kScanFailed
        // M7-E：扫描启动失败 → 事务终态 Error（尚未挂起，恢复 no-op）。
        scanTransaction_.onScanStarted(false);
        return;
    }
    // M7-E：扫描已启动 → 挂起 OLED（失败由事务记录），进入 DisplaySuspended。
    scanTransaction_.onScanStarted(suspendDisplay());
}

void WifiProvisioning::applyConfig(Command cmd) {
    const size_t ssidLen = std::strlen(cmd.ssid);
    const size_t passLen = std::strlen(cmd.password);
    if (ssidLen == 0 || ssidLen > 32 || (passLen != 0 && (passLen < 8 || passLen > 63)) ||
        cmd.serverIp == 0 || cmd.serverPort == 0) {
        // M7-F：错误路径同样清零栈上密码副本（AF.4；正常不可达，防御性清理）。
        std::memset(cmd.password, 0, sizeof(cmd.password));
        setError(2);  // kInvalidParam（防御：requestConfig 已校验）
        return;
    }

    const esp_err_t r = ensureWifiReady();
    if (r != ESP_OK) {
        std::memset(cmd.password, 0, sizeof(cmd.password));
        setError(12);  // kApiError
        return;
    }

    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        phase_ = 2;  // kConfigApplying
        errorCode_ = 0;
        serverIp_ = cmd.serverIp;
        serverPort_ = cmd.serverPort;
        statusDirty_ = true;
        xSemaphoreGive(mutex_);
    }
    publishStatus();  // 先发 CONFIG_APPLYING

    // AF.4：esp_wifi_stop → set_config → start → connect（重连不需重启）。
    esp_wifi_stop();  // 未启动时返回 ESP_ERR_WIFI_NOT_STARTED，忽略。
    wifi_config_t sta = {};
    std::strncpy(reinterpret_cast<char*>(sta.sta.ssid), cmd.ssid, sizeof(sta.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(sta.sta.password), cmd.password,
                 sizeof(sta.sta.password) - 1);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        err = esp_wifi_connect();
    }
    // 应用后立即清零本地凭据副本（驱动已持有 RAM 副本；AF.4）。
    std::memset(sta.sta.password, 0, sizeof(sta.sta.password));
    std::memset(sta.sta.ssid, 0, sizeof(sta.sta.ssid));
    std::memset(cmd.password, 0, sizeof(cmd.password));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "wifi config apply failed: %s", esp_err_to_name(err));
        setError(12);  // kApiError
        return;
    }

    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        configured_ = true;
        ssidLen_ = static_cast<uint8_t>(ssidLen);
        std::memcpy(ssid_, cmd.ssid, ssidLen);
        ssid_[ssidLen] = 0;
        phase_ = 3;  // kWifiConnecting
        errorCode_ = 0;
        connectStartMs_ = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        statusDirty_ = true;
        xSemaphoreGive(mutex_);
    }
}

void WifiProvisioning::applyClear() {
    zeroPasswordBuffers();
    {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        configured_ = false;
        ssidLen_ = 0;
        std::memset(ssid_, 0, sizeof(ssid_));
        serverIp_ = 0;
        serverPort_ = 0;
        ip_ = 0;
        rssi_ = -128;
        channel_ = 0;
        connectStartMs_ = 0;
        phase_ = 9;  // kCleared
        errorCode_ = 0;
        statusDirty_ = true;
        xSemaphoreGive(mutex_);
    }
    esp_wifi_disconnect();  // 已连接/连接中则断开（不自动重连）。
    wifi_config_t empty = {};
    esp_wifi_set_config(WIFI_IF_STA, &empty);  // 清驱动内凭据（RAM 存储）。
}

void WifiProvisioning::setError(uint16_t code) {
    if (mutex_ == nullptr) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    phase_ = 8;  // kError
    errorCode_ = code;
    statusDirty_ = true;
    xSemaphoreGive(mutex_);
}

void WifiProvisioning::publishStatus() {
    if (!cb_.onStatus || mutex_ == nullptr) {
        return;
    }
    WifiProvStatus snap;
    bool dirty = false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (statusDirty_) {
        statusDirty_ = false;  // 先清再回调：回调期间的新变化会重新置位。
        snap.phase = phase_;
        snap.errorCode = errorCode_;
        snap.flags = 0;
        snap.rssi = rssi_;
        snap.channel = channel_;
        snap.ip = ip_;
        snap.serverIp = serverIp_;
        snap.serverPort = serverPort_;
        snap.ssidLen = ssidLen_;
        std::memcpy(snap.ssid, ssid_, sizeof(snap.ssid));
        dirty = true;
    }
    xSemaphoreGive(mutex_);
    if (dirty) {
        cb_.onStatus(snap);
    }
}

bool WifiProvisioning::isConfigured() const {
    if (mutex_ == nullptr) {
        return false;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool c = configured_;
    xSemaphoreGive(mutex_);
    return c;
}

WifiProvStatus WifiProvisioning::statusSnapshot() const {
    WifiProvStatus snap;
    if (mutex_ == nullptr) {
        return snap;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snap.phase = phase_;
    snap.errorCode = errorCode_;
    snap.flags = 0;
    snap.rssi = rssi_;
    snap.channel = channel_;
    snap.ip = ip_;
    snap.serverIp = serverIp_;
    snap.serverPort = serverPort_;
    snap.ssidLen = ssidLen_;
    std::memcpy(snap.ssid, ssid_, sizeof(snap.ssid));
    xSemaphoreGive(mutex_);
    return snap;
}

}  // namespace espview
