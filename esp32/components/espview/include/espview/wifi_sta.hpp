// ESPView M6-A — Wi-Fi STA 管理器（ESP32 Station mode）。
//
// 规范来源：M6-A 任务书 §三/§四/§五/§二十六/§三十七 + ESP-IDF v6.0.2 station 示例。
// 职责：STA 初始化（esp_netif / esp_event / esp_wifi）、发起连接、等待 DHCP GOT_IP、
//   Wi-Fi 断开自动重连（事件驱动 + 有界重试）；**不接触 TCP / 协议**。
// 状态分离（§四）：Wi-Fi connected ≠ TCP connected ≠ Protocol connected。
//   WifiSta 只表达 Wi-Fi 链路本身：DISCONNECTED → CONNECTING → CONNECTED(GOT_IP)。
//
// 安全（§三十七）：凭据（SSID/密码）由 menuconfig / 未跟踪 sdkconfig 注入
//   （CONFIG_ESPVIEW_WIFI_SSID / CONFIG_ESPVIEW_WIFI_PASSWORD），本类只接收
//   外部传入的指针，绝不把密码写进日志/错误信息/协议字节。
//
// 线程：事件处理在 esp_event 任务执行（只更新标志 + 唤醒等待者）；
//   waitForIp() 由 TcpTransport link 任务调用，阻塞等待。
// 依赖：esp_wifi / esp_netif / esp_event（组件 REQUIRES 声明）。

#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace espview {

// Wi-Fi STA 配置（Kconfig 字符串由调用方持有，生命周期 ≥ transport）。
struct WifiStaConfig {
    const char* ssid = "";     // 目标 AP SSID（本地开发凭据，不落 source/git/log）
    const char* password = ""; // 目标 AP 密码（同上；仅本机未跟踪配置）
    bool ps_none = false;      // 实验性 WIFI_PS_NONE（延迟优化，非生产默认）
};

class WifiSta {
public:
    // 初始化 netif/event/wifi 并应用配置（幂等；重复调用返回 ESP_ERR_INVALID_STATE）。
    // 不阻塞：不自动连接，调用方随后 startConnect()。
    esp_err_t init(const WifiStaConfig& cfg);
    // 卸载：停止 Wi-Fi、注销事件 handler、清理 netif。幂等。
    void deinit();

    // 发起连接（幂等；断开后自动重连事件在内部驱动）。
    esp_err_t startConnect();

    // 阻塞等待 GOT_IP（含重连）。返回 true = 有 IP。timeoutMs=0 表示无限等待。
    bool waitForIp(uint32_t timeoutMs);

    // 只读快照（日志用）。
    bool isWifiConnected() const { return wifiConnected_; }
    // M6-E §22：当前关联 AP 信息（rssi dBm / primary channel）。
    // 未连接或读取失败返回 false，输出参数保持不变。绝不打印凭据。
    bool apInfo(int8_t* rssi, uint8_t* channel) const;
    bool hasIp() const { return gotIp_; }
    const char* ipStr() const { return ipBuf_; }
    bool isInitialized() const { return initDone_; }

private:
    static void eventHandler(void* arg, esp_event_base_t base, int32_t id, void* data);
    void handleEvent(esp_event_base_t base, int32_t id, void* data);

    // 事件位：GOT_IP 置 kGotIpBit；STA_DISCONNECTED（重试耗尽/明确失败）置 kFailBit。
    static constexpr EventBits_t kGotIpBit = BIT0;
    static constexpr EventBits_t kFailBit = BIT1;

    EventGroupHandle_t eventGroup_ = nullptr;
    esp_netif_t* netif_ = nullptr;
    esp_event_handler_instance_t wifiAny_ = nullptr;
    esp_event_handler_instance_t ipGotIp_ = nullptr;
    bool initDone_ = false;
    bool psNone_ = false;
    volatile bool wifiConnected_ = false;
    volatile bool gotIp_ = false;
    char ipBuf_[16] = {};  // "x.x.x.x\0"
};

}  // namespace espview
