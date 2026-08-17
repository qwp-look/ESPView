// ESPView M8-A3 — TransportBase 实现（见 transport_base.hpp）。

#include "espview/transport_base.hpp"

#include <utility>

#include "esp_log.h"

namespace espview {

void TransportBase::setDataCallback(DataCallback cb) {
    ScopedLock lock(stateMutex_);
    dataCb_ = std::move(cb);
}

void TransportBase::setStateCallback(StateCallback cb) {
    ScopedLock lock(stateMutex_);
    stateCb_ = std::move(cb);
}

void TransportBase::setState(State s) {
    StateCallback cb;
    {
        ScopedLock lock(stateMutex_);
        if (state_ == s) {
            return;
        }
        state_ = s;
        cb = stateCb_;
    }
    ESP_LOGI(tag_, "state -> %s", stateName(s));
    if (cb) {
        cb(s);
    }
}

espview::transport::SendStatus TransportBase::mapSend(esp_err_t err) {
    switch (err) {
        case ESP_OK:
            return espview::transport::SendStatus::kOk;
        case ESP_ERR_TIMEOUT:
            return espview::transport::SendStatus::kBackpressure;  // would-block
        case ESP_ERR_INVALID_STATE:
            return espview::transport::SendStatus::kNotConnected;  // 未开/已断开
        default:
            return espview::transport::SendStatus::kError;
    }
}

const char* TransportBase::stateName(State s) {
    switch (s) {
        case State::kDisconnected:
            return "Disconnected";
        case State::kConnecting:
            return "Connecting";
        case State::kConnected:
            return "Connected";
        case State::kError:
            return "Error";
    }
    return "?";
}

bool TransportBase::ensureStateMutex() {
    if (stateMutex_ == nullptr) {
        stateMutex_ = xSemaphoreCreateMutex();
    }
    return stateMutex_ != nullptr;
}

}  // namespace espview
