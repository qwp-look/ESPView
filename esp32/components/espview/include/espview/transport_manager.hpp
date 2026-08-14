// ESPView M6-C — ESP32 Transport 适配层（espview/transport_manager.hpp）
//
// 规范来源：M6-C 任务书 §三（TransportManager）/§七（Transport Capability）/
//   §八–§十三（Transport-specific pacing）+ shared/transport（host 已验证）。
//
// 内容（命名遵循任务书"transport_manager"）：
//   1. Esp32UartAdapter / Esp32TcpAdapter：把旧 espview::ITransport 实现
//      （UartTransport / TcpTransport，M1-1/M6-A）适配到 shared/transport
//      新接口 espview::transport::ITransport（open() 无参、SendStatus 返回、
//      capabilities() 报告）；状态枚举与 send 结果显式映射。
//   2. uartCaps()/tcpCaps()：与 shared/transport 测试的 uartCaps/tcpCaps 一致
//      （两端共用同一策略推导 TxPolicy）。
//
// 运行时选择由 shared/transport::TransportManager 提供（switchTo/open/close/
//   发送门）；本文件不重复实现管理逻辑。错误路径不使用异常。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "transport.h"               // shared/transport 新接口（espview::transport::ITransport）
#include "espview/transport.hpp"     // 旧接口 espview::ITransport
#include "espview/tcp_transport.hpp" // TcpTransport + TcpTransportConfig
#include "espview/uart_transport.hpp"

namespace espview {
namespace transport {

// ---- Transport Capability 报告（§七；与 host 测试共享同一策略推导）----
inline TransportCapabilities uartCaps(size_t mtu) {
    TransportCapabilities c;
    c.mtu = mtu;
    c.preferredPacketSize = mtu < 4096u ? mtu : 4096u;
    c.lowLatency = false;
    c.orderedReliableStream = true;
    c.paced = true;  // UART：按 wire 速率节流/背压重试
    return c;
}

inline TransportCapabilities tcpCaps() {
    TransportCapabilities c;
    c.mtu = 20u + 4096u;  // Packet Header(20) + 最大 payload(4096)
    c.preferredPacketSize = 4116u;
    c.lowLatency = true;  // NODELAY 已开、小包优先
    c.orderedReliableStream = true;
    c.paced = false;      // TCP：依赖 send() 自身背压（socket send buffer）
    return c;
}

// ---- 状态/发送结果映射（旧接口 → 新接口；显式映射，不依赖数值相等）----
inline ITransport::State mapState(::espview::ITransport::State s) {
    switch (s) {
        case ::espview::ITransport::State::Disconnected: return ITransport::State::kDisconnected;
        case ::espview::ITransport::State::Connecting: return ITransport::State::kConnecting;
        case ::espview::ITransport::State::Connected: return ITransport::State::kConnected;
        case ::espview::ITransport::State::Error: return ITransport::State::kError;
    }
    return ITransport::State::kError;
}

inline SendStatus mapSend(esp_err_t err) {
    if (err == ESP_OK) {
        return SendStatus::kOk;
    }
    if (err == ESP_ERR_TIMEOUT) {
        return SendStatus::kBackpressure;  // 缓冲满（上层整帧丢弃）
    }
    return SendStatus::kError;
}

// ---- UART 适配器（旧 UartTransport → 新 ITransport）----
class Esp32UartAdapter final : public ::espview::transport::ITransport {
public:
    explicit Esp32UartAdapter(const ::espview::UartTransportConfig& cfg);
    ~Esp32UartAdapter() override;

    bool open() override;
    void close() override;
    bool isConnected() const override;
    SendStatus send(const uint8_t* data, size_t len) override;
    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;
    const TransportCapabilities& capabilities() const override { return caps_; }
    size_t mtu() const override;

private:
    void onInnerState(::espview::ITransport::State s);
    void onInnerData(const uint8_t* data, size_t len);

    ::espview::UartTransportConfig uartCfg_;
    ::espview::UartTransport inner_;
    TransportCapabilities caps_;
    DataCallback dataCb_;
    StateCallback stateCb_;
};

// ---- TCP 适配器（旧 TcpTransport → 新 ITransport）----
class Esp32TcpAdapter final : public ::espview::transport::ITransport {
public:
    explicit Esp32TcpAdapter(const ::espview::TcpTransportConfig& cfg);
    ~Esp32TcpAdapter() override;

    bool open() override;
    void close() override;
    bool isConnected() const override;
    SendStatus send(const uint8_t* data, size_t len) override;
    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;
    const TransportCapabilities& capabilities() const override { return caps_; }
    size_t mtu() const override;

    // M6-E §22：只读统计/AP 诊断（透传 inner_；UART adapter 用默认值）。
    uint64_t reconnectCount() const override { return inner_.reconnectCount(); }
    uint64_t txBytes() const override { return inner_.txBytes(); }
    uint64_t rxBytes() const override { return inner_.rxBytes(); }
    bool wifiApInfo(int8_t* rssi, uint8_t* channel) const override {
        return inner_.wifiApInfo(rssi, channel);
    }

private:
    void onInnerState(::espview::ITransport::State s);
    void onInnerData(const uint8_t* data, size_t len);

    ::espview::TcpTransportConfig tcpCfg_;
    ::espview::TcpTransport inner_;
    TransportCapabilities caps_;
    DataCallback dataCb_;
    StateCallback stateCb_;
};

}  // namespace transport
}  // namespace espview
