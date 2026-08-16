#include "protocol_endpoint.h"

#include <utility>

#include "byte_order.h"

namespace espview {
namespace proto {

namespace {

// 小端读取（与 message.cpp 对称；仅用于解析对端载荷）。
// 保留 vector 边界检查语义，内部走 byte_order.h（M8-A1 内部重构）。
uint16_t readU16(const std::vector<uint8_t>& p, size_t off) {
    if (off + 2 > p.size()) {
        return 0;
    }
    return readU16LE(p.data() + off);
}

uint64_t readU64(const std::vector<uint8_t>& p, size_t off) {
    if (off + 8 > p.size()) {
        return 0;
    }
    return readU64LE(p.data() + off);
}

// M8-A1：StreamDecoder 半包滞留超时（实现层常量，非 wire）。
// 由 ProtocolEndpoint::tick() 驱动（上层每 100-200ms 调 tick；ESP32 sessionLoop、
// PC serial_worker）。超时 → StreamDecoder::onTimeout()：丢弃滞留字节、回 SYNC、
// 作废组装中的 Message、expectedSeq 保持不变。
constexpr uint64_t kDecoderHalfPacketTimeoutMs = 500;

}  // namespace

ProtocolEndpoint::Clock ProtocolEndpoint::defaultClock() {
    return [] {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    };
}

FrameAssembler::Callbacks ProtocolEndpoint::makeFrameCallbacks() {
    FrameAssembler::Callbacks fcb;
    fcb.onBegin = [this](const FrameBeginInfo& b) {
        if (cb_.onFrameBegin) {
            cb_.onFrameBegin(b);
        }
    };
    fcb.onRect = [this](const RectInfo& r, const uint8_t* p, size_t n) {
        if (cb_.onFrameRect) {
            cb_.onFrameRect(r, p, n);
        }
    };
    fcb.onCommit = [this](const CommittedFrame& f) {
        if (cb_.onFrameCommit) {
            cb_.onFrameCommit(f);
        }
    };
    fcb.onDiscard = [this](FrameDiscardReason r) {
        if (cb_.onFrameDiscard) {
            cb_.onFrameDiscard(r);
        }
    };
    return fcb;
}

ProtocolEndpoint::ProtocolEndpoint(EndpointConfig cfg, PacketSink sink, Callbacks cb, Clock clock)
    : ProtocolEndpoint(std::move(cfg), sink, TrySink{}, std::move(cb), std::move(clock)) {}

ProtocolEndpoint::ProtocolEndpoint(EndpointConfig cfg, PacketSink sink, TrySink trySink,
                                   Callbacks cb, Clock clock)
    : cfg_(std::move(cfg)),
      sink_(std::move(sink)),
      trySink_(std::move(trySink)),
      cb_(std::move(cb)),
      clock_(clock ? std::move(clock) : defaultClock()),
      frames_(makeFrameCallbacks()),
      encoder_(seq_),
      decoder_(
          [this](const Message& m) { handleMessage(m); },
          [this](const PacketHeader& h, const uint8_t*, size_t) {
              // RX 回调线程：统计经 sessionMutex_；帧状态由调用方持有的
              // decoderMutex_ 保护（onTransportData feed 临界区）。
              {
                  std::lock_guard<std::mutex> lock(sessionMutex_);
                  ++stats_.packetsRx;
                  // 单包消息的 SEQ（ACK_REQ 只允许出现在单包控制消息上）。
                  if (!(h.flags & kFlagChunked)) {
                      lastSinglePacketSeq_ = h.seq;
                  }
              }
              // 任何通过 CRC 的包都说明对端活着（含被 seq 规则丢弃的包）。
              lastPeerRxMs_ = clock_();
          },
          [this](DecoderError e) {
              {
                  std::lock_guard<std::mutex> lock(sessionMutex_);
                  ++stats_.decoderErrors;
                  // M4：Packet 级错误分类（累计计数，不刷屏）。
                  switch (e) {
                      case DecoderError::kCrcMismatch:
                          ++stats_.crcErrors;
                          break;
                      case DecoderError::kSequenceGap:
                          ++stats_.seqGaps;
                          break;
                      case DecoderError::kBadMagic:
                          ++stats_.badMagic;
                          break;
                      case DecoderError::kBadVersion:
                          ++stats_.badVersion;
                          break;
                      case DecoderError::kChunkTypeMismatch:
                      case DecoderError::kChunkViolation:
                      case DecoderError::kMessageTooLarge:
                          ++stats_.chunkErrors;
                          break;
                      case DecoderError::kBadType:
                      case DecoderError::kBadLength:
                          ++stats_.badHeader;
                          break;
                      default:
                          break;  // kNeedMoreData 不上报
                  }
              }
              frames_.onStreamError(e);  // 正在收帧 → 帧作废；不伪造断开
              // （frames_ 访问处于 decoderMutex_ 临界区内，见 onTransportData）
          }) {}

void ProtocolEndpoint::setState(SessionState s) {
    // 更新 state_ 在锁内，回调在锁外（不得持锁调用用户回调）。
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (state_ == s) {
            return;
        }
        state_ = s;
    }
    if (cb_.onSessionState) {
        cb_.onSessionState(s);
    }
}

SessionState ProtocolEndpoint::state() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return state_;
}

bool ProtocolEndpoint::isConnected() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return state_ == SessionState::kConnected;
}

SessionStats ProtocolEndpoint::stats() const {
    std::lock_guard<std::mutex> lock(sessionMutex_);
    return stats_;
}

FrameStats ProtocolEndpoint::frameStats() const {
    std::lock_guard<std::mutex> lock(decoderMutex_);
    return frames_.stats();
}void ProtocolEndpoint::onTransportConnected() {
    // 会话层全新开始：seq/解码器/帧/ACK 全部清零（DESIGN.md：握手/重连后 seq 清零）。
    // 锁序：先 decoderMutex_（decoder/frames 清理），后 sessionMutex_（会话字段）。
    {
        std::lock_guard<std::mutex> dlock(decoderMutex_);
        decoder_.reset();
        frames_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        pendingAck_.reset();
        pendingHello_.reset();
        pendingCapabilities_.reset();
        // 主动发起的 HELLO 必须从 seq=0 开始：对端 failSession/断线后 decoder reset，
        // expectedSeq=0，首个包 seq != 0 会被当作 seq 跳变丢弃 → 被动恢复永远无法完成
        // （M3 reconnect 暴露：断线前已发出 PING/输入包时，重连 HELLO 携带旧 seq）。
        seq_.reset(0);
        lastPingMs_ = 0;
        lastPingSentAtMs_ = 0;
        // M4：新会话开始 → RTT 无测量（nullopt）+ 聚合清零；心跳时间戳清零。
        stats_.rtt.reset();
        stats_.lastPingTimeMs = 0;
        stats_.lastPongTimeMs = 0;
        connectMs_ = clock_();
    }
    lastPeerRxMs_ = connectMs_;  // 原子
    lastDecoderRxMs_ = connectMs_;  // 原子：握手初始时钟，防首个 tick 误触发半包超时
    setState(SessionState::kConnecting);
    sendHello();
}

void ProtocolEndpoint::onTransportDisconnected() {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        pendingAck_.reset();
        pendingHello_.reset();
        pendingCapabilities_.reset();
        lastPingMs_ = 0;
        lastPingSentAtMs_ = 0;
        // M4：会话结束 → RTT 回到「无测量」状态（不保留跨会话的旧值）。
        stats_.rtt.reset();
        stats_.lastPingTimeMs = 0;
        stats_.lastPongTimeMs = 0;
        seq_.reset(0);  // 会话结束：下一会话从 seq=0 重新开始（DESIGN.md 握手/重连 seq 清零）
    }
    {
        std::lock_guard<std::mutex> dlock(decoderMutex_);
        decoder_.reset();
        frames_.reset();
    }
    setState(SessionState::kDisconnected);
}

void ProtocolEndpoint::onTransportData(const uint8_t* data, size_t len) {
    // M8-A1：每次喂入（len>0）都刷新半包超时时钟（半包等待从最近一次喂入算起）。
    if (len > 0) {
        lastDecoderRxMs_ = clock_();
    }
    // decoder_/frames_ 对象访问统一在 decoderMutex_ 临界区内（含消息/错误回调
    // 内部对 frames_ 的访问）。不因会话状态丢弃字节：DESIGN.md「DISCONNECTED →
    // ESP32 等待对端 HELLO」，断线/超时后收到的 HELLO 必须能进入解码路径。
    std::lock_guard<std::mutex> dlock(decoderMutex_);
    if (decoderResetPending_) {
        // 上次 failSession 发生在 decoder 回调内，reset 被延后到这里执行。
        decoder_.reset();
        frames_.reset();
        decoderResetPending_ = false;
        lastDecoderRxMs_ = clock_();  // 复位后重新起算，防首个 tick 误触发
    }
    inDecoderCallback_ = true;
    decoder_.feed(data, len);
    inDecoderCallback_ = false;
}

SendResult ProtocolEndpoint::transmit(const Message& msg, bool requireConnected, bool isRetry) {
    // 整条消息原子送出：并发调用（如 ESP32 上 App 帧发送任务与心跳任务）
    // 不得在包粒度交叉，否则多包消息会被对端判定为 CHUNKED 违规（DESIGN.md）。
    uint16_t firstSeq = 0;
    bool haveFirstSeq = false;
    SendResult r;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        r = transmitImplWithSink(msg, requireConnected, isRetry, sink_, &firstSeq,
                                 &haveFirstSeq);
    }
    // 会话登记（txMessages/pending ACK）在 sendMutex_ 释放后进行：保持锁序
    // decoderMutex_ → sessionMutex_ → sendMutex_。
    afterSend(msg, isRetry, r, firstSeq, haveFirstSeq);
    return r;
}

SendResult ProtocolEndpoint::tryTransmit(const Message& msg, bool requireConnected, bool isRetry) {
    // 尽力而为：锁忙（例如对端正在发长 CHUNKED 消息）时不阻塞调用者。
    // 用途：RX 任务发起的 PONG/ACK 回复、会话 tick 的 PING/ACK 重试——
    //   阻塞 RX 任务会延误解码，且回复本身是 best-effort（PING/PONG 独立
    //   机制；ACK 由对端 500ms 重试兜底）。
    // M4：非阻塞 sink（trySink_）保证即使锁可用，发送本身也不会进入长重试
    //   循环（大帧流式发送期间 TX 缓冲满时立即放弃，而不是把 RX 任务/会话
    //   tick 卡在 transportSink 内）。
    if (!sendMutex_.try_lock()) {
        return SendResult::kBackpressure;
    }
    uint16_t firstSeq = 0;
    bool haveFirstSeq = false;
    SendResult r;
    {
        // lock_guard 仅覆盖发送段：afterSend 在 sendMutex_ 释放后登记
        // （会话统计 + pending ACK），不得持 sendMutex_ 取 sessionMutex_。
        std::lock_guard<std::mutex> lock(sendMutex_, std::adopt_lock);
        r = transmitImplWithSink(msg, requireConnected, isRetry, trySink_ ? trySink_ : sink_,
                                 &firstSeq, &haveFirstSeq);
    }
    afterSend(msg, isRetry, r, firstSeq, haveFirstSeq);  // sendMutex_ 已释放
    return r;
}

SendResult ProtocolEndpoint::transmitImpl(const Message& msg, bool requireConnected, bool isRetry) {
    uint16_t firstSeq = 0;
    bool haveFirstSeq = false;
    return transmitImplWithSink(msg, requireConnected, isRetry, sink_, &firstSeq,
                                &haveFirstSeq);
}

SendResult ProtocolEndpoint::transmitImplWithSink(const Message& msg, bool requireConnected,
                                                  bool isRetry, const PacketSink& sink,
                                                  uint16_t* firstSeq, bool* haveFirstSeq) {
    (void)isRetry;
    SessionState st;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        st = state_;
    }
    if (st == SessionState::kDisconnected) {
        return SendResult::kNotConnected;
    }
    if (requireConnected && st != SessionState::kConnected) {
        return SendResult::kNotConnected;
    }
    if (!sink) {
        return SendResult::kTransportError;
    }

    // M8-A1 ACK_REQ 白名单（发送面防御）：白名单外类型 + ACK_REQ → kInvalidMessage。
    if ((msg.flags & kFlagAckReq) != 0 && !allowedAckRequestType(msg.type)) {
        return SendResult::kInvalidMessage;
    }
    // DESIGN.md：ACK_REQ 只允许单包控制消息（多包 = payload > kMaxPacketPayload）。
    if ((msg.flags & kFlagAckReq) != 0 && msg.payload.size() > kMaxPacketPayload) {
        return SendResult::kInvalidMessage;
    }

    // 流式编码逐包发送：长消息（如 153 KB FRAME_RECT）不一次性物化全部 Packet，
    // 峰值内存 = payload + 单包缓冲（经典 ESP32 内存约束；M8-A1 单包缓冲复用）。
    SendStatus sinkResult = SendStatus::kOk;
    bool aborted = false;
    const PacketError encodeErr = encoder_.encodeStream(
        msg, [&](const uint8_t* data, size_t len) {
            if ((msg.flags & kFlagAckReq) != 0 && !*haveFirstSeq) {
                PacketHeader h;
                if (decodeHeader(data, len, &h) == PacketError::kNone) {
                    *firstSeq = h.seq;
                    *haveFirstSeq = true;
                }
            }
            const SendStatus st2 = sink(data, len);
            if (st2 != SendStatus::kOk) {
                aborted = true;
                sinkResult = st2;
                return false;
            }
            return true;
        });
    if (aborted) {
        // 背压/错误：部分包可能已发出；上层按整帧丢弃策略处理。
        return sinkResult == SendStatus::kBackpressure ? SendResult::kBackpressure
                                                       : SendResult::kTransportError;
    }
    if (encodeErr != PacketError::kNone) {
        return SendResult::kInvalidMessage;
    }
    return SendResult::kOk;
}

void ProtocolEndpoint::afterSend(const Message& msg, bool isRetry, SendResult r,
                                 uint16_t firstSeq, bool haveFirstSeq) {
    if (r != SendResult::kOk) {
        return;
    }
    std::lock_guard<std::mutex> lock(sessionMutex_);
    ++stats_.txMessages;

    // 登记 pending ACK（重试时重新编码 → 按 SequenceCounter 规则生成新 seq）。
    if ((msg.flags & kFlagAckReq) != 0 && haveFirstSeq) {
        if (isRetry && pendingAck_) {
            // 重试：更新为最新发送包的 seq，deadline 顺延；attempts 由 tick() 递增
            pendingAck_->seq = firstSeq;
            pendingAck_->deadlineMs = clock_() + cfg_.ack_timeout_ms;
        } else if (!isRetry) {
            PendingAck pa;
            pa.seq = firstSeq;
            pa.message = msg;
            pa.attempts = 1;
            pa.deadlineMs = clock_() + cfg_.ack_timeout_ms;
            pendingAck_ = std::move(pa);
        }
    }
}

SendResult ProtocolEndpoint::sendMessage(const Message& msg) {
    return transmit(msg, /*requireConnected=*/true);
}

SendResult ProtocolEndpoint::sendMessageStreaming(const MessageHeader& header,
                                                  IMessagePayloadSource& source) {
    return transmitStreaming(header, source, /*requireConnected=*/true);
}

SendResult ProtocolEndpoint::transmitStreaming(const MessageHeader& header,
                                               IMessagePayloadSource& source,
                                               bool requireConnected) {
    // 与 transmit() 同语义：整条流式消息原子送出（sendMutex_ 串行化）。
    SendResult r;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        r = transmitStreamingImpl(header, source, requireConnected);
    }
    // 统计在 sendMutex_ 释放后登记（锁序）。
    if (r == SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.txMessages;
    }
    return r;
}

SendResult ProtocolEndpoint::transmitStreamingImpl(const MessageHeader& header,
                                                   IMessagePayloadSource& source,
                                                   bool requireConnected) {
    SessionState st;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        st = state_;
    }
    if (st == SessionState::kDisconnected) {
        return SendResult::kNotConnected;
    }
    if (requireConnected && st != SessionState::kConnected) {
        return SendResult::kNotConnected;
    }
    if (!sink_) {
        return SendResult::kTransportError;
    }
    // DESIGN.md：ACK_REQ 只允许单包控制消息；流式路径用于大型数据消息，
    // 一律拒绝 ACK_REQ（且不支持 pending-ACK 重试）。
    if ((header.flags & kFlagAckReq) != 0) {
        return SendResult::kInvalidMessage;
    }

    SendStatus sinkResult = SendStatus::kOk;
    bool aborted = false;
    const PacketError encodeErr =
        encoder_.encodeStreaming(header, source, [&](const uint8_t* data, size_t len) {
            const SendStatus st2 = sink_(data, len);
            if (st2 != SendStatus::kOk) {
                aborted = true;
                sinkResult = st2;
                return false;
            }
            return true;
        });
    if (aborted) {
        return sinkResult == SendStatus::kBackpressure ? SendResult::kBackpressure
                                                       : SendResult::kTransportError;
    }
    if (encodeErr != PacketError::kNone) {
        return SendResult::kInvalidMessage;
    }
    return SendResult::kOk;
}

SendResult ProtocolEndpoint::sendHello() {
    const auto hello = makeHello(cfg_.protocol_version, cfg_.device_class, cfg_.width,
                                 cfg_.height, cfg_.pixel_format, cfg_.mode_mask, cfg_.device_name);
    if (!hello.has_value()) {
        return SendResult::kInvalidMessage;
    }
    const SendResult r = transmit(*hello, /*requireConnected=*/false);
    if (r == SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.txHello;
    }
    return r;
}

SendResult ProtocolEndpoint::acknowledge(uint16_t ackSeq, uint8_t status, ErrorCode errorCode) {
    // RX 任务上下文调用：尽力而为（发送互斥被长消息占用时放弃，由对端重试兜底）。
    const SendResult r =
        tryTransmit(makeAck(ackSeq, status, errorCode), /*requireConnected=*/false,
                    /*isRetry=*/false);
    if (r == SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.ackSent;
    }
    return r;
}

SendResult ProtocolEndpoint::sendError(ErrorCode errorCode, std::string_view text) {
    const auto err = makeError(errorCode, text);
    if (!err.has_value()) {
        return SendResult::kInvalidMessage;
    }
    return transmit(*err, /*requireConnected=*/false);
}

SendResult ProtocolEndpoint::sendPhysicalPreview(const PhysicalPreviewInfo& info,
                                                  const uint8_t* pixels) {
    const auto msg = makePhysicalPreview(info.frameId, info.width, info.height,
                                         info.pixelFormat, info.flags, pixels);
    if (!msg.has_value()) {
        return SendResult::kInvalidMessage;
    }
    // AE.3：无 ACK_REQ（fire-and-forget）；仅 CONNECTED 可发送。
    // 用 tryTransmit（非阻塞 + 尽力 sink）：preview 是丢得起的数据面，锁忙/背压
    // 时整帧丢弃（AE.3「背压整帧丢弃」），绝不让会话任务阻塞在长流式帧后。
    const SendResult r = tryTransmit(*msg, /*requireConnected=*/true,
                                     /*isRetry=*/false);
    if (r == SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.txPhysicalPreview;
    }
    return r;
}

SendResult ProtocolEndpoint::sendWifiScanResult(const WifiScanResultInfo& result) {
    const auto msg = makeWifiScanResult(result.scanSeq, (result.flags & kWifiScanResultFlagTruncated) != 0,
                                        result.total, result.records.data(), result.records.size());
    if (!msg.has_value()) {
        return SendResult::kInvalidMessage;
    }
    // AF.3：无 ACK_REQ（fire-and-forget）；仅 CONNECTED 可发送。与 PHYSICAL_PREVIEW
    // 同策略：tryTransmit（非阻塞 + 尽力 sink），锁忙/背压整帧丢弃，绝不让会话任务
    // 阻塞在长流式帧后（状态流无重试价值）。
    const SendResult r = tryTransmit(*msg, /*requireConnected=*/true, /*isRetry=*/false);
    if (r == SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.txWifiScanResult;
    }
    return r;
}

SendResult ProtocolEndpoint::sendWifiStatus(const WifiStatusInfo& status) {
    const auto msg = makeWifiStatus(status.phase, status.errorCode, status.flags, status.rssi,
                                    status.channel, status.ip, status.serverIp,
                                    status.serverPort, status.ssid);
    if (!msg.has_value()) {
        return SendResult::kInvalidMessage;
    }
    // AF.3：fire-and-forget；仅 CONNECTED 可发送；tryTransmit 非阻塞（同上）。
    const SendResult r = tryTransmit(*msg, /*requireConnected=*/true, /*isRetry=*/false);
    if (r == SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.txWifiStatus;
    }
    return r;
}

SendResult ProtocolEndpoint::sendCapabilities(const CapabilitiesInfo& caps) {
    const auto msg = makeCapabilities(
        caps.virtualPresent, caps.physicalPresent, caps.width, caps.height,
        caps.pixelFormat, caps.colorDepth, caps.virtualMono, caps.virtualCanReadback,
        caps.modeMask, caps.physWidth, caps.physHeight, caps.physPixelFormat,
        caps.physColorDepth, caps.physMono, caps.physCanReadback, caps.physController,
        caps.physI2cAddress, caps.sceneSupport);
    if (!msg.has_value()) {
        return SendResult::kInvalidMessage;
    }
    // AD.3：不带 ACK_REQ（fire-and-forget）；仅在 CONNECTED 后可发。
    // M8-A1：改为 tryTransmit（非阻塞）——RX/会话任务不得阻塞在长流式帧后；
    // 失败（背压/锁忙/暂未连接）时登记单槽 pendingCapabilities_，由 tick()
    // 在 CONNECTED 后排空（单槽、不重复）。可能返回 kBackpressure。
    const SendResult r = tryTransmit(*msg, /*requireConnected=*/true, /*isRetry=*/false);
    std::lock_guard<std::mutex> lock(sessionMutex_);
    if (r == SendResult::kOk) {
        ++stats_.txCapabilities;
    } else {
        pendingCapabilities_ = *msg;
    }
    return r;
}

void ProtocolEndpoint::tick() {
    const uint64_t now = clock_();

    // ---- 1) decoder/frames 清理 + StreamDecoder 半包超时（decoderMutex_）----
    // M8-A1：半包超时在所有状态分支之前判定（握手期间也生效），由 tick() 驱动；
    // 只作废滞留字节/组装中 Message（onTimeout 保留 expectedSeq），不清会话。
    {
        std::lock_guard<std::mutex> dlock(decoderMutex_);
        if (decoderResetPending_) {
            // 延后的 decoder/frame 清理（failSession 在回调内时排队到这里执行）。
            decoder_.reset();
            frames_.reset();
            decoderResetPending_ = false;
            lastDecoderRxMs_ = now;  // 复位后重新起算，防本 tick 误触发
        }
        if ((decoder_.bufferedBytes() > 0 || decoder_.assemblingMessage()) &&
            now - lastDecoderRxMs_.load(std::memory_order_relaxed) >=
                kDecoderHalfPacketTimeoutMs) {
            decoder_.onTimeout();
            // FrameAssembler::onStreamError 忽略错误码、仅 kInFrame 时作废当前帧
            // （frame_assembler.cpp:90-96）——其余状态是安全 no-op。
            frames_.onStreamError(DecoderError::kMessageTooLarge);
        }
    }

    // ---- 2) 会话状态快照 ----
    SessionState st;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        st = state_;
    }

    if (st == SessionState::kConnecting || st == SessionState::kHandshake) {
        // HANDSHAKE 超时（HELLO 丢失时避免永久卡在 Connecting）。
        bool timeout = false;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            if (now - connectMs_ >= cfg_.handshake_timeout_ms) {
                ++stats_.handshakeTimeouts;
                timeout = true;
            }
        }
        if (timeout) {
            failSession(SessionError::kHandshakeTimeout, "HELLO handshake timeout");
            return;
        }
        // M8-A1：排空被动恢复时未发出的 HELLO（单槽）。
        drainPendingHello();
        return;
    }

    if (st != SessionState::kConnected) {
        return;
    }

    // ---- 3) 对端超时（DESIGN.md：5s 无响应判断线）----
    if (now - lastPeerRxMs_.load(std::memory_order_relaxed) >= cfg_.peer_timeout_ms) {
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            ++stats_.pingTimeouts;
            ++stats_.heartbeatTimeouts;  // M4 别名：GUI 显示 heartbeat timeouts
        }
        failSession(SessionError::kPeerTimeout, "peer timeout: no response for 5s");
        return;
    }

    // ---- 4) 心跳：每 2s 一个 PING ----
    // M1-3C 修正：必须用 tryTransmit（锁忙即放弃），不能 sendMessage——
    // 长流式消息（153608B FRAME_RECT）会持有 sendMutex_ 十余秒，阻塞式
    // 心跳会让本 tick() 卡死，peer 超时检测被饿死 → 断线永远无法被发现
    // （M1-3B reconnect 回归暴露）。锁忙/失败时保持 lastPingMs_ 不前进，
    // 下一 tick 立即重试；PING 是 best-effort，跳过不影响会话状态机。
    {
        bool pingDue = false;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            if (now - lastPingMs_ >= cfg_.ping_interval_ms) {
                pingDue = true;
            }
        }
        if (pingDue) {
            const SendResult r =
                tryTransmit(makePing(now), /*requireConnected=*/true, /*isRetry=*/false);
            if (r == SendResult::kOk) {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                lastPingMs_ = now;
                lastPingSentAtMs_ = now;
                ++stats_.txPing;
                stats_.lastPingTimeMs = now;
            }
        }
    }

    // ---- 5) ACK 重试（DESIGN.md：500ms 超时，最多重试 2 次）----
    {
        bool ackDue = false;
        Message ackMsg;
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            if (pendingAck_ && now >= pendingAck_->deadlineMs) {
                ackDue = true;
                ackMsg = pendingAck_->message;
            }
        }
        if (ackDue) {
            bool gaveUp = false;
            uint16_t lastSeq = 0;
            {
                std::lock_guard<std::mutex> lock(sessionMutex_);
                const uint32_t maxSends = 1 + cfg_.ack_max_retries;
                if (pendingAck_ && pendingAck_->attempts >= maxSends) {
                    lastSeq = pendingAck_->seq;
                    pendingAck_.reset();
                    ++stats_.ackFailures;
                    gaveUp = true;
                }
            }
            if (gaveUp) {
                if (cb_.onAckTimeout) {
                    cb_.onAckTimeout(lastSeq);
                }
            } else {
                // 与心跳同理：长流式发送占用 sendMutex_ 时不得阻塞本 tick()，
                // 锁忙则放弃本次重试（对端 500ms 重试机制兜底，见 tryTransmit 注释）。
                const SendResult r = tryTransmit(ackMsg, /*requireConnected=*/true,
                                                 /*isRetry=*/true);
                if (r == SendResult::kOk) {
                    std::lock_guard<std::mutex> lock(sessionMutex_);
                    // handleAck 可能已清空 pendingAck_（对端恰好回了 ACK）：
                    // 此时不再递增 attempts。
                    if (pendingAck_) {
                        ++pendingAck_->attempts;
                        ++stats_.ackRetries;
                    }
                }
            }
        }
    }

    // ---- 6) M8-A1：排空 sendCapabilities 背压暂存（Connected；单槽）----
    drainPendingCapabilities();
}

void ProtocolEndpoint::drainPendingHello() {
    bool have = false;
    Message msg;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (pendingHello_) {
            have = true;
            msg = *pendingHello_;
        }
    }
    if (!have) {
        return;
    }
    const SendResult r = tryTransmit(msg, /*requireConnected=*/false, /*isRetry=*/false);
    if (r == SendResult::kOk) {
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            pendingHello_.reset();
            ++stats_.txHello;
        }
        // 被动恢复时 HELLO 回复此刻才真正发出 → 完成握手（与 handleHello 正常
        // 路径语义一致）。completeHandshake 会动 decoder_/frames_，而本路径不在
        // decoder feed 内，必须显式取 decoderMutex_（handleHello 路径由 feed 持有，
        // 不得重复加锁）。仅 tick() 的 Connecting/Handshake 分支调用本函数。
        {
            std::lock_guard<std::mutex> dlock(decoderMutex_);
            completeHandshake();
        }
    }
}

void ProtocolEndpoint::drainPendingCapabilities() {
    bool have = false;
    Message msg;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        if (pendingCapabilities_) {
            have = true;
            msg = *pendingCapabilities_;
        }
    }
    if (!have) {
        return;
    }
    const SendResult r = tryTransmit(msg, /*requireConnected=*/true, /*isRetry=*/false);
    if (r == SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        pendingCapabilities_.reset();
        ++stats_.txCapabilities;
    }
}

void ProtocolEndpoint::handleMessage(const Message& msg) {
    SessionState st;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        st = state_;
        if (st == SessionState::kDisconnected &&
            msg.type != static_cast<uint8_t>(MessageType::kHello)) {
            return;  // 会话已死；仅 HELLO 可触发被动恢复
        }
        ++stats_.rxMessages;
    }
    lastPeerRxMs_ = clock_();

    // M8-A1 ACK_REQ 白名单（RX / decoder 侧）：白名单外类型携带 ACK_REQ →
    // 忽略消息 + 计数（不 invoke handleAckRequest、不回 ACK、不发任何 wire 错误）。
    // StreamDecoder 保持 wire-transparent（不解释 ACK_REQ）；本检查是 Endpoint 层语义。
    if ((msg.flags & kFlagAckReq) != 0 && !allowedAckRequestType(msg.type)) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.invalidAckReq;
        return;
    }

    switch (static_cast<MessageType>(msg.type)) {
        case MessageType::kHello:
            handleHello(msg);
            return;
        case MessageType::kPing:
            handlePing(msg);
            return;
        case MessageType::kPong:
            handlePong(msg);
            return;
        case MessageType::kAck:
            handleAck(msg);
            return;
        case MessageType::kError:
            handleError(msg);
            return;
        case MessageType::kCapabilities:
            handleCapabilities(msg);
            return;
        case MessageType::kPhysicalPreview:
            handlePhysicalPreview(msg);
            return;
        case MessageType::kWifiScanResult:
            handleWifiScanResult(msg);
            return;
        case MessageType::kWifiStatus:
            handleWifiStatus(msg);
            return;
        case MessageType::kFrameBegin:
        case MessageType::kFrameRect:
        case MessageType::kFrameEnd:
            frames_.onMessage(msg);  // decoderMutex_ 临界区内（feed 调用链）
            return;
        default:
            break;
    }

    // 其他 ACK_REQ 控制消息（白名单内：SET_MODE / WIFI_SCAN_REQ / WIFI_CONFIG）。
    if ((msg.flags & kFlagAckReq) != 0) {
        handleAckRequest(msg);
        return;
    }
    if (cb_.onOtherMessage) {
        cb_.onOtherMessage(msg);
    }
}

void ProtocolEndpoint::handleHello(const Message& msg) {
    // HELLO layout：ver(1) class(1) w(2) h(2) fmt(1) mask(1) nameLen(1) name[n]
    const auto& p = msg.payload;
    if (p.size() < 9) {
        failSession(SessionError::kHelloInvalidLayout, "HELLO payload too short");
        return;
    }
    const size_t nameLen = p[8];
    if (p.size() != 9 + nameLen) {
        failSession(SessionError::kHelloInvalidLayout, "HELLO name length mismatch");
        return;
    }

    const uint8_t version = p[0];
    if (version != cfg_.protocol_version) {
        failSession(SessionError::kHelloVersionMismatch,
                    "HELLO protocol version " + std::to_string(version) + " != " +
                        std::to_string(static_cast<unsigned>(cfg_.protocol_version)));
        return;
    }

    SessionState st;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.rxHello;
        st = state_;
    }
    // M8-A1：peerHello_ 跨 RX/tick 线程读写——先构建局部副本，再一次性赋值
    // （sessionMutex_）；回调分发用局部副本（不持锁调用用户回调）。
    HelloInfo local;
    local.protocol_version = version;
    local.device_class = p[1];
    local.width = readU16(p, 2);
    local.height = readU16(p, 4);
    local.pixel_format = static_cast<PixelFormat>(p[6]);
    local.mode_mask = p[7];
    local.device_name.assign(p.begin() + 9, p.end());
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        peerHello_ = local;
    }

    // M8-A1：被动恢复时 HELLO 回复是否已实际发出。
    //   回复成功（r==kOk）→ 立即完成握手（与 M1-2 原语义一致：对端 HELLO 即确认）；
    //   回复失败（背压/锁忙）→ 保持 kConnecting，pendingHello_ 由 tick()
    //   drainPendingHello() 排空后完成握手——否则对端永远收不到本端 HELLO，
    //   双方握手永不完成。
    bool helloSent = true;
    if (st == SessionState::kDisconnected) {
        // DESIGN.md：DISCONNECTED 后 ESP32 等待对端 HELLO；收到即被动恢复会话。
        // 重新发送本端 HELLO 并进入握手（seq/帧/ACK 状态重新清零）。
        // M8-A1：RX 任务内不得阻塞——改用 tryTransmit 发 HELLO；失败登记单槽
        // pendingHello_，由 tick() 在 Connecting/Handshake 排空。
        seq_.reset(0);
        decoder_.resetSeqBaseline();
        frames_.reset();
        {
            std::lock_guard<std::mutex> lock(sessionMutex_);
            pendingAck_.reset();
            pendingHello_.reset();
            pendingCapabilities_.reset();
            connectMs_ = clock_();
        }
        lastPeerRxMs_ = clock_();  // 原子
        setState(SessionState::kConnecting);
        const auto hello = makeHello(cfg_.protocol_version, cfg_.device_class, cfg_.width,
                                     cfg_.height, cfg_.pixel_format, cfg_.mode_mask,
                                     cfg_.device_name);
        if (hello.has_value()) {
            const SendResult r = tryTransmit(*hello, /*requireConnected=*/false,
                                             /*isRetry=*/false);
            std::lock_guard<std::mutex> lock(sessionMutex_);
            if (r == SendResult::kOk) {
                ++stats_.txHello;
            } else {
                pendingHello_ = *hello;
                helloSent = false;
            }
        } else {
            helloSent = false;  // makeHello 配置违规：无 HELLO 可发，握手只能靠对端重试
        }
    }

    if (cb_.onHello) {
        cb_.onHello(local);
    }

    // 与 M1-2 原语义一致：被动恢复（Disconnected → Connecting + 回发 HELLO）后
    // 也立即完成握手（对端 HELLO 即确认）；因此用实时 state() 而非块前快照。
    // M8-A1 例外：被动 HELLO 回复未发出（helloSent==false）时保持 kConnecting，
    // 由 tick() 排空 pendingHello_ 后 completeHandshake()。
    const SessionState live = state();
    if (helloSent && (live == SessionState::kConnecting || live == SessionState::kHandshake)) {
        completeHandshake();
    }
    // CONNECTED 后重复 HELLO：按"重新确认"处理，不重置会话（对端重启时会再触发重连流程）。
}

void ProtocolEndpoint::handlePing(const Message& msg) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.rxPing;
    }
    const uint64_t ts = readU64(msg.payload, 0);
    // RX 任务内发送回复：尽力而为，避免在长消息发送期间阻塞 RX 线程。
    if (tryTransmit(makePong(ts), /*requireConnected=*/true, /*isRetry=*/false) ==
        SendResult::kOk) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.txPong;
    }
}

void ProtocolEndpoint::handlePong(const Message& msg) {
    const uint64_t ts = readU64(msg.payload, 0);
    (void)ts;
    const uint64_t now = clock_();
    bool recordRtt = false;
    uint32_t rttMs = 0;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.rxPong;
        stats_.lastPongTimeMs = now;
        if (lastPingSentAtMs_ != 0) {
            if (now > lastPingSentAtMs_) {
                // M4：RTT 聚合（last/avg/min/max/samples）；nullopt 语义由
                // RttAggregate 维护——断线/重连后 reset()，不再用 0 表示无测量。
                rttMs = static_cast<uint32_t>(now - lastPingSentAtMs_);
                recordRtt = true;
            }
            lastPingSentAtMs_ = 0;
        }
    }
    if (recordRtt) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        stats_.rtt.record(rttMs);
    }
}

void ProtocolEndpoint::handleAck(const Message& msg) {
    const auto& p = msg.payload;
    if (p.size() < 5) {
        return;  // 布局非法：忽略（不伪造断开）
    }
    const uint16_t ackSeq = readU16(p, 0);
    const uint8_t status = p[2];
    const ErrorCode err = static_cast<ErrorCode>(readU16(p, 3));
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.ackReceived;
        if (pendingAck_ && pendingAck_->seq == ackSeq) {
            pendingAck_.reset();
        }
    }
    if (cb_.onAck) {
        cb_.onAck(ackSeq, status, err);
    }
}

void ProtocolEndpoint::handleError(const Message& msg) {
    const auto& p = msg.payload;
    if (p.size() < 3) {
        return;
    }
    const ErrorCode code = static_cast<ErrorCode>(readU16(p, 0));
    const size_t msgLen = p[2];
    std::string_view text;
    if (p.size() >= 3 + msgLen) {
        text = std::string_view(reinterpret_cast<const char*>(p.data() + 3), msgLen);
    }
    if (cb_.onError) {
        cb_.onError(code, text);
    }
}

void ProtocolEndpoint::handleCapabilities(const Message& msg) {
    // AD.3：短于 32B / version 不支持 → 丢弃并计数（不 failSession，旧 PC 兼容）；
    // 长于 32B 由 parseCapabilities 忽略尾部。
    if (msg.payload.size() < kCapabilitiesPayloadSize) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.capabilitiesDropped;
        return;
    }
    CapabilitiesInfo info;
    if (!parseCapabilities(BytesView(msg.payload.data(), msg.payload.size()), info)) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.capabilitiesDropped;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.rxCapabilities;
    }
    if (cb_.onCapabilities) {
        cb_.onCapabilities(info);
    }
}

void ProtocolEndpoint::handlePhysicalPreview(const Message& msg) {
    // AE.3：短包/非法 payload 仅计数丢弃，不 failSession（数据面，与 FRAME_* 一致）。
    if (msg.payload.size() < kPhysicalPreviewPayloadSize) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.physicalPreviewDropped;
        return;
    }
    PhysicalPreviewInfo info;
    if (!parsePhysicalPreview(BytesView(msg.payload.data(), msg.payload.size()), info)) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.physicalPreviewDropped;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.rxPhysicalPreview;
    }
    if (cb_.onPhysicalPreview) {
        cb_.onPhysicalPreview(info, msg.payload.data() + kPhysicalPreviewPixelOffset,
                              kPhysicalPreviewPixelBytes);
    }
}

void ProtocolEndpoint::handleWifiScanResult(const Message& msg) {
    // AF.3：短包/非法 payload 仅计数丢弃，不 failSession（状态流，与 PHYSICAL_PREVIEW 一致）。
    WifiScanResultInfo info;
    if (!parseWifiScanResult(BytesView(msg.payload.data(), msg.payload.size()), info)) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.wifiScanResultDropped;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.rxWifiScanResult;
    }
    if (cb_.onWifiScanResult) {
        cb_.onWifiScanResult(info);
    }
}

void ProtocolEndpoint::handleWifiStatus(const Message& msg) {
    // AF.3：短包/非法 payload 仅计数丢弃，不 failSession。WIFI_STATUS 绝无密码字段。
    WifiStatusInfo info;
    if (!parseWifiStatus(BytesView(msg.payload.data(), msg.payload.size()), info)) {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.wifiStatusDropped;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.rxWifiStatus;
    }
    if (cb_.onWifiStatus) {
        cb_.onWifiStatus(info);
    }
}

void ProtocolEndpoint::handleAckRequest(const Message& msg) {
    uint16_t ackSeq = 0;
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ackSeq = lastSinglePacketSeq_;
    }
    if (cb_.onAckRequest) {
        cb_.onAckRequest(msg.type, msg.payload, ackSeq);
    }
}

void ProtocolEndpoint::completeHandshake() {
    // 调用方必须持有 decoderMutex_（handleHello 路径由 feed 持有；drainPendingHello
    // 路径显式加锁）。
    // DESIGN.md：握手完成 → 双方 packet.seq 清零、帧状态清零。
    // 注意：此处正处于 decoder 消息回调内（调用方持有 decoderMutex_），不能调用
    // decoder_.reset()（会清空同批 feed 的后续字节且违反"回调内不得重入 reset"
    // 约定）；仅把 seq 基线重定位为 0，缓冲继续消费。
    seq_.reset(0);
    decoder_.resetSeqBaseline();
    frames_.reset();
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        pendingAck_.reset();
        lastPingMs_ = clock_();
        lastPingSentAtMs_ = 0;
    }
    setState(SessionState::kConnected);
}

void ProtocolEndpoint::failSession(SessionError err, std::string_view detail) {
    {
        std::lock_guard<std::mutex> lock(sessionMutex_);
        ++stats_.errors;
        pendingAck_.reset();
        pendingHello_.reset();
        pendingCapabilities_.reset();
        lastPingMs_ = 0;
        lastPingSentAtMs_ = 0;
    }
    if (cb_.onProtocolError) {
        cb_.onProtocolError(err, std::string(detail));
    }
    if (inDecoderCallback_) {
        // 正处于 decoder feed 回调内（decoderMutex_ 已被本线程持有）：不得重入
        // reset()（会破坏正在迭代的缓冲），延后到下一次 onTransportData()/tick()
        // 执行（那里的 decoderMutex_ 临界区内清理）。
        decoderResetPending_ = true;
    } else {
        // tick 等非回调上下文：直接取 decoderMutex_ 清理（此刻未持任何锁）。
        std::lock_guard<std::mutex> dlock(decoderMutex_);
        decoder_.reset();
        frames_.reset();
    }
    setState(SessionState::kDisconnected);
}

}  // namespace proto
}  // namespace espview
