// ESPView — ITransport 抽象（ESP32 侧；PC 侧同构接口）。
//
// 规范来源：docs/DESIGN.md E/F 节。
// 边界：Transport 是"可靠的字节管道"，不理解 Packet/Message/Frame；
//   UART/driver 可用 ≠ PC 已连接 ≠ 协议 HANDSHAKE 完成。
//   HELLO/CONNECTED 属于 Protocol 层（protocol_endpoint），Transport 只上报自身状态。
// 依赖 ESP-IDF esp_err.h（ESP32 侧实现）；纯 C++17。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "esp_err.h"

namespace espview {

// Transport 配置基类：具体 Transport 定义子类（如 UartTransportConfig）。
// 注意：ESP-IDF 默认 -fno-rtti，open() 内不得使用 dynamic_cast；
//   调用方必须传入与具体 Transport 匹配的配置子类（接口约定）。
struct TransportConfig {
    virtual ~TransportConfig() = default;
};

class ITransport {
public:
    // 状态只描述 Transport/driver 自身可用性，不代表对端已连接。
    enum class State : uint8_t {
        Disconnected = 0,
        Connecting = 1,
        Connected = 2,  // UART/driver 已就绪（物理层可用）
        Error = 3,
    };

    // 收到原始字节流。回调仅在调用期间有效；禁止缓存指针，跨线程保留需复制。
    using DataCallback = std::function<void(const uint8_t* data, size_t len)>;
    using StateCallback = std::function<void(State state)>;

    virtual ~ITransport() = default;

    // 打开并配置 Transport。失败返回错误码并进入 Error 状态。
    virtual esp_err_t open(const TransportConfig& cfg) = 0;
    // 关闭并释放全部资源；幂等（重复 close 无副作用）。
    virtual void close() = 0;
    // 当前 Transport/driver 是否可用（≠ 对端已连接）。
    virtual bool isConnected() const = 0;
    // 发送 len 字节。ESP_OK = 数据已进入 Transport TX 缓冲。
    // 背压（缓冲满）返回 ESP_ERR_TIMEOUT；上层应整帧丢弃，Transport 不理解帧。
    virtual esp_err_t send(const uint8_t* data, size_t len) = 0;
    virtual void setDataCallback(DataCallback cb) = 0;
    virtual void setStateCallback(StateCallback cb) = 0;
    // 单次 send 允许的最大字节数。
    virtual size_t mtu() const = 0;
};

}  // namespace espview
