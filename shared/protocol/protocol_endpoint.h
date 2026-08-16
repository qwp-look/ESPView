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

#include <atomic>
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
    uint8_t mode_mask = 0b1111;  // bit0=WINDOW, bit1=DEVICE, bit2=MIRROR, bit3=SPLIT (M7-C2)
    std::string device_name = "espview";

    // 心跳（DESIGN.md：PING 每 2s；对端 5s 无响应判超时）
    uint64_t ping_interval_ms = 2000;
    uint64_t peer_timeout_ms = 5000;
    // HANDSHAKE 阶段超时（HELLO 丢失时避免永久卡在 Connecting；取对端超时值）
    uint64_t handshake_timeout_ms = 5000;
    // ACK（DESIGN.md：500ms 超时，最多重试 2 次）
    uint64_t ack_timeout_ms = 500;
    uint32_t ack_max_retries = 2;

    // M8-A2：test-only 同步钩子（默认全空；生产代码不得设置）。
    //   全部调用点位于内部锁外、纯信号（不携带数据、不返回值）；回调内禁止
    //   重入本 endpoint 的任何方法（仅用于确定性并发测试的线程编排）。
    struct TestHooks {
        std::function<void()> onTickStateSnapshot;  // tick() 会话状态快照后（分支前）
        std::function<void()> onTickBeforeFail;     // tick() 调用 failSession 前
        std::function<void()> onDisconnectCleared;  // onTransportDisconnected 清理完成后
        std::function<void()> onFeedEnter;          // onTransportData 取 decoderMutex_ 前
        std::function<void()> onConnectEnter;       // onTransportConnected 取任何锁前
    };
    TestHooks testHooks;
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
    // M7-D1：CAPABILITIES（TYPE 0x02）会话级计数。
    uint64_t rxCapabilities = 0;        // 收到并通过 parseCapabilities 的消息
    uint64_t txCapabilities = 0;        // 发出成功的 CAPABILITIES（sendCapabilities）
    uint64_t capabilitiesDropped = 0;   // 收到但 payload 非法/版本不支持（AD.3 诊断计数）
    // M7-D2：PHYSICAL_PREVIEW（TYPE 0x13）会话级计数。
    uint64_t rxPhysicalPreview = 0;      // 收到并通过 parsePhysicalPreview 的消息
    uint64_t txPhysicalPreview = 0;      // 发出成功的 PHYSICAL_PREVIEW（sendPhysicalPreview）
    uint64_t physicalPreviewDropped = 0; // 收到但 payload 非法（AE.3 诊断计数）
    // M7-D3：Wi-Fi provisioning（TYPE 0x06..0x09）会话级计数。
    uint64_t rxWifiScanResult = 0;       // 收到并通过 parseWifiScanResult 的消息
    uint64_t txWifiScanResult = 0;       // 发出成功的 WIFI_SCAN_RESULT（sendWifiScanResult）
    uint64_t wifiScanResultDropped = 0;  // 收到但 payload 非法（AF.3 诊断计数）
    uint64_t rxWifiStatus = 0;           // 收到并通过 parseWifiStatus 的消息
    uint64_t txWifiStatus = 0;           // 发出成功的 WIFI_STATUS（sendWifiStatus）
    uint64_t wifiStatusDropped = 0;      // 收到但 payload 非法（AF.3 诊断计数）

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

    // M8-A1：RX 收到 ACK_REQ 出现在白名单外类型（FRAME_*/INPUT_*/PING/...）
    // 的次数——忽略 + 计数，不回 ACK、不发任何 wire 错误（DESIGN.md E 节 ACK 语义）。
    uint64_t invalidAckReq = 0;
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
        // 收到并解析成功的 CAPABILITIES（M7-D1；fire-and-forget，无 ACK_REQ）。
        std::function<void(const CapabilitiesInfo& caps)> onCapabilities;
        // 收到并解析成功的 PHYSICAL_PREVIEW（M7-D2 AE.3；fire-and-forget，无 ACK_REQ）。
        // pixels 指向 msg.payload 的 [8..1032)，回调期间有效；pixelBytes=1024。
        std::function<void(const PhysicalPreviewInfo& info, const uint8_t* pixels,
                           size_t pixelBytes)> onPhysicalPreview;
        // 收到并解析成功的 WIFI_SCAN_RESULT（M7-D3 AF.3；fire-and-forget，无 ACK_REQ）。
        std::function<void(const WifiScanResultInfo& result)> onWifiScanResult;
        // 收到并解析成功的 WIFI_STATUS（M7-D3 AF.3；fire-and-forget，无 ACK_REQ）。
        std::function<void(const WifiStatusInfo& status)> onWifiStatus;
        // 未被会话层消费的消息（INPUT_*、未知类型等；诊断用）
        // （CAPABILITIES 由 onCapabilities 消费，M7-D1。）
        std::function<void(const Message& msg)> onOtherMessage;
    };

    // ---- Callbacks 生命周期契约（M8-A2 冻结；违反 = UB）----
    // 1. 引用/指针仅回调调用期间有效，禁止缓存：
    //    onFrameRect/onPhysicalPreview 的像素指针、onError/onProtocolError 的
    //    string_view、onAckRequest 的 payload vector、onOtherMessage 的 Message，
    //    以及所有按引用传入的结构体——回调返回后即失效（内部缓冲可能被复用）。
    // 2. Callbacks 在构造时一次性注入，无运行时替换 API；
    //    空 std::function 表示"不订阅"，不会被调用。
    // 3. 销毁序：必须先 quiesce（停止调用 onTransportData/onTransportConnected/
    //    onTransportDisconnected/tick/send*）并关闭 transport，再销毁 endpoint；
    //    不得在回调仍可能并发执行时销毁。
    // 4. 回调内禁止重入任何取 decoderMutex_/sendMutex_ 的公开 API：
    //    onTransportData / onTransportConnected / onTransportDisconnected / tick /
    //    frameStats（decoderMutex_ 不可重入 → 自死锁）；阻塞式 sendMessage /
    //    sendMessageStreaming / sendError（sendMutex_ 不可重入 → 自死锁）。
    //    非阻塞回复只能走 tryTransmit 系（见第 5 条）。
    // 5. RX 路径回调内发回复只能走 tryTransmit 系（PONG/ACK/HELLO），不得阻塞。
    // 6. 回调不得长时间阻塞（RX/tick 线程会因此停滞，对端 5s 超时判线被饿死）。

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
    // 重入约束：sink 回调内不得再次调用本端点的阻塞式 sendMessage /
    // sendMessageStreaming（sendMutex_ 不可重入 → 自死锁）；重入只能走 tryTransmit
    // （锁忙返回 kBackpressure）。
    SendResult sendMessage(const Message& msg);
    // 发送流式消息（M1-3C）：payload 由 source 按需产生，不要求整段驻留内存。
    // 适用于大型数据消息（FRAME_RECT 等）；wire bytes 与 sendMessage 完全一致。
    // 约束：不接受 ACK_REQ（ACK_REQ 仅限单包控制消息，见 DESIGN.md）；不支持
    //   pending-ACK 重试（数据面整帧丢弃策略），错误时返回 kInvalidMessage。
    // 重入约束：sink/source 回调内不得再次调用本端点的阻塞式 sendMessage /
    //   sendMessageStreaming（sendMutex_ 不可重入 → 自死锁）。
    SendResult sendMessageStreaming(const MessageHeader& header, IMessagePayloadSource& source);
    // 主动重发 HELLO（重连确认等场景）
    SendResult sendHello();
    // 对 onAckRequest 的回应：回 ACK{ackSeq, status, errorCode}（仅响应，不再 ACK）
    SendResult acknowledge(uint16_t ackSeq, uint8_t status, ErrorCode errorCode);
    // 主动发 ERROR 消息（诊断/错误上报）
    SendResult sendError(ErrorCode errorCode, std::string_view text);
    // 主动发 CAPABILITIES（M7-D1 AD.3：fire-and-forget，不带 ACK_REQ）。
    // 违规字段（makeCapabilities 校验失败）→ kInvalidMessage。
    SendResult sendCapabilities(const CapabilitiesInfo& caps);
    // 主动发 PHYSICAL_PREVIEW（M7-D2 AE.3：fire-and-forget，不带 ACK_REQ）。
    // 违规字段（makePhysicalPreview 校验失败）→ kInvalidMessage。
    SendResult sendPhysicalPreview(const PhysicalPreviewInfo& info, const uint8_t* pixels);
    // 主动发 WIFI_SCAN_RESULT（M7-D3 AF.3：fire-and-forget，不带 ACK_REQ）。
    // 违规字段（makeWifiScanResult 校验失败）→ kInvalidMessage。内部走
    // tryTransmit（非阻塞）：锁忙/背压整帧丢弃（状态流无重试价值）。
    SendResult sendWifiScanResult(const WifiScanResultInfo& result);
    // 主动发 WIFI_STATUS（M7-D3 AF.3：fire-and-forget，不带 ACK_REQ）。
    // 违规字段（makeWifiStatus 校验失败）→ kInvalidMessage。同样走 tryTransmit。
    SendResult sendWifiStatus(const WifiStatusInfo& status);

    // ---- 心跳/超时驱动（由上层定时调用，如每 100-200ms）----
    // M8-A1：tick() 同时驱动 StreamDecoder 的半包超时（约 500 ms →
    // StreamDecoder::onTimeout()，见 decoder.h/protocol_endpoint.h 文档）；
    // 并排空单槽 pendingHello_/pendingCapabilities_（RX 非阻塞回复的兜底）。
    void tick();

    // ---- 状态/查询（M8-A1：按值返回，sessionMutex_/decoderMutex_ 短临界区保护）----
    SessionState state() const;
    bool isConnected() const;
    SessionStats stats() const;    // 快照拷贝（源兼容既有 `ep.stats().field` / `const auto s = ep.stats()`）
    FrameStats frameStats() const; // 快照拷贝（decoderMutex_ 保护 FrameAssembler）
    // M8-A1：peerHello_ 跨线程读写（RX 写 / 任意读）——按值返回（sessionMutex_
    // 短临界区快照；与既有调用 `ep.peerHello().width` / `.device_name.c_str()` 兼容）。
    HelloInfo peerHello() const {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        return peerHello_;
    }
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
    void handleCapabilities(const Message& msg);
    void handlePhysicalPreview(const Message& msg);
    void handleWifiScanResult(const Message& msg);
    void handleWifiStatus(const Message& msg);
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
    // M8-A2：重试/延迟发送的会话身份（retrySeq/retryEpoch/checkEpoch）——
    //   发送前在 sessionMutex_ 下校验 epoch 未变，防旧会话控制发进新会话。
    SendResult tryTransmit(const Message& msg, bool requireConnected, bool isRetry = false,
                           uint16_t retrySeq = 0, uint64_t retryEpoch = 0,
                           bool checkEpoch = false);
    // M8-A1：encodeStream 完成后把 firstSeq/haveFirstSeq 带回给调用方，
    //   由 afterSend()（sendMutex_ 释放后）登记 pending ACK —— 保证锁序
    //   decoderMutex_ → sessionMutex_ → sendMutex_（不持 sendMutex_ 取 sessionMutex_）。
    // M8-A2 HIGH-1：失败路径（sink 错误/背压/source 错误等）在发送临界区内按
    //   epoch 复查回退未上送 seq（seqBefore+sentCount），见 rollbackSeq。
    SendResult transmitImplWithSink(const Message& msg, bool requireConnected, bool isRetry,
                                    const PacketSink& sink, uint16_t* firstSeq,
                                    bool* haveFirstSeq, uint16_t retrySeq = 0,
                                    uint64_t retryEpoch = 0, bool checkEpoch = false);
    // 发送成功后的会话登记：txMessages++ / pending ACK 注册（sessionMutex_ 保护）。
    // M8-A2 HIGH-1：失败发送的 seq 回退（调用方必须持有 sendMutex_；仅当
    //   sessionEpoch_==epochBefore 时重置到 seqBefore+sentCount——已上送包的 seq
    //   保留、未上送包的 seq 释放；epoch 已换代则跳过，换代路径已重置 seq）。
    void rollbackSeq(uint64_t epochBefore, uint16_t seqBefore, uint32_t sentCount);
    void afterSend(const Message& msg, bool isRetry, SendResult r, uint16_t firstSeq,
                   bool haveFirstSeq, uint16_t retrySeq = 0, uint64_t retryEpoch = 0);
    // 单槽 pending 回复排空（tick() 调用；tryTransmit 非阻塞，kOk 清槽）。
    void drainPendingHello();
    void drainPendingCapabilities();
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

    // M8-A1 并发模型（DESIGN.md I 节线程模型，M8-A1 更新）：
    //   锁序不变量（真实边，非全序）：
    //     - RX 路径：decoderMutex_ → sessionMutex_（feed 临界区内的消息/错误
    //       回调可取 sessionMutex_，属允许方向）；
    //     - TX 路径：sendMutex_ → sessionMutex_（transmitImplWithSink /
    //       transmitStreamingImpl 内短状态快照；安全——从不持 sessionMutex_ 再取
    //       decoderMutex_，也不在持 sessionMutex_ 时阻塞等 sendMutex_——所有
    //       session→send 路径均用 tryTransmit/try_lock）。
    //   其余规则：
    //     - sessionMutex_：state_/stats_/pendingAck_/lastPingMs_/connectMs_/
    //       lastPingSentAtMs_/lastSinglePacketSeq_/pendingHello_/pendingCapabilities_
    //       /sessionEpoch_/lastPingEpoch_/sessionFailed_ 的短临界区访问
    //       （sessionMutex_ 持锁期间绝不调用用户回调）；
    //     - decoderMutex_：decoder_ 与 frames_ 对象访问（feed/reset/onTimeout/
    //       onMessage/onStreamError/查询）；
    //     - sendMutex_：整条消息编码+发送原子段；decoder 回调内的发送必须用
    //       tryTransmit（try_lock，非阻塞无死锁）；sink 回调不得重入阻塞式
    //       sendMessage/sendMessageStreaming（sendMutex_ 不可重入 → 自死锁）。
    mutable std::mutex sessionMutex_;
    mutable std::mutex decoderMutex_;

    SessionState state_ = SessionState::kDisconnected;
    SessionStats stats_;
    HelloInfo peerHello_;

    // M8-A2：会话纪元（sessionMutex_ 保护；无 wire 字段）。在 4 个 generation
    //   边界 ++：onTransportConnected / onTransportDisconnected / failSession /
    //   handleHello 被动恢复。PendingAck.epoch / lastPingEpoch_ 与之配套，
    //   用于识别跨会话的 stale ACK / PONG / 延迟控制发送。
    uint64_t sessionEpoch_ = 0;
    // failSession 幂等标记：每个会话（connect/被动恢复 起）至多失败并清理一次。
    bool sessionFailed_ = false;

    // 帧组装（不持有 framebuffer）
    FrameAssembler frames_;

    // 收发核心（握手上 seq 清零）
    SequenceCounter seq_;
    MessageEncoder encoder_;
    StreamDecoder decoder_;

    // 发送串行化：整条消息原子送出（并发 sendMessage 不得交叉包）。
    std::mutex sendMutex_;

    // 心跳/超时（M8-A1：lastPeerRxMs_/lastDecoderRxMs_ 原子——RX 写、tick 读）
    std::atomic<uint64_t> lastPeerRxMs_{0};     // 最近一次收到对端有效消息的时间
    std::atomic<uint64_t> lastDecoderRxMs_{0};  // 最近一次向 decoder 喂字节的时间（半包超时时钟）
    uint64_t lastPingMs_ = 0;       // 最近一次发送 PING 的时间（sessionMutex_）
    uint64_t connectMs_ = 0;        // Transport 连接时间（handshake 超时基准；sessionMutex_）
    uint64_t lastPingSentAtMs_ = 0; // 最近一次 PING 的发送时刻（RTT 测量；sessionMutex_）
    uint64_t lastPingEpoch_ = 0;    // lastPingSentAtMs_ 所属会话纪元（sessionMutex_）

    // M8-A2：逐包热路径计数器（独立原子块；RX 回调线程无锁自增，
    //   stats() 快照时合并进 SessionStats 副本——公共字段不变）。
    struct PerPacketCounters {
        std::atomic<uint64_t> packetsRx{0};
        std::atomic<uint64_t> crcErrors{0};
        std::atomic<uint64_t> seqGaps{0};
        std::atomic<uint64_t> chunkErrors{0};
        std::atomic<uint64_t> badMagic{0};
        std::atomic<uint64_t> badVersion{0};
        std::atomic<uint64_t> badHeader{0};
        std::atomic<uint64_t> decoderErrors{0};
    };
    PerPacketCounters perPacket_;

    uint16_t lastSinglePacketSeq_ = 0;  // 最近一个单包（非 CHUNKED）的 SEQ（ACK_REQ 消息定位用；sessionMutex_）

    // failSession 重入保护：decoder feed 回调内不得调用 decoder_.reset()
    // （会破坏正在迭代的缓冲），延后到下一次 onTransportData()/tick() 执行。
    // M8-A1：原子（RX 回调线程写 / tick 线程读）。
    std::atomic<bool> inDecoderCallback_{false};
    std::atomic<bool> decoderResetPending_{false};

    // M8-A1 单槽非阻塞回复（RX 路径背压时的兜底，tick() 排空）：
    //   pendingHello_：被动恢复时 HELLO 发送失败暂存（Connecting/Handshake 排空）；
    //   pendingCapabilities_：sendCapabilities 背压暂存（Connected 排空）。
    // 均受 sessionMutex_ 保护。
    std::optional<Message> pendingHello_;
    std::optional<Message> pendingCapabilities_;

    // Pending ACK（最多同时一个；v0.1 控制面串行）
    struct PendingAck {
        uint16_t seq = 0;
        uint64_t epoch = 0;          // 登记时的 sessionEpoch_（跨会话识别）
        Message message;          // 重试时重新编码（生成新 seq）
        uint32_t attempts = 0;    // 已发送次数（含首次）
        uint64_t deadlineMs = 0;
    };
    std::optional<PendingAck> pendingAck_;
};

}  // namespace proto
}  // namespace espview


