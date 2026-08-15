// ESPView M6-C — 测试专用 FakeTransport（仅 host 测试使用，禁止引入生产组件）。
//
// 可配置：能力、open 成败、send 固定结果/结果序列、MTU。
// 可观测：open/close/send 计数、累计发送字节、状态日志、RX 注入。
// 纯 C++17，零平台依赖。

#pragma once

#include <cstddef>
#include <memory>
#include <cstdint>
#include <vector>

#include "transport.h"

namespace espview {
namespace transport {
namespace test {

class FakeTransport : public ITransport {
public:
    explicit FakeTransport(TransportType type, const TransportCapabilities& caps = {})
        : type_(type), caps_(caps) {
        if (caps_.mtu == 0) {
            caps_.mtu = 4096;
        }
        if (caps_.preferredPacketSize == 0) {
            caps_.preferredPacketSize = caps_.mtu;
        }
        caps_.orderedReliableStream = true;
    }

    // ---- 配置 ----
    void setOpenResult(bool ok) { openResult_ = ok; }
    void setCapabilities(const TransportCapabilities& caps) { caps_ = caps; }
    // 固定返回结果（默认 kOk）。
    void setSendResult(SendStatus r) { fixedSendResult_ = r; sendSeq_.clear(); }
    // M7-B：diagSnapshot 可观测字段（默认值保持既有测试行为）。
    void setRxBytes(uint64_t v) { rxBytes_ = v; }
    void setReconnectCount(uint64_t v) { reconnectCount_ = v; }
    void setApInfo(int8_t rssi, uint8_t ch) { apInfoValid_ = true; rssi_ = rssi; channel_ = ch; }
    void clearApInfo() { apInfoValid_ = false; }
    // 按序返回（队列用完后回落到 fixedSendResult_）。
    void setSendSequence(std::vector<SendStatus> seq) { sendSeq_ = std::move(seq); }

    // ---- 观测 ----
    TransportType type() const { return type_; }
    size_t openCount() const { return openCount_; }
    size_t closeCount() const { return closeCount_; }
    size_t sendCount() const { return sendCount_; }
    uint64_t sentBytes() const { return sentBytes_; }
    const std::vector<uint8_t>& txData() const { return txData_; }
    const std::vector<State>& stateLog() const { return stateLog_; }

    // ---- ITransport ----
    bool open() override {
        ++openCount_;
        if (!openResult_) {
            setState(State::kError);
            return false;
        }
        setState(State::kConnected);
        return true;
    }
    void close() override {
        ++closeCount_;
        setState(State::kDisconnected);
    }
    bool isConnected() const override { return openResult_ && closeCount_ == 0 && openCount_ > 0; }
    SendStatus send(const uint8_t* data, size_t len) override {
        ++sendCount_;
        sentBytes_ += len;
        txData_.insert(txData_.end(), data, data + len);
        if (!sendSeq_.empty()) {
            const SendStatus r = sendSeq_.front();
            sendSeq_.erase(sendSeq_.begin());
            return r;
        }
        return fixedSendResult_;
    }
    void setDataCallback(DataCallback cb) override { dataCb_ = std::move(cb); }
    void setStateCallback(StateCallback cb) override { stateCb_ = std::move(cb); }
    const TransportCapabilities& capabilities() const override { return caps_; }
    size_t mtu() const override { return caps_.mtu; }
    uint64_t reconnectCount() const override { return reconnectCount_; }
    uint64_t txBytes() const override { return sentBytes_; }
    uint64_t rxBytes() const override { return rxBytes_; }
    bool wifiApInfo(int8_t* rssiOut, uint8_t* channelOut) const override {
        if (!apInfoValid_) {
            return false;
        }
        *rssiOut = rssi_;
        *channelOut = channel_;
        return true;
    }

    // ---- 测试辅助 ----
    void setState(State s) {
        stateLog_.push_back(s);
        if (stateCb_ != nullptr) {
            stateCb_(s);
        }
    }
    void deliverRx(const uint8_t* data, size_t len) {
        if (dataCb_ != nullptr) {
            dataCb_(data, len);
        }
    }
    void clearTx() { txData_.clear(); }

private:
    TransportType type_;
    TransportCapabilities caps_;
    bool openResult_ = true;
    SendStatus fixedSendResult_ = SendStatus::kOk;
    std::vector<SendStatus> sendSeq_;

    size_t openCount_ = 0;
    size_t closeCount_ = 0;
    size_t sendCount_ = 0;
    uint64_t sentBytes_ = 0;
    uint64_t rxBytes_ = 0;
    uint64_t reconnectCount_ = 0;
    bool apInfoValid_ = false;
    int8_t rssi_ = -128;
    uint8_t channel_ = 0;
    std::vector<uint8_t> txData_;
    std::vector<State> stateLog_;

    DataCallback dataCb_;
    StateCallback stateCb_;
};

// UART 语义能力（paced）。
inline TransportCapabilities uartCaps(size_t mtu = 8192) {
    TransportCapabilities c;
    c.mtu = mtu;
    c.preferredPacketSize = 4096;
    c.lowLatency = false;
    c.paced = true;
    return c;
}

// TCP 语义能力（unpaced）。
inline TransportCapabilities tcpCaps() {
    TransportCapabilities c;
    c.mtu = 20 + 4096;
    c.preferredPacketSize = 4116;
    c.lowLatency = true;
    c.paced = false;
    return c;
}

}  // namespace test
}  // namespace transport
}  // namespace espview
