// ESPView M7-D3 — Wi-Fi Provisioning（UART bootstrap：扫描/配置/状态上报）。
//
// 规范来源：docs/DESIGN.md AF.2（消息族）/ AF.3（传输语义）/ AF.4（凭据生命周期）。
// 职责（纯 ESP-IDF，协议无关）：
//   - 懒初始化 Wi-Fi STA（netif/event/wifi，RAM 存储），与 WifiSta 二选一持有：
//     本模块 init 成功（esp_wifi_init 返回 ESP_OK）→ 自有；返回
//     ESP_ERR_INVALID_STATE → 视为外部已初始化（TcpTransport/WifiSta），
//     只注册事件 handler 复用驱动，不创建第二个 netif。
//   - 扫描：esp_wifi_scan_start（非阻塞）→ WIFI_EVENT_SCAN_DONE → tick() 取
//     结果，按 RSSI 降序取 top-N（≤64）→ onScanResult 回调。
//   - 配置：RAM-only 凭据副本（断电即失）；esp_wifi_stop → set_config → start
//     → connect（AF.4 重连流程）；应用后清零本地 password 副本。
//   - CLEAR：flags bit0 → 清零凭据 + 断开（不重连）。
//   - 状态机：IDLE/SCANNING/CONFIG_APPLYING/WIFI_CONNECTING/WIFI_CONNECTED/
//     GOT_IP/ERROR/CLEARED；onStatus 回调（tick 去重）。TCP 相位
//     （TCP_CONNECTING/TCP_CONNECTED）由 D6 握手期外部设置，本模块不产生。
//   - 错误映射：认证类 reason → kAuthFailed；NO_AP_FOUND → kApNotFound；
//     连接后限时无 IP → kDhcpTimeout；ESP-IDF API 失败 → kApiError。
//
// 安全（AF.4）：password 只存在于本类 RAM 缓冲（命令槽 + 应用临时副本），
//   绝不进入日志/状态快照/协议字节；应用后/清除时立即清零。
//
// 线程：request* 可从任意任务调用（互斥保护，非阻塞）；tick() 由会话任务
//   （espview_sess）每 ~200ms 调用；事件 handler 在 esp_event 任务执行（只
//   更新互斥保护状态 + 标志，不做协议发送——发送由 tick 回调到 main.cpp）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace espview {

// 扫描记录宿主表示（对应 WIFI_SCAN_RESULT record 42B；ssid NUL 结尾 1..32）。
struct WifiProvScanRecord {
    char ssid[33] = {};
    uint8_t bssid[6] = {};
    int8_t rssi = -128;
    uint8_t channel = 0;
    uint8_t authmode = 0;  // ESP-IDF wifi_auth_mode_t 0..8
};

// 状态快照宿主表示（对应 WIFI_STATUS 载荷；绝无 password 字段）。
struct WifiProvStatus {
    uint8_t phase = 0;       // proto::WifiStatusPhase 数值
    uint16_t errorCode = 0;  // proto::ErrorCode 数值
    uint8_t flags = 0;
    int8_t rssi = -128;
    uint8_t channel = 0;
    uint32_t ip = 0;         // 网络序
    uint32_t serverIp = 0;   // 网络序
    uint16_t serverPort = 0;
    uint8_t ssidLen = 0;
    char ssid[33] = {};      // 当前网络名（非 secret metadata）
};

class WifiProvisioning {
public:
    struct Callbacks {
        // 状态变化（tick 内去重后回调一次；调用方转发 WIFI_STATUS，fire-and-forget）。
        std::function<void(const WifiProvStatus& status)> onStatus;
        // 扫描完成（top-N 按 RSSI 降序；records/count 仅在回调期间有效；
        // 调用方转发 WIFI_SCAN_RESULT）。
        std::function<void(uint8_t scanSeq, bool truncated, uint16_t total,
                           const WifiProvScanRecord* records, size_t count)> onScanResult;
    };

    explicit WifiProvisioning(Callbacks cb);
    ~WifiProvisioning();
    WifiProvisioning(const WifiProvisioning&) = delete;
    WifiProvisioning& operator=(const WifiProvisioning&) = delete;

    // 非阻塞命令（可从 RX 任务/会话回调调用；线程安全；无动态分配）。
    void requestScan(uint8_t maxEntries);  // 0=默认 32；上限 64（超出按 64）
    // 复制凭据到 RAM（ssid 1..32；password 0 或 8..63；serverIp 网络序非 0；
    // serverPort 1..65535——调用方负责校验，本方法对非法输入拒绝并置命令无效）。
    void requestConfig(const char* ssid, size_t ssidLen, const char* password,
                       size_t passLen, uint32_t serverIp, uint16_t serverPort);
    // CLEAR：清零凭据并断开（AF.4）。
    void requestClear();

    // 会话任务每 ~200ms 调用：处理命令队列、驱动相位机、派发 onStatus/onScanResult。
    // 必须在 esp_event 默认循环已创建后调用（main 中构造后即可，首次命令前懒 init）。
    void tick(uint64_t nowMs);

    // 只读查询（诊断/握手期；绝无敏感字段）。
    bool isConfigured() const;
    WifiProvStatus statusSnapshot() const;
    bool ownsWifi() const { return wifiOwned_; }

private:
    enum class CommandKind : uint8_t { kNone = 0, kScan = 1, kConfig = 2, kClear = 3 };

    struct Command {
        CommandKind kind = CommandKind::kNone;
        uint8_t maxEntries = 0;
        char ssid[33] = {};
        char password[65] = {};
        uint32_t serverIp = 0;
        uint16_t serverPort = 0;
    };

    static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    void handleEvent(esp_event_base_t base, int32_t id, void* data);

    esp_err_t ensureWifiReady();
    void processCommand(uint64_t nowMs);
    void startScan(uint8_t maxEntries);
    void applyConfig(Command cmd);  // 按值：应用后清零本地凭据副本
    void applyClear();
    void publishStatus();
    void setStatus(uint8_t phase, uint16_t errorCode);
    void setError(uint16_t errorCode);
    void zeroPasswordBuffers();

    Callbacks cb_;
    bool initialized_ = false;   // ensureWifiReady 成功（或外部已初始化）
    bool wifiOwned_ = false;     // 本模块成功调用 esp_wifi_init
    bool handlersRegistered_ = false;
    esp_event_handler_instance_t wifiAny_ = nullptr;
    esp_event_handler_instance_t ipGotIp_ = nullptr;

    SemaphoreHandle_t mutex_ = nullptr;

    // 命令槽（单槽；新命令覆盖旧命令——v0.1 控制面串行）。
    Command command_;
    bool commandPending_ = false;

    // 状态（互斥保护）。
    uint8_t phase_ = 0;            // WifiStatusPhase
    uint16_t errorCode_ = 0;
    int8_t rssi_ = -128;
    uint8_t channel_ = 0;
    uint32_t ip_ = 0;
    uint32_t serverIp_ = 0;
    uint16_t serverPort_ = 0;
    uint8_t ssidLen_ = 0;
    char ssid_[33] = {};
    bool configured_ = false;
    uint64_t connectStartMs_ = 0;
    uint8_t prevPhase_ = 0;        // 扫描前相位（SCAN_DONE 后恢复）

    // 扫描结果缓冲（tick 内填充/派发）。
    uint8_t scanMaxEntries_ = 32;  // 本次扫描 top-N（0=默认 32）
    bool scanDonePending_ = false;
    bool scanReady_ = false;
    uint8_t scanSeq_ = 0;
    bool scanTruncated_ = false;
    uint16_t scanTotal_ = 0;
    WifiProvScanRecord scanRecords_[64] = {};
    size_t scanCount_ = 0;

    // 状态去重（tick 只在上一次派发后变化时再回调）。
    bool statusDirty_ = true;
};

}  // namespace espview
