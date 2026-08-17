// ESPView — UartTransport（ESP32，M1-1 / M8-A3 收敛到 canonical ITransport）。
//
// 规范来源：docs/DESIGN.md E/F 节 + M1-1 任务书 + M8-A3（§三十五）。
// 职责：打开 UART、RX task 收字节并转发 dataCallback、send() 写字节、
//   stateCallback 上报自身状态、capabilities() 报告 {mtu, paced=true}。
// 不解析 Packet/Message/Frame，不丢帧/合并 RECT/生成 FULL（属于上层）。
// 直接实现 shared/transport 的 espview::transport::ITransport（唯一 canonical，
//   不再有 legacy espview::ITransport / adapter 层）；配置经构造函数注入，
//   open() 无参数（§三十五.1）。
//
// 线程模型：
//   - RX task：uart_read_bytes -> dataCallback（回调内禁止调用本对象方法/重入）。
//   - send()：调用者线程同步写入 UART driver TX ring buffer；先检查空闲空间，
//     不足返回 kBackpressure（would-block），不长期阻塞调用者。
//   - close()：置停止标志 -> 通知 RX task 退出 -> 等待退出 -> 删除 driver。
//     M8-A3（审计 R3）：RX 任务未及时退出时【不删除】driver（UAF 防护），
//     标记泄漏并保留全部同步原语存活。
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
#include <memory>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "espview/transport_base.hpp"

namespace espview {

// UartTransport 配置（构造时注入；menuconfig CONFIG_ESPVIEW_UART_* 默认值）。
struct UartTransportConfig {
    uart_port_t port = static_cast<uart_port_t>(CONFIG_ESPVIEW_UART_PORT);
    int tx_pin = CONFIG_ESPVIEW_UART_TX_GPIO;
    int rx_pin = CONFIG_ESPVIEW_UART_RX_GPIO;
    int baud_rate = CONFIG_ESPVIEW_UART_BAUD;
    size_t rx_buffer_size = CONFIG_ESPVIEW_UART_RX_BUF;
    size_t tx_buffer_size = CONFIG_ESPVIEW_UART_TX_BUF;
};

// enable_shared_from_this（M8-A3 B2）：仅在泄漏（RX 任务未退出）降级路径自持
// shared_ptr 续命 —— close() 正常路径直接释放；泄漏路径保留（对象与 RX 任务
// 同寿命，杜绝 UAF）。所有生产路径经 factory make_shared 创建；open() 对非
// shared_ptr 所有权显式拒绝。
class UartTransport : public TransportBase, public std::enable_shared_from_this<UartTransport> {
public:
    explicit UartTransport(UartTransportConfig cfg = {});
    ~UartTransport() override;

    bool open() override;
    void close() override;
    bool isConnected() const override;
    espview::transport::SendStatus send(const uint8_t* data, size_t len) override;
    const espview::transport::TransportCapabilities& capabilities() const override {
        return caps_;
    }

private:
    static void rxTaskEntry(void* arg);
    void rxTaskLoop();

    // driverMutex_：串行化所有 UART driver 操作（send 的写、close 的删除）。
    SemaphoreHandle_t driverMutex_ = nullptr;
    // rxFinished_：RX task 退出信号（close 等待用）。
    SemaphoreHandle_t rxFinished_ = nullptr;
    // rxLeaked_：close() 等 RX task 超时后置位 —— driver/semaphore 全部保留存活
    // （泄漏；析构不再删除，杜绝 use-after-free）。
    bool rxLeaked_ = false;
    // M8-A3（B2）：仅泄漏路径自持引用（任务未退出时对象保活；正常路径不赋值）。
    std::shared_ptr<UartTransport> selfRef_;

    TaskHandle_t rxTask_ = nullptr;
    bool rxRunning_ = false;  // RX task 运行标志（stateMutex_ 保护）

    UartTransportConfig cfg_;
    espview::transport::TransportCapabilities caps_;
    bool opened_ = false;
    uart_port_t port_ = UART_NUM_0;
};

}  // namespace espview
