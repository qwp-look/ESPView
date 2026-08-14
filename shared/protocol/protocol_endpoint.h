// ESPView — ProtocolEndpoint（M1-2 纯协议会话层）
//
// 规范来源：docs/DESIGN.md E 节「连接状态机 / ACK 语义 / 消息表 / 控制消息 Payload Layout」。
//
// 职责（两端共用同一实现，纯 C++17、零平台依赖）：
//   Transport 字节流 → StreamDecoder → Message → 会话状态机/控制消息处理 → Application
//   Application → sendMessage() → MessageEncoder → PacketSink → Transport
//
//   1. HELLO 双方互换 → HANDSHAKE → CONNECTED；
//      握手完成：packet.seq 清零（Encoder 计数器 + Decoder 基线）、帧状态清零；
//   2. CONNECTED 后每 2s 发 PING；对端 5s 无任何有效消息 → 超时断开；
//   3. 收到 PING 自动回 PONG（协议层行为）；PONG 用于 RTT 统计；
//   4. ACK_REQ 控制消息（v0.1：SET_MODE）：发送方 500ms 超时、最多重试 2 次，
//      重试重新编码（按 SequenceCounter 规则消耗新 seq），耗尽后回调 onAckTimeout；
//   5. 收到 SET_MODE（ACK_REQ）→ 回调 onSetModeRequest(mode, ackSeq)，
//      Application 处理完后调用 acknowledge(ackSeq, status, errorCode) 回 ACK；
//   6. FRAME_* 消息转发给内部 FrameAssembler（不持有 framebuffer，提交/丢弃经回调上抛）；
//   7. Decoder 错误（CRC/seq/...）转发 FrameAssembler 作废当前帧，但绝不伪造断开；
//   8. 断线/超时 → DISCONNECTED：清 decoder、frame 状态、pending ACK、会话统计保留。
//
// 边界：不实现 Transport / UART / Qt / Display；不做帧重传（整帧丢弃策略在上层）；
//   PING/PONG 不进入 FrameAssembler；控制消息不得插入 CHUNKED Message 内部
//   （由 StreamDecoder 强制，Endpoint 只转发其错误）。
// 错误路径不使用异常。

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "message.h"
#include "protocol.h"
#include "runtime_stats.h"

namespace espview {
namespace proto {

// ---- 会话状态（DESIGN.md E 节「连接状态机」；与 Transport 自身状态严格分离）----
enum class SessionState : uint8_t {
    kDisconnected = 0,  // Transport 不可用 / 会话未建立
    kConnecting = 1,    // Transport 已就绪，HELLO 已发送，等待对端 HELLO
    kHandshake = 2,     // 已收到对端 HELLO，正在验证（瞬时态，验证通过即 CONNECTED）
    kConnected = 3,     // HELLO 互换完成，控制面可用
};

// ---- Transport 发送结果（共享层不依赖 esp_err_t；适配层负责映射）----
enum class SendStatus : uint8_t {
    kOk = 0,
    kBackpressure = 1,  // Transport 缓冲满（上层整帧丢弃，Transport 不理解帧）
    kError = 2,         // Transport 层错误
    kNotConnected = 3,  // Transport 未打开/会话未建立
};

// ---- sendMessage() 返回值 ----
enum class SendResult : uint8_t {
    kOk = 0,
    kNotConnected,     // 会话未建立（未 CONNECTED），拒绝发送
    kInvalidMessage,   // 编码失败；或 ACK_REQ 出现在多包（CHUNKED）消息上（DESIGN.md：仅单包控制消息可 ACK_REQ）
    kTransportError,   // PacketSink 返回错误
    kBackpressure,     // PacketSink 返回背压（部分包可能已发出；上层整帧丢弃）
};

// ---- 会话层协议错误（诊断回调 onProtocolError）----
enum class SessionError : uint8_t {
    kNone = 0,
    kHelloVersionMismatch,  // 对端 HELLO.protocolVersion != 本端协议版本
    kHelloInvalidLayout,    // HELLO payload 长度/字段非法
    kHelloDuplicate,        // CONNECTED 后再次收到 HELLO（按重新确认处理，不算错误）
    kHandshakeTimeout,      // Connecting/Handshake 超时未完成 HELLO 互换
    kPeerTimeout,           // CONNECTED 后对端 5s 无响应
};

// ---- Endpoint 配置 ----
struct EndpointConfig {
    uint8_t protocol_version = kProtocolVersion;  // HELLO.protocolVersion
    uint8_t device_class = 0;                     // HELLO.deviceClass
    uint16_t width = 320;                         // HELLO.width（默认输出分辨率）
    uint16_t height = 240;                        // HELLO.height
    PixelFormat pixel_format = PixelFormat::kRgb565;
    uint8_t mode_mask = 0b111;  // bit0=WINDOW, bit1=DEVICE, bit2=MIRROR
    std::string device_name = "espview";

    // 心跳（DESIGN.md：PING 每 2s；对端 5s 无响应判超时）
    uint64_t ping_interval_ms = 2000;
    uint64_t peer_timeout_ms = 5000;
    // HANDSHAKE 阶段超时（HELLO 丢失时避免永久卡在 Connecting；取对端超时值）
    uint64_t handshake_timeout_ms = 5000;
    // ACK（DESIGN.md：500ms 超时，最多重试 2 次）
    uint64_t ack_timeout_ms = 500;
    uint32_t ack_max_retries = 2;
};

// ---- 对端 HELLO 摘要（握手后可用）----
struct HelloInfo {
    uint8_t protocol_version = 0;
    uint8_t device_class = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    PixelFormat pixel_format = PixelFormat::kRgb565;
    uint8_t mode_mask = 0;
    std::string device_name;
};

// ---- 会话统计（计数器不回绕，uint64）----
struct SessionStats {
    uint64_t txHello = 0;
    uint64_t rxHello = 0;
    uint64_t txPing = 0;
    uint64_t rxPing = 0;
    uint64_t txPong = 0;
    uint64_t rxPong = 0;
    uint64_t pingTimeouts = 0;      // peer_timeout 触发次数
    uint64_t handshakeTimeouts = 0;
    uint64_t ackSent = 0;           // 本端作为接收方发送的 ACK（acknowledge()）
    uint64_t ackReceived = 0;       // 本端作为发送方收到的 ACK
    uint64_t ackRetries = 0;        // ACK 重试次数
    uint64_t ackFailures = 0;       // 重试耗尽
    uint64_t errors = 0;            // SessionError 触发次数
    uint64_t decoderErrors = 0;     // Decoder 上报的协议错误（CRC/seq/...）
    uint64_t rxMessages = 0;        // 收到的全部合法消息
    uint64_t txMessages = 0;        // 发出的全部消息（含自动 PONG）

    // ---- M4 心跳可观察（spec §五/§六）----
    uint64_t lastPingTimeMs = 0;    // 最近一次 PING 发送时刻（本端时钟）
    uint64_t lastPongTimeMs = 0;    // 最近一次收到 PONG 的时刻（本端时钟）
    uint64_t heartbeatTimeouts = 0; // 对端超时次数（== pingTimeouts）
    RttAggregate rtt;               // PING→PONG RTT：last/avg/min/max/samples
                                    // （nullopt = 无测量；断线/重连后重置）

    // ---- M4 Packet 级统计（StreamDecoder onPacket/onError 分类）----
    uint64_t packetsRx = 0;         // 收到并通过 CRC 校验的 Packet 数（含被 seq 规则丢弃的包）
    uint64_t crcErrors = 0;         // CRC 校验失败次数
    uint64_t seqGaps = 0;           // packet.seq 跳变次数
    uint64_t chunkErrors = 0;       // CHUNKED 组装违规次数（类型不一致/非法穿插/超限）
    uint64_t badMagic = 0;          // MAGIC 不可信次数
    uint64_t badVersion = 0;        // VERSION 不支持次数
    uint64_t badHeader = 0;         // 其余头部非法（badType/badLength 等）
};

class ProtocolEndpoint {
public:
    // 注入时钟（返回单调毫秒；默认 steady_clock；ESP32 建议注入 esp_timer_get_time()/1000）
    using Clock = std::function<uint64_t()>;
    // Transport 发送适配（每包调用一次；返回是否送达）
    using PacketSink = std::function<SendStatus(const uint8_t* data, size_t len)>;
    // 非阻塞控制发送适配（tryTransmit 专用）：单次尝试，禁止阻塞/重试。
    using TrySink = std::function<SendStatus(const uint8_t* data, size_t len)>;

    struct Callbacks {
        std::function<void(SessionState state)> onSessionState;
        std::function<void(SessionError error, std::string_view detail)> onProtocolError;

        // FrameAssembler 透传（帧提交/丢弃/矩形；像素指针仅在回调期间有效）
        std::function<void(const FrameBeginInfo& begin)> onFrameBegin;
        std::function<void(const RectInfo& rect, const uint8_t* pixels, size_t pixelBytes)>
            onFrameRect;
        std::function<void(const CommittedFrame& frame)> onFrameCommit;
        std::function<void(FrameDiscardReason reason)> onFrameDiscard;

        // 控制面
        std::function<void(const HelloInfo& hello)> onHello;  // 收到并验证通过对端 HELLO
        // 收到 ACK_REQ 控制消息（v0.1: SET_MODE）：Application 处理后调用 acknowledge()
        std::function<void(uint8_t type, const std::vector<uint8_t>& payload, uint16_t ackSeq)>
            onAckRequest;
        // 收到 ACK（本端作为发送方）：status=0 OK / 1 ERR
        std::function<void(uint16_t ackSeq, uint8_t status, ErrorCode errorCode)> onAck;
        // ACK 重试耗尽
        std::function<void(uint16_t lastSeq)> onAckTimeout;
        // 收到 ERROR 消息
        std::function<void(ErrorCode errorCode, std::string_view text)> onError;
        // 未被会话层消费的消息（INPUT_*、CAPABILITIES、未知类型等；诊断用）
        std::function<void(const Message& msg)> onOtherMessage;
    };

    ProtocolEndpoint(EndpointConfig cfg, PacketSink sink, Callbacks cb,
                     Clock clock = defaultClock());
    // 可选 control sink：供 tryTransmit() 使用（PONG/ACK 回复、心跳 PING、ACK 重试）。
    //   语义：必须非阻塞（单次尝试，缓冲满立即返回 kBackpressure），
    //   用于 RX 任务/会话 tick 上下文，禁止进入长时间重试/等待 —— 阻塞 RX
    //   任务会延误解码；长流式发送期间锁忙时本就应放弃（M4 大帧可靠性修复）。
    //   未提供时回退到主 sink_（PC 侧 sink 本身单次尝试，无差异）。
    ProtocolEndpoint(EndpointConfig cfg, PacketSink sink, TrySink trySink, Callbacks cb,
                     Clock clock = defaultClock());

    // ---- Transport 事件（由适配层调用）----
    void onTransportConnected();     // Transport 可用：发 HELLO，进入 Connecting
    void onTransportDisconnected();  // Transport 断开：清会话/帧/ACK 状态
    void onTransportData(const uint8_t* data, size_t len);  // 喂字节流给 StreamDecoder
    void onTransportData(const std::vector<uint8_t>& data) {
        onTransportData(data.data(), data.size());
    }

    // ---- Application 发送面 ----
    // 发送逻辑消息（经 MessageEncoder → PacketSink）。
    // ACK_REQ 消息（如 makeSetMode）自动登记 pending ACK；多包消息置 ACK_REQ 被拒绝。
    SendResult sendMessage(const Message& msg);
    // 发送流式消息（M1-3C）：payload 由 source 按需产生，不要求整段驻留内存。
    // 适用于大型数据消息（FRAME_RECT 等）；wire bytes 与 sendMessage 完全一致。
    // 约束：不接受 ACK_REQ（ACK_REQ 仅限单包控制消息，见 DESIGN.md）；不支持
    //   pending-ACK 重试（数据面整帧丢弃策略），错误时返回 kInvalidMessage。
    SendResult sendMessageStreaming(const MessageHeader& header, IMessagePayloadSource& source);
    // 主动重发 HELLO（重连确认等场景）
    SendResult sendHello();
    // 对 onAckRequest 的回应：回 ACK{ackSeq, status, errorCode}（仅响应，不再 ACK）
    SendResult acknowledge(uint16_t ackSeq, uint8_t status, ErrorCode errorCode);
    // 主动发 ERROR 消息（诊断/错误上报）
    SendResult sendError(ErrorCode errorCode, std::string_view text);

    // ---- 心跳/超时驱动（由上层定时调用，如每 100-200ms）----
    void tick();

    // ---- 状态/查询 ----
    SessionState state() const { return state_; }
    bool isConnected() const { return state_ == SessionState::kConnected; }
    const SessionStats& stats() const { return stats_; }
    const FrameStats& frameStats() const { return frames_.stats(); }
    const HelloInfo& peerHello() const { return peerHello_; }
    uint64_t nowMs() const { return clock_(); }

private:
    static Clock defaultClock();

    void setState(SessionState s);
    void handleMessage(const Message& msg);
    void handleHello(const Message& msg);
    void handlePing(const Message& msg);
    void handlePong(const Message& msg);
    void handleAck(const Message& msg);
    void handleError(const Message& msg);
    void handleAckRequest(const Message& msg);
    FrameAssembler::Callbacks makeFrameCallbacks();
    void completeHandshake();
    void failSession(SessionError err, std::string_view detail);

    // 发送面内部实现：整条消息（1..N 包）在 sendMutex_ 下原子送出，保证
    //   并发调用时包不会被交叉（DESIGN.md：控制消息不得插入 CHUNKED 消息内部）。
    // transmit()：阻塞式（锁可用则立即发送；否则等待）；
    // tryTransmit()：尽力而为（锁忙时返回 kBackpressure，不阻塞调用者）。
    //   RX 任务发起的回复（PONG/ACK）必须用 tryTransmit，避免阻塞 RX 线程。
    SendResult transmit(const Message& msg, bool requireConnected, bool isRetry = false);
    SendResult tryTransmit(const Message& msg, bool requireConnected, bool isRetry = false);
    SendResult transmitImpl(const Message& msg, bool requireConnected, bool isRetry);
    SendResult transmitImplWithSink(const Message& msg, bool requireConnected, bool isRetry,
                                    const PacketSink& sink);
    // 流式发送（M1-3C）：transmit 的流式版本，sendMutex_ 原子送出整条流式消息。
    SendResult transmitStreaming(const MessageHeader& header, IMessagePayloadSource& source,
                                 bool requireConnected);
    SendResult transmitStreamingImpl(const MessageHeader& header, IMessagePayloadSource& source,
                                     bool requireConnected);

    EndpointConfig cfg_;
    PacketSink sink_;
    TrySink trySink_;  // 可为空；空时 tryTransmit 回退到 sink_
    Callbacks cb_;
    Clock clock_;

    SessionState state_ = SessionState::kDisconnected;
    SessionStats stats_;
    HelloInfo peerHello_;

    // 帧组装（不持有 framebuffer）
    FrameAssembler frames_;

    // 收发核心（握手上 seq 清零）
    SequenceCounter seq_;
    MessageEncoder encoder_;
    StreamDecoder decoder_;

    // 发送串行化：整条消息原子送出（并发 sendMessage 不得交叉包）。
    std::mutex sendMutex_;

    // 心跳/超时
    uint64_t lastPeerRxMs_ = 0;     // 最近一次收到对端有效消息的时间
    uint64_t lastPingMs_ = 0;       // 最近一次发送 PING 的时间
    uint64_t connectMs_ = 0;        // Transport 连接时间（handshake 超时基准）
    uint64_t lastPingSentAtMs_ = 0; // 最近一次 PING 的发送时刻（RTT 测量）

    uint16_t lastSinglePacketSeq_ = 0;  // 最近一个单包（非 CHUNKED）的 SEQ（ACK_REQ 消息定位用）

    // failSession 重入保护：decoder feed 回调内不得调用 decoder_.reset()
    // （会破坏正在迭代的缓冲），延后到下一次 onTransportData()/tick() 执行。
    bool inDecoderCallback_ = false;
    bool decoderResetPending_ = false;

    // Pending ACK（最多同时一个；v0.1 控制面串行）
    struct PendingAck {
        uint16_t seq = 0;
        Message message;          // 重试时重新编码（生成新 seq）
        uint32_t attempts = 0;    // 已发送次数（含首次）
        uint64_t deadlineMs = 0;
    };
    std::optional<PendingAck> pendingAck_;
};

}  // namespace proto
}  // namespace espview




