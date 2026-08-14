// ESPView — UartTransport（ESP32，M1-1）。
//
// 规范来源：docs/DESIGN.md E/F 节 + M1-1 任务书。
// 职责：打开 UART、RX task 收字节并转发 dataCallback、send() 写字节、
//   stateCallback 上报自身状态、提供 mtu()。
// 不解析 Packet/Message/Frame，不丢帧/合并 RECT/生成 FULL（属于上层）。
//
// 线程模型：
//   - RX task：uart_read_bytes -> dataCallback（回调内禁止调用本对象方法/重入）。
//   - send()：调用者线程同步写入 UART driver TX ring buffer；先检查空闲空间，
//     不足返回 ESP_ERR_TIMEOUT（背压），不长期阻塞调用者。
//   - close()：置停止标志 -> 通知 RX task 退出 -> 等待退出 -> 删除 driver。
//   - driverMutex_ 串行化 send/close 对 driver 的访问，避免 use-after-free。
//
// 缓冲区模型：ESP-IDF UART driver RX/TX ring buffer 由 driver 持有；
//   RX task 使用栈上局部 buffer，dataCallback 指针仅在调用期间有效。
//
// 状态语义：open 成功 = Connected（仅 UART/driver 可用）；open 失败 = Error；
//   close = Disconnected。"PC 已连接/HANDSHAKE 完成"由 Protocol 层判定。

#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espview/transport.hpp"

namespace espview {

// UartTransport 配置。默认值引用当前开发板（CH340 -> UART0, GPIO1/3, 921600 8N1），
// 板级引脚通过 menuconfig（CONFIG_ESPVIEW_UART_*）覆盖，不写死在 Transport 核心。
struct UartTransportConfig : public TransportConfig {
    uart_port_t port = static_cast<uart_port_t>(CONFIG_ESPVIEW_UART_PORT);
    int tx_pin = CONFIG_ESPVIEW_UART_TX_GPIO;
    int rx_pin = CONFIG_ESPVIEW_UART_RX_GPIO;
    int baud_rate = CONFIG_ESPVIEW_UART_BAUD;
    size_t rx_buffer_size = CONFIG_ESPVIEW_UART_RX_BUF;
    size_t tx_buffer_size = CONFIG_ESPVIEW_UART_TX_BUF;
};

class UartTransport : public ITransport {
public:
    UartTransport() = default;
    ~UartTransport() override;

    esp_err_t open(const TransportConfig& cfg) override;
    void close() override;
    bool isConnected() const override;
    esp_err_t send(const uint8_t* data, size_t len) override;
    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;
    size_t mtu() const override;

private:
    static void rxTaskEntry(void* arg);
    void rxTaskLoop();
    void setState(State s);

    // driverMutex_：串行化所有 UART driver 操作（send 的写、close 的删除）。
    SemaphoreHandle_t driverMutex_ = nullptr;
    // rxFinished_：RX task 退出信号（close 等待用）。
    SemaphoreHandle_t rxFinished_ = nullptr;
    // stateMutex_：保护状态/回调/运行标志。
    mutable SemaphoreHandle_t stateMutex_ = nullptr;

    TaskHandle_t rxTask_ = nullptr;
    bool rxRunning_ = false;  // RX task 运行标志（stateMutex_ 保护）

    State state_ = State::Disconnected;
    bool opened_ = false;
    uart_port_t port_ = UART_NUM_0;
    size_t mtu_ = 0;
    DataCallback dataCb_;
    StateCallback stateCb_;
};

}  // namespace espview
