// ESPView M8-A3 — TransportBase：ESP32 UART/TCP backend 共享骨架。
//
// 职责（§三十五 Transport Abstraction Semantics）：
//   - 直接实现 canonical espview::transport::ITransport 的公共部分：
//     setDataCallback / setStateCallback（锁内替换）与 setState
//     （锁内去重 + 锁外回调，回调内禁止重入本对象方法）；
//   - ScopedLock（FreeRTOS mutex RAII）、stateName（诊断日志）、
//     mapSend（esp_err_t → transport::SendStatus 唯一映射点）；
//   - 惰性创建 stateMutex_（同步原语必须在 FreeRTOS 调度器启动后创建）。
//
// 边界：不持有 driver/socket 资源（由 UartTransport / TcpTransport 各自持有并
//   管理生命周期）；open()/close()/isConnected()/send()/capabilities() 由派生类实现。
// 错误路径不使用异常。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "transport.h"  // shared/transport：唯一 canonical ITransport

namespace espview {

class TransportBase : public espview::transport::ITransport {
public:
    using ITransport = espview::transport::ITransport;
    using DataCallback = ITransport::DataCallback;
    using StateCallback = ITransport::StateCallback;
    using State = ITransport::State;

    explicit TransportBase(const char* tag) : tag_(tag) {}
    ~TransportBase() override = default;

    // 回调注册（canonical 接口实现；open 前设置、close 后不得使用）。
    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;

    // 状态上报：锁内去重 + 锁外回调（回调内禁止重入本对象方法）。
    void setState(State s);

    // esp_err_t → SendStatus（唯一映射点，M8-A3）：
    //   ESP_OK → kOk；ESP_ERR_TIMEOUT → kBackpressure（would-block）；
    //   ESP_ERR_INVALID_STATE → kNotConnected（未开/已断开）；
    //   其余 → kError。
    static espview::transport::SendStatus mapSend(esp_err_t err);

    static const char* stateName(State s);

    // 简单 RAII 锁（FreeRTOS mutex；portMAX_DELAY 等待）。
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

protected:
    // 惰性创建 stateMutex_；失败返回 false（open() 应返回 false）。
    // M8-A3（B M6）：非线程安全 —— 必须在 FreeRTOS 调度器启动后、且在其他
    // 线程访问回调/状态前，由单一管理线程（open 调用者）先调用一次。
    bool ensureStateMutex();

    mutable SemaphoreHandle_t stateMutex_ = nullptr;  // 保护 state_/回调
    State state_ = State::kDisconnected;
    DataCallback dataCb_;
    StateCallback stateCb_;

private:
    const char* tag_;
};

}  // namespace espview
