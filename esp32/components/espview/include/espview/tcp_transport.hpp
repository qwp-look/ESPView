// ESPView M6-A — TcpTransport（ESP32，Wi-Fi STA + TCP Client）。
//
// 规范来源：M6-A 任务书 §二/§四/§六/§七/§二十二/§二十三/§二十四/§二十五/§二十六。
// 职责（与 UartTransport 同构）：open / close / send / isConnected / dataCallback /
//   stateCallback / mtu()。Transport 只负责 TCP 字节流，不理解 Packet/Message/Frame/
//   CRC/HELLO（§六）；HELLO/会话全部属于 Protocol 层。
//
// 状态分离（§四）：Wi-Fi connected（WifiSta 内部）≠ TCP connected（本类 State）≠
//   Protocol connected（ProtocolEndpoint SessionState）。本类 State 只在 TCP 链路
//   建立/断开时变化；Wi-Fi 状态由 WifiSta 日志 + 内部重连驱动。
//
// TCP 是字节流（§七）：一次 recv() 可能得到半个/多个 Packet，本类原样转发
//   dataCallback，不做任何协议解析；recv 用 select 超时轮询（rx_timeout），
//   保证 close() 及时退出。send() 实现 sendAll（§二十三）：循环处理 short write，
//   直到全部发送或 error/timeout/disconnect；调用方（ProtocolEndpoint）不处理
//   TCP short write。
//
// 线程模型（§二十二）：
//   - link 任务：Wi-Fi 等待 + TCP 连接/重连（指数/固定退避）；连接成功后启动 RX 任务；
//   - RX 任务：select(rx_timeout) → recv → dataCallback；断开（recv=0/错误）→ 通知 link；
//   - send()：任意调用线程，sockMutex_ 串行化；SO_SNDTIMEO 保证不无限阻塞；
//   - close()：置停止标志 → 唤醒 link → 等待 link 退出 → 关闭 socket → 等待 RX 退出。
//   - 所有 fd 关闭都在 sockMutex_ 内（shutdown+close），避免 double close / use-after-close。
//
// 安全（§二十六/§三十七）：Wi-Fi 凭据只经 WifiStaConfig 传入，本类不打印 SSID/密码。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espview/transport.hpp"
#include "espview/wifi_sta.hpp"

namespace espview {

// TcpTransport 配置。默认值引用 Kconfig（CONFIG_ESPVIEW_TCP_*），本地测试凭据
// （Wi-Fi SSID/密码）来自 menuconfig / 未跟踪 sdkconfig，不写死在源码。
struct TcpTransportConfig : public TransportConfig {
    const char* server_ip = "";        // PC TCP server IPv4（本地测试配置/handoff 凭据）
    uint16_t server_port = 8765;       // PC TCP server port
    uint32_t connect_timeout_ms = 10000;  // TCP connect 超时
    uint32_t reconnect_delay_ms = 3000;   // 断开后重连退避
    uint32_t rx_timeout_ms = 200;         // RX select 周期（close 及时性）
    uint32_t send_timeout_ms = 5000;      // 单次 sendAll 的总预算
    size_t rx_buf = 4096;                 // RX chunk（栈上 buffer）
    WifiStaConfig wifi;                   // Wi-Fi STA 凭据/参数（adopt 模式下未用）
    // M7-G：TCP handoff 模式 —— 复用已由 WifiProvisioning 初始化的 Wi-Fi 驱动
    // （驱动已含凭据并取得 GOT_IP）。为 true 时本类不 init/deinit/startConnect/
    // waitForIp：直接进入 TCP connect 阶段（UART bootstrap → TCP handoff）。
    bool adopt_existing_wifi = false;
};

class TcpTransport : public ITransport {
public:
    TcpTransport() = default;
    ~TcpTransport() override;

    esp_err_t open(const TransportConfig& cfg) override;
    void close() override;
    bool isConnected() const override;
    esp_err_t send(const uint8_t* data, size_t len) override;
    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;
    size_t mtu() const override;

    // 诊断（日志用；不回绕）。M7-B：原子化 —— diagSnapshot 由 OLED 任务读取、
    // link 任务写入，存在真实数据竞争（原裸 uint32_t 未同步）。
    uint32_t reconnectCount() const {
        return reconnectCount_.load(std::memory_order_relaxed);
    }
    uint64_t rxBytes() const { return rxBytes_.load(); }
    uint64_t txBytes() const { return txBytes_.load(); }
    // M6-E §22：当前关联 AP 信息（rssi/channel；仅诊断，无 wire 影响）。
    bool wifiApInfo(int8_t* rssi, uint8_t* channel) const { return wifi_.apInfo(rssi, channel); }

private:
    static void linkTaskEntry(void* arg);
    static void rxTaskEntry(void* arg);
    void linkLoop();
    void rxLoop();
    bool connectOnce(int& outFd);   // 非阻塞 connect + select 超时；成功返回 true
    void startRxTask();
    void closeSocketLocked();       // 已持 sockMutex_：shutdown + close + sock_=-1
    void notifyLink();              // 唤醒 link 任务（断开/关闭）
    void setState(State s);

    // 同步原语（惰性创建，须在调度器启动后）。
    SemaphoreHandle_t sockMutex_ = nullptr;  // 保护 sock_/connected_
    SemaphoreHandle_t stateMutex_ = nullptr; // 保护 state_/回调/标志
    SemaphoreHandle_t linkWake_ = nullptr;   // link 任务等待（断开通知/关闭）
    SemaphoreHandle_t taskExit_ = nullptr;   // link/RX 任务退出计数（close join）

    TaskHandle_t linkTask_ = nullptr;
    TaskHandle_t rxTask_ = nullptr;
    bool running_ = false;   // open 后 true；close 后 false（stateMutex_ 保护）

    int sock_ = -1;          // 当前 TCP fd（sockMutex_ 保护；-1 = 未连接）
    bool connected_ = false; // TCP 层连接状态（sockMutex_ 保护）

    TcpTransportConfig cfg_{};
    WifiSta wifi_;

    size_t mtu_ = 0;
    DataCallback dataCb_;
    StateCallback stateCb_;
    State state_ = State::Disconnected;

    std::atomic<uint32_t> reconnectCount_{0};  // link 任务写，OLED diag 任务读（M7-B 原子化）
    std::atomic<uint64_t> rxBytes_{0};  // 统计（§三十四 Transport 子域）
    std::atomic<uint64_t> txBytes_{0};
};

}  // namespace espview
