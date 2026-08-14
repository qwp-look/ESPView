// ESPView M6-C — ESP32 Transport 适配层实现（见 transport_manager.hpp）。

#include "espview/transport_manager.hpp"

#include <utility>

namespace espview {
namespace transport {

// ---- Esp32UartAdapter ----
Esp32UartAdapter::Esp32UartAdapter(const ::espview::UartTransportConfig& cfg)
    : uartCfg_(cfg), caps_(uartCaps(cfg.tx_buffer_size)) {
    inner_.setDataCallback([this](const uint8_t* d, size_t n) { onInnerData(d, n); });
    inner_.setStateCallback([this](::espview::ITransport::State s) { onInnerState(s); });
}

Esp32UartAdapter::~Esp32UartAdapter() {
    inner_.close();
}

bool Esp32UartAdapter::open() {
    return inner_.open(uartCfg_) == ESP_OK;
}

void Esp32UartAdapter::close() {
    inner_.close();
}

bool Esp32UartAdapter::isConnected() const {
    return inner_.isConnected();
}

SendStatus Esp32UartAdapter::send(const uint8_t* data, size_t len) {
    return mapSend(inner_.send(data, len));
}

void Esp32UartAdapter::setDataCallback(DataCallback cb) {
    dataCb_ = std::move(cb);
}

void Esp32UartAdapter::setStateCallback(StateCallback cb) {
    stateCb_ = std::move(cb);
}

size_t Esp32UartAdapter::mtu() const {
    return inner_.mtu();
}

void Esp32UartAdapter::onInnerState(::espview::ITransport::State s) {
    if (stateCb_) {
        stateCb_(mapState(s));
    }
}

void Esp32UartAdapter::onInnerData(const uint8_t* data, size_t len) {
    if (dataCb_) {
        dataCb_(data, len);
    }
}

// ---- Esp32TcpAdapter ----
Esp32TcpAdapter::Esp32TcpAdapter(const ::espview::TcpTransportConfig& cfg)
    : tcpCfg_(cfg), caps_(tcpCaps()) {
    inner_.setDataCallback([this](const uint8_t* d, size_t n) { onInnerData(d, n); });
    inner_.setStateCallback([this](::espview::ITransport::State s) { onInnerState(s); });
}

Esp32TcpAdapter::~Esp32TcpAdapter() {
    inner_.close();
}

bool Esp32TcpAdapter::open() {
    return inner_.open(tcpCfg_) == ESP_OK;
}

void Esp32TcpAdapter::close() {
    inner_.close();
}

bool Esp32TcpAdapter::isConnected() const {
    return inner_.isConnected();
}

SendStatus Esp32TcpAdapter::send(const uint8_t* data, size_t len) {
    return mapSend(inner_.send(data, len));
}

void Esp32TcpAdapter::setDataCallback(DataCallback cb) {
    dataCb_ = std::move(cb);
}

void Esp32TcpAdapter::setStateCallback(StateCallback cb) {
    stateCb_ = std::move(cb);
}

size_t Esp32TcpAdapter::mtu() const {
    return inner_.mtu();
}

void Esp32TcpAdapter::onInnerState(::espview::ITransport::State s) {
    if (stateCb_) {
        stateCb_(mapState(s));
    }
}

void Esp32TcpAdapter::onInnerData(const uint8_t* data, size_t len) {
    if (dataCb_) {
        dataCb_(data, len);
    }
}

}  // namespace transport
}  // namespace espview
