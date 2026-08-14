// ESPView M6-A — PC 侧 Transport 抽象（IPcTransport）。
//
// 规范来源：M6-A 任务书 §三十二/§三十三（ConnectionManager 支持 UART + TCP，
//   GUI 不绑定 COM3）+ M1-3B（HostUartTransport 同构接口）。
// 与 ESP32 侧 ITransport（espview/transport.hpp）同构：open / close / send /
//   isConnected / dataCallback / stateCallback / mtu()；PC 侧额外有
//   rxBytes()/txBytes()（诊断统计，§三十四 Transport 子域）。
// 边界：Transport 是"可靠的字节管道"，不理解 Packet/Message/Frame/CRC/HELLO。
//
// 配置基类：具体 Transport 定义子类（HostUartTransport::Config /
//   HostTcpTransport::Config）。调用方必须传入与具体 Transport 匹配的配置。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace espview {
namespace pc {

struct PcTransportConfig {
    virtual ~PcTransportConfig() = default;
};

class IPcTransport {
public:
    // 状态只描述 Transport/driver 自身可用性，不代表对端已连接。
    enum class State : uint8_t {
        Disconnected = 0,
        Connecting = 1,
        Connected = 2,
        Error = 3,
    };

    using DataCallback = std::function<void(const uint8_t* data, size_t len)>;
    using StateCallback = std::function<void(State state)>;

    virtual ~IPcTransport() = default;

    virtual bool open(const PcTransportConfig& cfg) = 0;
    virtual void close() = 0;
    virtual bool isConnected() const = 0;
    // 完整发送 len 字节（Transport 层处理 TCP short write）。false = 未连接/失败/超时。
    virtual bool send(const uint8_t* data, size_t len) = 0;
    virtual void setDataCallback(DataCallback cb) = 0;
    virtual void setStateCallback(StateCallback cb) = 0;
    virtual size_t mtu() const = 0;

    // 诊断统计（§三十四：Transport 子域；计数器不回绕）。
    virtual uint64_t rxBytes() const = 0;
    virtual uint64_t txBytes() const = 0;
};

}  // namespace pc
}  // namespace espview
