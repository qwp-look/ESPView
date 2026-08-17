// ESPView M6-C / M8-A3 — Transport 抽象层（shared/transport，纯 C++17、零平台依赖）。
//
// 规范来源：docs/DESIGN.md E/F/V 节 + M6-C 任务书（§三 TransportManager、§七
//   Transport Capability、§八–§十三 Transport-specific pacing）+ M8-A3（§三十五
//   Transport Abstraction Semantics 冻结）。
//
// 边界：Transport 是"可靠的字节管道"，不理解 Packet/Message/Frame/CRC/HELLO；
//   HELLO/CONNECTED 属于 Protocol 层（protocol_endpoint），Transport 只上报自身状态。
//   本层是唯一 canonical transport 契约：ESP32 UART/TCP、PC UART/TCP、未来
//   UsbTransport（kUsb，预留）全部直接实现本接口，不再有第二套 ITransport/State/
//   send/close/callback/MTU/lifecycle/backpressure（§三十五.1）。
//
// 依赖链：Application → Display/Input → ProtocolEndpoint → Transport Abstraction
//   （本文件）→ Backend。protocol_endpoint 通过 using 引用本层的 SendStatus（§三十五.2）。
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
    kUsb = 2,  // 预留：S3 USB CDC（TinyUSB FIFO 背压 → paced=true，CDC 语义）
                // 本阶段无实现、无 factory 分支（仅架构预留，§三十五.13）。
};

// ---- Transport 能力（§七）：上层只消费抽象结果，禁止 if(uart)/if(tcp) ----
struct TransportCapabilities {
    size_t mtu = 0;      // 单次 send 允许的最大字节数（hard bound：len > mtu → kError，
                         // backend 不尝试发送）。informational 上限：encoder 固定按
                         // MAX_PACKET_PAYLOAD(4096) 分包，不读 mtu。
    bool paced = false;  // true=UART/CDC：上层必须按 wire 速率节流/背压重试；
                         // false=TCP：依赖 send() 自身背压（socket send buffer）。
                         // 默认 false（TCP 语义）；UART backend 显式置 true（M8-A3 修正）。
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

// ---- 发送结果（唯一 canonical；protocol_endpoint 经 using 引用本类型，§三十五.2）----
enum class SendStatus : uint8_t {
    kOk = 0,           // 全部 len 字节已完整进入 Transport TX 缓冲（完整写入语义，
                       // backend 内部处理 short write）。
    kBackpressure = 1, // would-block：Transport 缓冲满/发送超时/门忙（trySend 专用）。
                       // 与 kNotConnected 严格区分：会话仍存活，仅本次发送未完成。
                       // paced 传输由 TransportSink 按 TxPolicy 重试；unpaced 由
                       // 上层整帧丢弃（Transport 不理解帧）。
    kError = 2,        // Transport 层错误（driver 失败、参数非法、len > mtu）。
    kNotConnected = 3, // Transport 未打开/未建立/已断开：发送被拒绝，不是背压。
};

// ---- Transport 接口（平台无关；唯一 canonical；ESP32/PC/测试全部直接实现）----
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

    // ---- 生命周期契约（M8-A3 冻结，§三十五）----
    // open()：工厂已注入配置；open 成功 = Transport 已激活（状态可能仍为
    //   kConnecting）。重复 open（未 close）返回 false（明确失败，不做隐式重开）；
    //   close() 后可再次 open（reopen 语义）。
    // close()：同步；返回后保证不再触发任何 data/state 回调、不再使用任何
    //   driver/socket 资源；幂等（重复 close 无副作用）。
    // send()：kOk = 全部 len 字节已进入 TX 缓冲（完整写入，backend 处理 short write）；
    //   len > capabilities().mtu → kError（不尝试发送）；kBackpressure = would-block，
    //   重试策略由上层按 TxPolicy 决定（TransportSink）。
    // 回调线程模型：data/state 回调在 Transport 内部线程同步调用，回调内禁止
    //   重入本对象的 open/close/send/setXxxCallback（backend 串行化保护下可能死锁）；
    //   close() 返回后无任何回调。回调指针/引用仅在调用期间有效。
    // 线程模型：open/close 由管理任务调用（TransportManager）；send 可被任意线程
    //   并发调用（backend 内部串行化）；setXxxCallback 在 open 前设置、close 后不得使用。
    virtual bool open() = 0;
    virtual void close() = 0;
    // 当前 Transport/driver 是否可用（≠ 对端已连接）。
    virtual bool isConnected() const = 0;
    virtual SendStatus send(const uint8_t* data, size_t len) = 0;
    virtual void setDataCallback(DataCallback cb) = 0;
    virtual void setStateCallback(StateCallback cb) = 0;
    // 能力报告（§七）：上层只消费本结果做 pacing 决策。mtu 经
    //   capabilities().mtu 读取（M8-A3：删除 mtu() 虚函数，消除双来源）。
    virtual const TransportCapabilities& capabilities() const = 0;

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
