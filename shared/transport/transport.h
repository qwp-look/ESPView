// ESPView M6-C — Transport 抽象层（shared/transport，纯 C++17、零平台依赖）。
//
// 规范来源：docs/DESIGN.md E/F 节 + M6-C 任务书（§三 TransportManager、§七
//   Transport Capability、§八–§十三 Transport-specific pacing）。
//
// 边界（与 ESP32 espview::ITransport / PC IPcTransport 同构）：Transport 是
//   "可靠的字节管道"，不理解 Packet/Message/Frame/CRC/HELLO；HELLO/CONNECTED
//   属于 Protocol 层（protocol_endpoint），Transport 只上报自身状态。
//
// 本层解决 M6-C 的两个核心问题：
//   1. Runtime Transport Selection（TransportManager）：管理当前 ITransport
//      实例，支持 open/close/switchTo；只管理 ITransport，不理解协议。
//   2. Transport-aware TX Pacing（TransportCapabilities + TxPolicy +
//      TransportSink）：UART（paced=true）保留按 wire 速率节流/背压重试；
//      TCP（paced=false）依赖 send() 自身背压（socket send buffer），
//      不做 UART 式 sleep；上层只消费抽象结果，禁止散落 if(uart)/if(tcp)。
//
// 错误路径不使用异常。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace espview {
namespace transport {

// ---- Transport 类型（§三）----
enum class TransportType : uint8_t {
    kUart = 0,
    kTcp = 1,
};

// ---- Transport 能力（§七）：上层只消费抽象结果，禁止 if(uart)/if(tcp) ----
struct TransportCapabilities {
    size_t mtu = 0;                  // 单次 send 允许的最大字节数
    size_t preferredPacketSize = 0;  // 建议发送 chunk（≤ mtu；供上层调度参考）
    bool lowLatency = false;         // TCP：延迟敏感（NODELAY 已开、小包优先）
    bool orderedReliableStream = true;  // 可靠有序字节流（UART/TCP 均满足）
    bool paced = true;               // true=UART：上层必须按 wire 速率节流/背压重试；
                                     // false=TCP：依赖 send() 自身背压（socket 缓冲）
};

// ---- TX 策略（由 capabilities 推导；Transport 报告，上层消费）----
struct TxPolicy {
    uint32_t retryIntervalMs = 5;    // paced 传输的背压重试间隔
    uint32_t maxWaitMs = 30000;      // paced 传输的整体发送预算
    bool retryOnBackpressure = true; // false = 单次尝试（TCP：send() 已内部阻塞到超时）
};

// capabilities → policy 的唯一映射点（两端共用；新增 Transport 只改这里）。
inline TxPolicy txPolicyFor(const TransportCapabilities& caps) {
    TxPolicy p;
    if (!caps.paced) {
        p.retryOnBackpressure = false;
        p.maxWaitMs = 0;
        p.retryIntervalMs = 0;
    }
    return p;
}

// ---- 发送结果（平台无关；与 proto::SendStatus 同序，由适配层显式映射）----
enum class SendStatus : uint8_t {
    kOk = 0,
    kBackpressure = 1,  // Transport 缓冲满（上层整帧丢弃，Transport 不理解帧）
    kError = 2,         // Transport 层错误
    kNotConnected = 3,  // Transport 未打开/会话未建立
};

// ---- Transport 接口（平台无关；ESP32/PC/测试各自实现）----
class ITransport {
public:
    // 状态只描述 Transport/driver 自身可用性，不代表对端已连接。
    enum class State : uint8_t {
        kDisconnected = 0,
        kConnecting = 1,
        kConnected = 2,
        kError = 3,
    };

    // 收到原始字节流。回调仅在调用期间有效；禁止缓存指针。
    using DataCallback = std::function<void(const uint8_t* data, size_t len)>;
    using StateCallback = std::function<void(State state)>;

    virtual ~ITransport() = default;

    // 打开并激活 Transport（工厂已注入配置；M6-C：open() 无参数，
    // 具体 Transport 在构造时持有自己的配置）。
    virtual bool open() = 0;
    // 关闭并释放全部资源；幂等（重复 close 无副作用）。
    virtual void close() = 0;
    // 当前 Transport/driver 是否可用（≠ 对端已连接）。
    virtual bool isConnected() const = 0;
    // 发送 len 字节。kOk = 数据已进入 Transport TX 缓冲。
    // 背压（缓冲满）返回 kBackpressure；上层应整帧丢弃，Transport 不理解帧。
    virtual SendStatus send(const uint8_t* data, size_t len) = 0;
    virtual void setDataCallback(DataCallback cb) = 0;
    virtual void setStateCallback(StateCallback cb) = 0;
    // 能力报告（§七）：上层只消费本结果做 pacing 决策。
    virtual const TransportCapabilities& capabilities() const = 0;
    // 单次 send 允许的最大字节数。
    virtual size_t mtu() const = 0;

    // ---- 只读统计快照（M6-E §22；非 wire 字段，纯诊断）----
    virtual uint64_t reconnectCount() const { return 0; }
    virtual uint64_t txBytes() const { return 0; }
    virtual uint64_t rxBytes() const { return 0; }
    virtual bool wifiApInfo(int8_t* rssiOut, uint8_t* channelOut) const {
        (void)rssiOut; (void)channelOut; return false;
    }
};

}  // namespace transport
}  // namespace espview
