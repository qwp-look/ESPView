#include "espview/uart_transport.hpp"

#include <cstddef>
#include <utility>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace espview {

namespace {

const char* kTag = "espview_uart";

constexpr size_t kRxTaskStackWords = 4096;
constexpr size_t kRxReadChunk = 512;                // RX task 单次读取上限（栈上 buffer）
constexpr TickType_t kRxReadTimeoutTicks = pdMS_TO_TICKS(50);
constexpr TickType_t kCloseWaitTicks = pdMS_TO_TICKS(200);  // 等待 RX task 退出上限
constexpr TickType_t kMutexTakeTicks = pdMS_TO_TICKS(100);
constexpr UBaseType_t kRxTaskPriority = 5;

// 简单 RAII 锁（FreeRTOS mutex）。
class ScopedLock {
public:
    explicit ScopedLock(SemaphoreHandle_t m) : m_(m) {
        if (m_ != nullptr) {
            xSemaphoreTake(m_, portMAX_DELAY);
        }
    }
    ~ScopedLock() {
        if (m_ != nullptr) {
            xSemaphoreGive(m_);
        }
    }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    SemaphoreHandle_t m_;
};

const char* stateName(ITransport::State s) {
    switch (s) {
        case ITransport::State::Disconnected:
            return "Disconnected";
        case ITransport::State::Connecting:
            return "Connecting";
        case ITransport::State::Connected:
            return "Connected";
        case ITransport::State::Error:
            return "Error";
    }
    return "?";
}

}  // namespace

UartTransport::~UartTransport() {
    close();
    if (driverMutex_ != nullptr) {
        vSemaphoreDelete(driverMutex_);
    }
    if (rxFinished_ != nullptr) {
        vSemaphoreDelete(rxFinished_);
    }
    if (stateMutex_ != nullptr) {
        vSemaphoreDelete(stateMutex_);
    }
}

esp_err_t UartTransport::open(const TransportConfig& cfg) {
    // ESP-IDF 默认 -fno-rtti：调用方必须传 UartTransportConfig（接口约定）。
    const auto* ucfg = static_cast<const UartTransportConfig*>(&cfg);
    // 参数校验（与 Kconfig 取值范围一致）。
    if (ucfg->port < UART_NUM_0 || ucfg->port >= UART_NUM_MAX || ucfg->baud_rate <= 0 ||
        ucfg->rx_buffer_size == 0 || ucfg->tx_buffer_size == 0) {
        setState(State::Error);
        return ESP_ERR_INVALID_ARG;
    }

    // 同步原语惰性创建（必须在 FreeRTOS 调度器启动后调用 open）。
    if (driverMutex_ == nullptr) {
        driverMutex_ = xSemaphoreCreateMutex();
    }
    if (rxFinished_ == nullptr) {
        rxFinished_ = xSemaphoreCreateBinary();
    }
    if (stateMutex_ == nullptr) {
        stateMutex_ = xSemaphoreCreateMutex();
    }
    if (driverMutex_ == nullptr || rxFinished_ == nullptr || stateMutex_ == nullptr) {
        setState(State::Error);
        return ESP_ERR_NO_MEM;
    }

    setState(State::Connecting);

    if (xSemaphoreTake(driverMutex_, kMutexTakeTicks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // 重复 open：明确报错，不做隐式重开。
    {
        ScopedLock lock(stateMutex_);
        if (opened_) {
            xSemaphoreGive(driverMutex_);
            return ESP_ERR_INVALID_STATE;
        }
    }

    port_ = ucfg->port;
    mtu_ = ucfg->tx_buffer_size;

    // 1) 安装 driver（RX/TX ring buffer；无事件队列，RX 用 task 轮询读取）。
    esp_err_t err = uart_driver_install(port_, static_cast<int>(ucfg->rx_buffer_size),
                                        static_cast<int>(ucfg->tx_buffer_size), 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_driver_install(port=%d) failed: %s", static_cast<int>(port_),
                 esp_err_to_name(err));
        xSemaphoreGive(driverMutex_);
        setState(State::Error);
        return err;
    }

    // 2) 参数配置：921600 8N1，无流控（DESIGN.md / M1-1 任务书）。
    uart_config_t uartCfg = {};
    uartCfg.baud_rate = ucfg->baud_rate;
    uartCfg.data_bits = UART_DATA_8_BITS;
    uartCfg.parity = UART_PARITY_DISABLE;
    uartCfg.stop_bits = UART_STOP_BITS_1;
    uartCfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uartCfg.rx_flow_ctrl_thresh = 0;
    uartCfg.source_clk = UART_SCLK_DEFAULT;
    err = uart_param_config(port_, &uartCfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(port_);
        xSemaphoreGive(driverMutex_);
        setState(State::Error);
        return err;
    }

    // 3) 引脚映射（-1 = 保持 driver 默认；RTS/CTS 不变）。
    err = uart_set_pin(port_, ucfg->tx_pin, ucfg->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(port_);
        xSemaphoreGive(driverMutex_);
        setState(State::Error);
        return err;
    }

    // 4) RX task（先置运行标志，再创建，避免 task 立即退出）。
    {
        ScopedLock lock(stateMutex_);
        rxRunning_ = true;
    }
    const BaseType_t rc = xTaskCreate(rxTaskEntry, "espview_uart_rx", kRxTaskStackWords, this,
                                      kRxTaskPriority, &rxTask_);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate(rx) failed");
        rxTask_ = nullptr;
        {
            ScopedLock lock(stateMutex_);
            rxRunning_ = false;
        }
        uart_driver_delete(port_);
        xSemaphoreGive(driverMutex_);
        setState(State::Error);
        return ESP_ERR_NO_MEM;
    }

    {
        ScopedLock lock(stateMutex_);
        opened_ = true;
    }
    // 先释放 driverMutex_ 再上报 Connected：状态回调会同步触发上层立即发送 HELLO
    // （send() 需要 driverMutex_）；否则首次发送会因互斥锁超时被静默丢弃。
    xSemaphoreGive(driverMutex_);
    setState(State::Connected);
    ESP_LOGI(kTag, "open OK: port=%d tx=%d rx=%d baud=%d", static_cast<int>(port_),
             ucfg->tx_pin, ucfg->rx_pin, ucfg->baud_rate);
    return ESP_OK;
}

void UartTransport::close() {
    if (driverMutex_ == nullptr) {
        return;  // 从未 open（无同步原语）
    }
    if (xSemaphoreTake(driverMutex_, kMutexTakeTicks) != pdTRUE) {
        return;
    }

    bool wasOpen = false;
    TaskHandle_t handle = nullptr;
    {
        ScopedLock lock(stateMutex_);
        wasOpen = opened_;
        opened_ = false;
        rxRunning_ = false;
        handle = rxTask_;
    }

    if (wasOpen && handle != nullptr) {
        xTaskNotifyGive(handle);
        if (xSemaphoreTake(rxFinished_, kCloseWaitTicks) != pdTRUE) {
            ESP_LOGW(kTag, "RX task did not exit in time");
        }
    }

    if (wasOpen) {
        uart_driver_delete(port_);
    }

    {
        ScopedLock lock(stateMutex_);
        rxTask_ = nullptr;
        port_ = UART_NUM_0;
        mtu_ = 0;
    }

    xSemaphoreGive(driverMutex_);
    setState(State::Disconnected);
    ESP_LOGI(kTag, "closed");
}

bool UartTransport::isConnected() const {
    ScopedLock lock(stateMutex_);
    return opened_ && state_ == State::Connected;
}

esp_err_t UartTransport::send(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (driverMutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;  // 从未 open
    }
    if (xSemaphoreTake(driverMutex_, kMutexTakeTicks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    uart_port_t port = UART_NUM_0;
    {
        ScopedLock lock(stateMutex_);
        if (!opened_ || state_ != State::Connected) {
            err = ESP_ERR_INVALID_STATE;
        } else if (len > mtu_) {
            err = ESP_ERR_INVALID_ARG;  // 超过单次传输上限
        } else {
            port = port_;
        }
    }

    if (err == ESP_OK) {
        size_t freeSize = 0;
        const esp_err_t gerr = uart_get_tx_buffer_free_size(port, &freeSize);
        if (gerr != ESP_OK) {
            err = ESP_FAIL;
        } else if (freeSize < len) {
            // 背压：TX ring buffer 空间不足。Transport 不理解帧，返回 busy，
            // 由上层（协议/帧层）决定整帧丢弃。
            err = ESP_ERR_TIMEOUT;
        } else {
            const int n = uart_write_bytes(port, data, len);
            err = (n == static_cast<int>(len)) ? ESP_OK : ESP_FAIL;
        }
    }

    xSemaphoreGive(driverMutex_);
    return err;
}

void UartTransport::setDataCallback(DataCallback cb) {
    ScopedLock lock(stateMutex_);
    dataCb_ = std::move(cb);
}

void UartTransport::setStateCallback(StateCallback cb) {
    ScopedLock lock(stateMutex_);
    stateCb_ = std::move(cb);
}

size_t UartTransport::mtu() const {
    ScopedLock lock(stateMutex_);
    return mtu_;
}

void UartTransport::setState(State s) {
    StateCallback cb;
    {
        ScopedLock lock(stateMutex_);
        if (state_ == s) {
            return;
        }
        state_ = s;
        cb = stateCb_;
    }
    ESP_LOGI(kTag, "state -> %s", stateName(s));
    if (cb) {
        cb(s);
    }
}

void UartTransport::rxTaskEntry(void* arg) {
    auto* self = static_cast<UartTransport*>(arg);
    self->rxTaskLoop();
}

void UartTransport::rxTaskLoop() {
    uint8_t buf[kRxReadChunk];
    while (true) {
        // close() 退出信号。
        if (ulTaskNotifyTake(pdTRUE, 0) != 0) {
            break;
        }
        {
            ScopedLock lock(stateMutex_);
            if (!rxRunning_) {
                break;
            }
        }

        const int n = uart_read_bytes(port_, buf, sizeof(buf), kRxReadTimeoutTicks);
        if (n > 0) {
            DataCallback cb;
            {
                ScopedLock lock(stateMutex_);
                cb = dataCb_;
            }
            if (cb) {
                // 指针仅在回调期间有效（文档约束：上层不得缓存）。
                cb(buf, static_cast<size_t>(n));
            }
        } else if (n < 0) {
            ESP_LOGE(kTag, "uart_read_bytes failed on port %d", static_cast<int>(port_));
            setState(State::Error);
            break;
        }
    }
    xSemaphoreGive(rxFinished_);
    vTaskDelete(nullptr);
}

}  // namespace espview
