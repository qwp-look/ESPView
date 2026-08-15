#include "protocol_endpoint.h"

#include <utility>

namespace espview {
namespace proto {

namespace {

// 小端读取（与 message.cpp 对称；仅用于解析对端载荷）。
uint16_t readU16(const std::vector<uint8_t>& p, size_t off) {
    if (off + 2 > p.size()) {
        return 0;
    }
    return static_cast<uint16_t>(p[off]) | static_cast<uint16_t>(static_cast<uint16_t>(p[off + 1]) << 8);
}

uint64_t readU64(const std::vector<uint8_t>& p, size_t off) {
    uint64_t v = 0;
    if (off + 8 > p.size()) {
        return 0;
    }
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[off + i]) << (8 * i);
    }
    return v;
}

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
              ++stats_.packetsRx;
              // 单包消息的 SEQ（ACK_REQ 只允许出现在单包控制消息上）。
              if (!(h.flags & kFlagChunked)) {
                  lastSinglePacketSeq_ = h.seq;
              }
              // 任何通过 CRC 的包都说明对端活着（含被 seq 规则丢弃的包）。
              lastPeerRxMs_ = clock_();
          },
          [this](DecoderError e) {
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
              frames_.onStreamError(e);  // 正在收帧 → 帧作废；不伪造断开
          }) {}

void ProtocolEndpoint::setState(SessionState s) {
    if (state_ == s) {
        return;
    }
    state_ = s;
    if (cb_.onSessionState) {
        cb_.onSessionState(s);
    }
}

void ProtocolEndpoint::onTransportConnected() {
    // 会话层全新开始：seq/解码器/帧/ACK 全部清零（DESIGN.md：握手/重连后 seq 清零）。
    decoder_.reset();
    frames_.reset();
    pendingAck_.reset();
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
    lastPeerRxMs_ = connectMs_;
    setState(SessionState::kConnecting);
    sendHello();
}


void ProtocolEndpoint::onTransportDisconnected() {
    pendingAck_.reset();
    lastPingMs_ = 0;
    lastPingSentAtMs_ = 0;
    // M4：会话结束 → RTT 回到「无测量」状态（不保留跨会话的旧值）。
    stats_.rtt.reset();
    stats_.lastPingTimeMs = 0;
    stats_.lastPongTimeMs = 0;
    seq_.reset(0);  // 会话结束：下一会话从 seq=0 重新开始（DESIGN.md 握手/重连 seq 清零）
    decoder_.reset();
    frames_.reset();
    setState(SessionState::kDisconnected);
}


void ProtocolEndpoint::onTransportData(const uint8_t* data, size_t len) {
    // 不因会话状态丢弃字节：DESIGN.md「DISCONNECTED → ESP32 等待对端 HELLO」，
    // 断线/超时后收到的 HELLO 必须能进入解码路径以被动恢复会话。
    if (decoderResetPending_) {
        // 上次 failSession 发生在 decoder 回调内，reset 被延后到这里执行。
        decoder_.reset();
        frames_.reset();
        decoderResetPending_ = false;
    }
    inDecoderCallback_ = true;
    decoder_.feed(data, len);
    inDecoderCallback_ = false;
}

SendResult ProtocolEndpoint::transmit(const Message& msg, bool requireConnected, bool isRetry) {
    // 整条消息原子送出：并发调用（如 ESP32 上 App 帧发送任务与心跳任务）
    // 不得在包粒度交叉，否则多包消息会被对端判定为 CHUNKED 违规（DESIGN.md）。
    std::lock_guard<std::mutex> lock(sendMutex_);
    return transmitImpl(msg, requireConnected, isRetry);
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
    std::lock_guard<std::mutex> lock(sendMutex_, std::adopt_lock);
    return transmitImplWithSink(msg, requireConnected, isRetry,
                                trySink_ ? trySink_ : sink_);
}

SendResult ProtocolEndpoint::transmitImpl(const Message& msg, bool requireConnected, bool isRetry) {
    return transmitImplWithSink(msg, requireConnected, isRetry, sink_);
}

SendResult ProtocolEndpoint::transmitImplWithSink(const Message& msg, bool requireConnected,
                                                  bool isRetry, const PacketSink& sink) {
    if (state_ == SessionState::kDisconnected) {
        return SendResult::kNotConnected;
    }
    if (requireConnected && state_ != SessionState::kConnected) {
        return SendResult::kNotConnected;
    }
    if (!sink) {
        return SendResult::kTransportError;
    }

    // DESIGN.md：ACK_REQ 只允许单包控制消息（多包 = payload > kMaxPacketPayload）。
    if ((msg.flags & kFlagAckReq) != 0 && msg.payload.size() > kMaxPacketPayload) {
        return SendResult::kInvalidMessage;
    }

    // 流式编码逐包发送：长消息（如 153 KB FRAME_RECT）不一次性物化全部 Packet，
    // 峰值内存 = payload + 单包缓冲（经典 ESP32 内存约束）。
    uint16_t firstSeq = 0;
    bool haveFirstSeq = false;
    SendStatus sinkResult = SendStatus::kOk;
    bool aborted = false;
    const PacketError encodeErr = encoder_.encodeStream(
        msg, [&](const uint8_t* data, size_t len) {
            if ((msg.flags & kFlagAckReq) != 0 && !haveFirstSeq) {
                PacketHeader h;
                if (decodeHeader(data, len, &h) == PacketError::kNone) {
                    firstSeq = h.seq;
                    haveFirstSeq = true;
                }
            }
            const SendStatus st = sink(data, len);
            if (st != SendStatus::kOk) {
                aborted = true;
                sinkResult = st;
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
    return SendResult::kOk;
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
    std::lock_guard<std::mutex> lock(sendMutex_);
    return transmitStreamingImpl(header, source, requireConnected);
}

SendResult ProtocolEndpoint::transmitStreamingImpl(const MessageHeader& header,
                                                   IMessagePayloadSource& source,
                                                   bool requireConnected) {
    if (state_ == SessionState::kDisconnected) {
        return SendResult::kNotConnected;
    }
    if (requireConnected && state_ != SessionState::kConnected) {
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
            const SendStatus st = sink_(data, len);
            if (st != SendStatus::kOk) {
                aborted = true;
                sinkResult = st;
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
    ++stats_.txMessages;
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
        ++stats_.txPhysicalPreview;
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
    const SendResult r = transmit(*msg, /*requireConnected=*/true);
    if (r == SendResult::kOk) {
        ++stats_.txCapabilities;
    }
    return r;
}

void ProtocolEndpoint::tick() {
    const uint64_t now = clock_();

    if (decoderResetPending_) {
        // 延后的 decoder/frame 清理（failSession 在回调内时排队到这里执行）。
        decoder_.reset();
        frames_.reset();
        decoderResetPending_ = false;
    }

    if (state_ == SessionState::kConnecting || state_ == SessionState::kHandshake) {
        if (now - connectMs_ >= cfg_.handshake_timeout_ms) {
            ++stats_.handshakeTimeouts;
            failSession(SessionError::kHandshakeTimeout, "HELLO handshake timeout");
        }
        return;
    }

    if (state_ != SessionState::kConnected) {
        return;
    }

    // 对端超时（DESIGN.md：5s 无响应判断线）。
    if (now - lastPeerRxMs_ >= cfg_.peer_timeout_ms) {
        ++stats_.pingTimeouts;
        ++stats_.heartbeatTimeouts;  // M4 别名：GUI 显示 heartbeat timeouts
        failSession(SessionError::kPeerTimeout, "peer timeout: no response for 5s");
        return;
    }

    // 心跳：每 2s 一个 PING。
    // M1-3C 修正：必须用 tryTransmit（锁忙即放弃），不能 sendMessage——
    // 长流式消息（153608B FRAME_RECT）会持有 sendMutex_ 十余秒，阻塞式
    // 心跳会让本 tick() 卡死，peer 超时检测被饿死 → 断线永远无法被发现
    // （M1-3B reconnect 回归暴露）。锁忙/失败时保持 lastPingMs_ 不前进，
    // 下一 tick 立即重试；PING 是 best-effort，跳过不影响会话状态机。
    if (now - lastPingMs_ >= cfg_.ping_interval_ms) {
        const SendResult r =
            tryTransmit(makePing(now), /*requireConnected=*/true, /*isRetry=*/false);
        if (r == SendResult::kOk) {
            lastPingMs_ = now;
            lastPingSentAtMs_ = now;
            ++stats_.txPing;
            stats_.lastPingTimeMs = now;
        }
    }

    // ACK 重试（DESIGN.md：500ms 超时，最多重试 2 次）。
    if (pendingAck_) {
        if (now >= pendingAck_->deadlineMs) {
            const uint32_t maxSends = 1 + cfg_.ack_max_retries;
            if (pendingAck_->attempts < maxSends) {
                // 与心跳同理：长流式发送占用 sendMutex_ 时不得阻塞本 tick()，
                // 锁忙则放弃本次重试（对端 500ms 重试机制兜底，见 tryTransmit 注释）。
                const SendResult r = tryTransmit(pendingAck_->message, /*requireConnected=*/true,
                                                 /*isRetry=*/true);
                if (r == SendResult::kOk) {
                    ++pendingAck_->attempts;
                    ++stats_.ackRetries;
                }
            } else {
                const uint16_t lastSeq = pendingAck_->seq;
                pendingAck_.reset();
                ++stats_.ackFailures;
                if (cb_.onAckTimeout) {
                    cb_.onAckTimeout(lastSeq);
                }
            }
        }
    }
}

void ProtocolEndpoint::handleMessage(const Message& msg) {
    if (state_ == SessionState::kDisconnected &&
        msg.type != static_cast<uint8_t>(MessageType::kHello)) {
        return;  // 会话已死；仅 HELLO 可触发被动恢复
    }
    ++stats_.rxMessages;
    lastPeerRxMs_ = clock_();

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
        case MessageType::kFrameBegin:
        case MessageType::kFrameRect:
        case MessageType::kFrameEnd:
            frames_.onMessage(msg);
            return;
        default:
            break;
    }

    // 其他 ACK_REQ 控制消息（v0.1: SET_MODE；未来 SET_RESOLUTION/SET_PIXEL_FORMAT/RESET）。
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

    ++stats_.rxHello;
    peerHello_.protocol_version = version;
    peerHello_.device_class = p[1];
    peerHello_.width = readU16(p, 2);
    peerHello_.height = readU16(p, 4);
    peerHello_.pixel_format = static_cast<PixelFormat>(p[6]);
    peerHello_.mode_mask = p[7];
    peerHello_.device_name.assign(p.begin() + 9, p.end());

    if (state_ == SessionState::kDisconnected) {
        // DESIGN.md：DISCONNECTED 后 ESP32 等待对端 HELLO；收到即被动恢复会话。
        // 重新发送本端 HELLO 并进入握手（seq/帧/ACK 状态重新清零）。
        seq_.reset(0);
        decoder_.resetSeqBaseline();
        frames_.reset();
        pendingAck_.reset();
        connectMs_ = clock_();
        lastPeerRxMs_ = clock_();
        setState(SessionState::kConnecting);
        sendHello();
    }

    if (cb_.onHello) {
        cb_.onHello(peerHello_);
    }

    if (state_ == SessionState::kConnecting || state_ == SessionState::kHandshake) {
        completeHandshake();
    }
    // CONNECTED 后重复 HELLO：按"重新确认"处理，不重置会话（对端重启时会再触发重连流程）。
}

void ProtocolEndpoint::handlePing(const Message& msg) {
    ++stats_.rxPing;
    const uint64_t ts = readU64(msg.payload, 0);
    // RX 任务内发送回复：尽力而为，避免在长消息发送期间阻塞 RX 线程。
    if (tryTransmit(makePong(ts), /*requireConnected=*/true, /*isRetry=*/false) ==
        SendResult::kOk) {
        ++stats_.txPong;
    }
}

void ProtocolEndpoint::handlePong(const Message& msg) {
    ++stats_.rxPong;
    const uint64_t ts = readU64(msg.payload, 0);
    (void)ts;
    stats_.lastPongTimeMs = clock_();
    if (lastPingSentAtMs_ != 0) {
        const uint64_t now = clock_();
        if (now > lastPingSentAtMs_) {
            // M4：RTT 聚合（last/avg/min/max/samples）；nullopt 语义由
            // RttAggregate 维护——断线/重连后 reset()，不再用 0 表示无测量。
            stats_.rtt.record(static_cast<uint32_t>(now - lastPingSentAtMs_));
        }
        lastPingSentAtMs_ = 0;
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
    ++stats_.ackReceived;
    if (pendingAck_ && pendingAck_->seq == ackSeq) {
        pendingAck_.reset();
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
        ++stats_.capabilitiesDropped;
        return;
    }
    CapabilitiesInfo info;
    if (!parseCapabilities(BytesView(msg.payload.data(), msg.payload.size()), info)) {
        ++stats_.capabilitiesDropped;
        return;
    }
    ++stats_.rxCapabilities;
    if (cb_.onCapabilities) {
        cb_.onCapabilities(info);
    }
}

void ProtocolEndpoint::handlePhysicalPreview(const Message& msg) {
    // AE.3：短包/非法 payload 仅计数丢弃，不 failSession（数据面，与 FRAME_* 一致）。
    if (msg.payload.size() < kPhysicalPreviewPayloadSize) {
        ++stats_.physicalPreviewDropped;
        return;
    }
    PhysicalPreviewInfo info;
    if (!parsePhysicalPreview(BytesView(msg.payload.data(), msg.payload.size()), info)) {
        ++stats_.physicalPreviewDropped;
        return;
    }
    ++stats_.rxPhysicalPreview;
    if (cb_.onPhysicalPreview) {
        cb_.onPhysicalPreview(info, msg.payload.data() + kPhysicalPreviewPixelOffset,
                              kPhysicalPreviewPixelBytes);
    }
}

void ProtocolEndpoint::handleAckRequest(const Message& msg) {
    if (cb_.onAckRequest) {
        cb_.onAckRequest(msg.type, msg.payload, lastSinglePacketSeq_);
    }
}

void ProtocolEndpoint::completeHandshake() {
    // DESIGN.md：握手完成 → 双方 packet.seq 清零、帧状态清零。
    // 注意：此处正处于 decoder 消息回调内，不能调用 decoder_.reset()
    // （会清空同批 feed 的后续字节且违反"回调内不得重入 reset"约定）；
    // 仅把 seq 基线重定位为 0，缓冲继续消费。
    seq_.reset(0);
    decoder_.resetSeqBaseline();
    frames_.reset();
    pendingAck_.reset();
    lastPingMs_ = clock_();
    lastPingSentAtMs_ = 0;
    setState(SessionState::kConnected);
}

void ProtocolEndpoint::failSession(SessionError err, std::string_view detail) {
    ++stats_.errors;
    if (cb_.onProtocolError) {
        cb_.onProtocolError(err, std::string(detail));
    }
    pendingAck_.reset();
    lastPingMs_ = 0;
    lastPingSentAtMs_ = 0;
    if (inDecoderCallback_) {
        // 正处于 decoder feed 回调内：不得重入 reset()（会破坏正在迭代的缓冲），
        // 延后到下一次 onTransportData()/tick() 再执行。
        decoderResetPending_ = true;
    } else {
        decoder_.reset();
        frames_.reset();
    }
    setState(SessionState::kDisconnected);
}

}  // namespace proto
}  // namespace espview


