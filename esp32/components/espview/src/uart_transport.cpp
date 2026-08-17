// ESPView — UartTransport 实现（见 uart_transport.hpp）。

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
// M8-A3：close 等待 RX 退出预算 —— 覆盖 ≥ 多个 RX 读周期（50ms×N）+ 调度余量；
// 超时即泄漏（UAF 防护），绝不删除仍在被使用的 driver。
constexpr TickType_t kCloseWaitTicks = pdMS_TO_TICKS(500);
constexpr TickType_t kMutexTakeTicks = pdMS_TO_TICKS(100);
constexpr UBaseType_t kRxTaskPriority = 5;

}  // namespace

UartTransport::UartTransport(UartTransportConfig cfg)
    : TransportBase(kTag), cfg_(std::move(cfg)) {
    caps_.mtu = cfg_.tx_buffer_size;   // UART TX ring buffer 即单次 send 硬上限
    caps_.paced = true;                // UART：按 wire 速率节流/背压重试
}

UartTransport::~UartTransport() {
    close();
    if (rxLeaked_) {
        // RX 任务未退出：driver/semaphore 全部保留存活（泄漏，避免 UAF）。
        ESP_LOGE(kTag, "UART driver/semaphores kept alive (RX task did not exit)");
        return;
    }
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

bool UartTransport::open() {
    // 参数校验（与 Kconfig 取值范围一致）。
    if (cfg_.port < UART_NUM_0 || cfg_.port >= UART_NUM_MAX || cfg_.baud_rate <= 0 ||
        cfg_.rx_buffer_size == 0 || cfg_.tx_buffer_size == 0) {
        setState(State::kError);
        return false;
    }
    if (rxLeaked_) {
        // M8-A3（B M1）：泄漏后不可 reopen —— RX 任务可能仍存活，对象被自持
        // 保活（driver/semaphore 保留）。重开需先销毁对象（泄漏）再新建实例。
        ESP_LOGE(kTag, "open rejected: RX task leaked (object pinned alive)");
        setState(State::kError);
        return false;
    }

    // 同步原语惰性创建（必须在 FreeRTOS 调度器启动后调用 open）。
    if (driverMutex_ == nullptr) {
        driverMutex_ = xSemaphoreCreateMutex();
    }
    if (rxFinished_ == nullptr) {
        rxFinished_ = xSemaphoreCreateBinary();
    }
    if (!ensureStateMutex()) {
        setState(State::kError);
        return false;
    }
    if (driverMutex_ == nullptr || rxFinished_ == nullptr) {
        setState(State::kError);
        return false;
    }

    setState(State::kConnecting);

    if (xSemaphoreTake(driverMutex_, kMutexTakeTicks) != pdTRUE) {
        return false;  // 锁忙：明确失败（管理任务串行化下不应发生）
    }

    // 重复 open：明确报错，不做隐式重开。
    {
        ScopedLock lock(stateMutex_);
        if (opened_) {
            xSemaphoreGive(driverMutex_);
            return false;
        }
    }

    port_ = cfg_.port;

    // 1) 安装 driver（RX/TX ring buffer；无事件队列，RX 用 task 轮询读取）。
    esp_err_t err = uart_driver_install(port_, static_cast<int>(cfg_.rx_buffer_size),
                                        static_cast<int>(cfg_.tx_buffer_size), 0, nullptr, 0);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_driver_install(port=%d) failed: %s", static_cast<int>(port_),
                 esp_err_to_name(err));
        xSemaphoreGive(driverMutex_);
        setState(State::kError);
        return false;
    }

    // 2) 参数配置：921600 8N1，无流控（DESIGN.md / M1-1 任务书）。
    uart_config_t uartCfg = {};
    uartCfg.baud_rate = cfg_.baud_rate;
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
        setState(State::kError);
        return false;
    }

    // 3) 引脚映射（-1 = 保持 driver 默认；RTS/CTS 不变）。
    err = uart_set_pin(port_, cfg_.tx_pin, cfg_.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(port_);
        xSemaphoreGive(driverMutex_);
        setState(State::kError);
        return false;
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
        setState(State::kError);
        return false;
    }

    {
        ScopedLock lock(stateMutex_);
        opened_ = true;
    }
    // 先释放 driverMutex_ 再上报 Connected：状态回调会同步触发上层立即发送 HELLO
    // （send() 需要 driverMutex_）；否则首次发送会因互斥锁超时被静默丢弃。
    xSemaphoreGive(driverMutex_);
    setState(State::kConnected);
    ESP_LOGI(kTag, "open OK: port=%d tx=%d rx=%d baud=%d", static_cast<int>(port_),
             cfg_.tx_pin, cfg_.rx_pin, cfg_.baud_rate);
    return true;
}

void UartTransport::close() {
    if (driverMutex_ == nullptr) {
        return;  // 从未 open（无同步原语）
    }
    // M8-A3（审计 R3）：不得因 driverMutex_ 拿不到就跳过停止序列 —— send 持锁
    // 仅覆盖单次 uart_write_bytes（有界），此处等待其完成后再走停止序列。
    if (xSemaphoreTake(driverMutex_, portMAX_DELAY) != pdTRUE) {
        return;  // 理论不可达（锁存在且无持有者泄漏）
    }

    bool wasOpen = false;
    {
        ScopedLock lock(stateMutex_);
        wasOpen = opened_;
        opened_ = false;
        rxRunning_ = false;
    }

    if (wasOpen) {
        TaskHandle_t handle = nullptr;
        {
            // M8-A3（B3）：锁内读句柄 + notify —— RX 任务退出路径在同一锁下清
            // rxTask_ 后再 vTaskDelete，杜绝对已删除 TCB 的 notify（stale TCB）。
            ScopedLock lock(stateMutex_);
            handle = rxTask_;
            if (handle != nullptr) {
                xTaskNotifyGive(handle);  // 不阻塞；任务已清句柄则此处读到 null
            }
        }
        if (handle != nullptr && xSemaphoreTake(rxFinished_, kCloseWaitTicks) != pdTRUE) {
            // M8-A3（审计 R3 / B2）：RX 任务未退出 → 不得 uart_driver_delete（UAF）。
            // 清回调 + 状态落定（泄漏后不可 reopen，B M2）；对象经自持 shared_ptr
            // 保活（与任务同寿命），driver/semaphore 全部保留存活（安全泄漏）。
            rxLeaked_ = true;
            {
                ScopedLock lock(stateMutex_);
                dataCb_ = nullptr;
                stateCb_ = nullptr;
                state_ = State::kError;
            }
#if defined(__cpp_exceptions) && __cpp_exceptions
            try {
                selfRef_ = shared_from_this();  // 从 close 调用者的引用续命
            } catch (const std::bad_weak_ptr&) {
                // close() 在析构路径调用且任务未退出：无法保活（违反
                // close-before-destroy 契约 + 泄漏双故障），保守记录。
                ESP_LOGE(kTag, "leak while destructing: cannot pin object");
            }
#else
            // -fno-exceptions：bad_weak_ptr 会 terminate，不能尝试续命，仅记录。
            ESP_LOGE(kTag, "leak while destructing: cannot pin object");
#endif
            xSemaphoreGive(driverMutex_);
            ESP_LOGE(kTag, "RX task did not exit; leaking UART driver (UAF guard)");
            return;
        }
        uart_driver_delete(port_);
    }

    {
        ScopedLock lock(stateMutex_);
        rxTask_ = nullptr;
        port_ = UART_NUM_0;
    }

    xSemaphoreGive(driverMutex_);
    if (wasOpen) {
        setState(State::kDisconnected);  // M8-A3（B M7）：从未 open 的实例不上报伪状态
    }
    ESP_LOGI(kTag, "closed");
}

bool UartTransport::isConnected() const {
    ScopedLock lock(stateMutex_);
    return opened_ && state_ == State::kConnected;
}

espview::transport::SendStatus UartTransport::send(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return espview::transport::SendStatus::kError;
    }
    if (driverMutex_ == nullptr) {
        return espview::transport::SendStatus::kNotConnected;  // 从未 open
    }
    if (xSemaphoreTake(driverMutex_, kMutexTakeTicks) != pdTRUE) {
        return espview::transport::SendStatus::kBackpressure;  // 门忙：would-block
    }

    esp_err_t err = ESP_OK;
    uart_port_t port = UART_NUM_0;
    {
        ScopedLock lock(stateMutex_);
        if (!opened_ || state_ != State::kConnected) {
            err = ESP_ERR_INVALID_STATE;
        } else if (len > caps_.mtu) {
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
            // 背压：TX ring buffer 空间不足。Transport 不理解帧，返回 would-block，
            // 由上层（协议/帧层）按 TxPolicy 决定重试或整帧丢弃。
            err = ESP_ERR_TIMEOUT;
        } else {
            const int n = uart_write_bytes(port, data, len);
            err = (n == static_cast<int>(len)) ? ESP_OK : ESP_FAIL;
        }
    }

    xSemaphoreGive(driverMutex_);
    return mapSend(err);
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
        uart_port_t port = UART_NUM_0;
        {
            ScopedLock lock(stateMutex_);
            if (!rxRunning_) {
                break;
            }
            port = port_;  // M8-A3（B M5）：锁内取端口副本，避免与 close() 写竞态
        }

        const int n = uart_read_bytes(port, buf, sizeof(buf), kRxReadTimeoutTicks);
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
            ESP_LOGE(kTag, "uart_read_bytes failed on port %d", static_cast<int>(port));
            setState(State::kError);
            break;
        }
    }
    // M8-A3（B3）：锁内清句柄后再 give+delete —— close() 同锁读句柄并 notify，
    // 不会对已删除 TCB 发 notify（stale TCB 竞态关闭）。
    {
        ScopedLock lock(stateMutex_);
        rxTask_ = nullptr;
    }
    xSemaphoreGive(rxFinished_);
    vTaskDelete(nullptr);
}

}  // namespace espview
