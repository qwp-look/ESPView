// ESPView M2 — SerialWorker 实现（见 serial_worker.h 的线程模型说明）。

#include "serial_worker.h"

#include <chrono>
#include <cstdio>
#include <utility>

#include <protocol_endpoint.h>

#include "input_codec.h"  // InputEvent → INPUT_* Message
#include "message.h"      // M7-C3：makeSetMode（SET_MODE payload [0] mode，自动 ACK_REQ）

namespace espview {
namespace pc {

namespace {

uint64_t steadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

SerialWorker::SerialWorker(QObject* parent) : QObject(parent) {}

SerialWorker::~SerialWorker() {
    stop();
}

void SerialWorker::start(const QString& port, quint32 baud, bool noReset) {
    stop();  // 幂等：先清理残留
    kind_ = TransportKind::kUart;
    port_ = port;
    baud_ = baud;
    noReset_ = noReset;
    stop_.store(false);
    {
        std::lock_guard<std::mutex> lk(tstateMutex_);
        transportState_ = IPcTransport::State::Disconnected;
    }
    thread_ = std::thread([this]() { runLoop(); });
}

void SerialWorker::startTcp(uint16_t port, const QString& bind) {
    stop();  // 幂等：先清理残留
    kind_ = TransportKind::kTcp;
    tcpPort_ = port;
    tcpBind_ = bind;
    stop_.store(false);
    {
        std::lock_guard<std::mutex> lk(tstateMutex_);
        transportState_ = IPcTransport::State::Disconnected;
    }
    thread_ = std::thread([this]() { runLoop(); });
}

bool SerialWorker::switchTransport(const TransportConfig& cfg) {
    const std::string verr = validateTransportConfig(cfg);
    if (!verr.empty()) {
        return false;  // 本地配置错误：不执行任何切换（GUI 显示 Switch failed）
    }
    // 1) 旧会话 stop + join（会话重置：ProtocolEndpoint 随线程销毁重建，
    //    seq/ACK/decoder/FrameAssembler/PARTIAL base 全部清零，M6-C §十一）。
    stop();
    // 2) Input / RX 残留清理（M6-D §十三：切换期间清 pending input；
    //    §十二：不保留旧 Transport 残留字节，避免 stale 解析）。
    clearInputQueue();
    clearDisplayModeQueue();
    {
        std::lock_guard<std::mutex> lk(rxMutex_);
        rxBuf_.clear();
    }
    // 3) 会话级统计清零（切换后 GUI 从新会话重新计数，stress 可按会话验收）。
    resetSessionCounters();
    // 4) 启动新 Transport（幂等 start/startTcp；UART runtime switch 跳过复位
    //    脉冲 —— 对端会话已由 F12 test hook 建立，复位会打断其当前 Transport）。
    if (cfg.kind == TransportKind::kUart) {
        start(QString::fromStdString(cfg.uartPort), cfg.uartBaud, cfg.uartNoReset);
    } else {
        startTcp(cfg.tcpPort, QString::fromStdString(cfg.tcpBind));
    }
    return true;
}

TransportConfig SerialWorker::currentConfig() const {
    TransportConfig cfg;
    cfg.kind = kind_;
    cfg.uartPort = port_.toStdString();
    cfg.uartBaud = baud_;
    cfg.tcpBind = tcpBind_.toStdString();
    cfg.tcpPort = tcpPort_;
    cfg.uartNoReset = noReset_;
    return cfg;
}

void SerialWorker::clearInputQueue() {
    std::lock_guard<std::mutex> lk(inputMutex_);
    inputQueue_.clear();
}

void SerialWorker::resetSessionCounters() {
    committedFrames_ = 0;
    discardedFrames_ = 0;
    discardByReason_.fill(0);
    lastDiscardReason_ = 0;
    lastDiscardTimeMs_ = 0;
    peerWidth_ = 0;
    peerHeight_ = 0;
    peerPixelFormat_ = 0;
    lastFrameId_ = 0;
    lastFrameType_ = 0;
    sawAnyFrame_ = false;
    reconnectCount_ = 0;
    inputSent_ = 0;
    inputDropped_ = 0;
    modeSent_ = 0;
    modeDropped_ = 0;
    diag_.clear();
    statsWindowMs_ = 0;
    lastRxBytes_ = 0;
    lastTxBytes_ = 0;
    framesAtLastStats_ = 0;
}

void SerialWorker::stop() {
    stop_.store(true);
    // TCP Server：唤醒阻塞在 acceptOne 的 Worker 线程（只置标志，不碰 socket；
    // Worker 线程随后在收尾中 close() listener）。UART 模式无副作用。
    listener_.cancel();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool SerialWorker::isRunning() const {
    return thread_.joinable() && threadAlive_.load();
}

uint64_t SerialWorker::nowMs() const {
    return steadyMs();
}

bool SerialWorker::waitMs(uint64_t ms) {
    const uint64_t end = nowMs() + ms;
    while (!stop_.load()) {
        if (nowMs() >= end) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;  // 被 stop() 中断
}

void SerialWorker::emitStatus(WorkerStatus status, const QString& text) {
    emit statusChanged(status, text);
}

void SerialWorker::pushDiag(proto::Severity severity, std::string source,
                            std::string message) {
    diag_.push(nowMs(), severity, source, message);
    emit diagAdded(nowMs(), static_cast<int>(severity),
                   QString::fromStdString(source), QString::fromStdString(message));
}

void SerialWorker::emitStats() {
    WorkerStats st;
    if (ep_) {
        const proto::SessionStats& s = ep_->stats();
        // Connection
        st.transportState = static_cast<uint8_t>(transportState_);
        st.sessionState = static_cast<uint8_t>(ep_->state());
        st.transportConnected = transport_ != nullptr && transport_->isConnected();
        st.reconnectCount = reconnectCount_;
        // Display
        st.committedFrames = committedFrames_;
        st.discardedFrames = discardedFrames_;
        st.discardByReason = discardByReason_;
        st.lastDiscardReason = lastDiscardReason_;
        st.lastDiscardTimeMs = lastDiscardTimeMs_;
        st.peerWidth = peerWidth_;
        st.peerHeight = peerHeight_;
        st.peerPixelFormat = peerPixelFormat_;
        st.lastFrameId = lastFrameId_;
        st.lastFrameType = lastFrameType_;
        // Protocol
        st.rxBytes = transport_ ? transport_->rxBytes() : 0;
        st.txBytes = transport_ ? transport_->txBytes() : 0;
        st.rxMessages = s.rxMessages;
        st.txMessages = s.txMessages;
        st.packetsRx = s.packetsRx;
        st.decoderErrors = s.decoderErrors;
        st.crcErrors = s.crcErrors;
        st.seqGaps = s.seqGaps;
        st.sessionErrors = s.errors;
        // Heartbeat
        st.pingSent = s.txPing;
        st.pingReceived = s.rxPing;
        st.pongSent = s.txPong;
        st.pongReceived = s.rxPong;
        st.heartbeatTimeouts = s.heartbeatTimeouts;
        st.lastPingTimeMs = s.lastPingTimeMs;
        st.lastPongTimeMs = s.lastPongTimeMs;
        st.rttValid = s.rtt.lastMs.has_value();
        st.rttMs = s.rtt.lastMs.value_or(0);
        st.rttMinMs = s.rtt.minMs;
        st.rttMaxMs = s.rtt.maxMs;
        st.rttAvgMs = s.rtt.avgMs;
        st.rttSamples = s.rtt.samples;
        // Input
        st.inputSent = inputSent_;
        st.inputDropped = inputDropped_;
        // M6-C：Transport 类型 + TCP 对端地址（状态栏显示）。
        st.transportKind = static_cast<uint8_t>(kind_);
        if (kind_ == TransportKind::kTcp && tcp_) {
            const std::string peer = tcp_->peerAddress();
            std::snprintf(st.transportPeer, sizeof(st.transportPeer), "%s", peer.c_str());
        }

        // 吞吐/帧率（最近一次 emitStats 窗口，~500ms）
        const uint64_t now = nowMs();
        const uint64_t dt = (statsWindowMs_ == 0) ? 0 : now - statsWindowMs_;
        if (dt > 0) {
            const double secs = static_cast<double>(dt) / 1000.0;
            st.rxBytesPerSec = static_cast<double>(st.rxBytes - lastRxBytes_) / secs;
            st.txBytesPerSec = static_cast<double>(st.txBytes - lastTxBytes_) / secs;
            st.effectiveFps =
                static_cast<double>(committedFrames_ - framesAtLastStats_) / secs;
        }
        statsWindowMs_ = now;
        lastRxBytes_ = st.rxBytes;
        lastTxBytes_ = st.txBytes;
        framesAtLastStats_ = committedFrames_;
    }
    emit statsChanged(st);
}

void SerialWorker::runLoop() {
    sessionId_ = ++sessionCounter_;  // P1-2：新传输会话 epoch（跨线程读安全）
    threadAlive_.store(true);
    // ---- 激活 Transport（M6-A：UART / TCP 二选一；上层协议完全透明）----
    // 必须在 sink / 回调桥接之前：它们引用 transport_。
    if (kind_ == TransportKind::kTcp) {
        tcp_ = std::make_unique<HostTcpTransport>();
        transport_ = tcp_.get();
    } else {
        uart_ = std::make_unique<HostUartTransport>();
        transport_ = uart_.get();
    }

    // ---- Endpoint 回调（全部在 Worker 线程内触发）----
    proto::EndpointConfig cfg;
    cfg.protocol_version = proto::kProtocolVersion;
    cfg.device_class = 0;
    cfg.width = 320;
    cfg.height = 240;
    cfg.pixel_format = proto::PixelFormat::kRgb565;
    cfg.mode_mask = 0b1111;  // M7-C2：WINDOW | DEVICE | MIRROR | SPLIT
    cfg.device_name = "espview-pc";

    auto sink = [this](const uint8_t* d, size_t n) -> proto::SendStatus {
        return transport_ && transport_->send(d, n) ? proto::SendStatus::kOk
                                                    : proto::SendStatus::kError;
    };

    proto::ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [this](proto::SessionState s) {
        if (s == proto::SessionState::kConnected) {
            emitStatus(WorkerStatus::Connected,
                       QString("CONNECTED (HELLO done) — peer %1x%2")
                           .arg(peerWidth_)
                           .arg(peerHeight_));
            pushDiag(proto::Severity::kInfo, "session", "CONNECTED (HELLO done)");
        } else if (s == proto::SessionState::kDisconnected) {
            emitStatus(WorkerStatus::Connecting, "Session disconnected — waiting for HELLO");
            pushDiag(proto::Severity::kWarning, "session", "session disconnected");
        }
    };
    cb.onProtocolError = [this](proto::SessionError e, std::string_view detail) {
        (void)e;
        const QString msg = QString::fromUtf8(detail.data(), static_cast<int>(detail.size()));
        emitStatus(WorkerStatus::Error, QString("Protocol error: %1").arg(msg));
        pushDiag(proto::Severity::kError, "session", "protocol error: " + msg.toStdString());
    };
    cb.onHello = [this](const proto::HelloInfo& hello) {
        peerWidth_ = hello.width;
        peerHeight_ = hello.height;
        peerPixelFormat_ = static_cast<uint8_t>(hello.pixel_format);
    };
    cb.onFrameBegin = [this](const proto::FrameBeginInfo&) {
        currentFrame_ = DisplayFrame{};
    };
    cb.onFrameRect = [this](const proto::RectInfo& r, const uint8_t* pixels, size_t pixelBytes) {
        DisplayRect dr;
        dr.x = r.x;
        dr.y = r.y;
        dr.w = r.w;
        dr.h = r.h;
        dr.pixels.assign(pixels, pixels + pixelBytes);
        currentFrame_.rects.push_back(std::move(dr));
    };
    cb.onFrameCommit = [this](const proto::CommittedFrame& f) {
        currentFrame_.frameId = f.frameId;
        currentFrame_.frameType = static_cast<uint8_t>(f.frameType);
        currentFrame_.pixelFormat = static_cast<uint8_t>(f.pixelFormat);
        currentFrame_.width = f.width;
        currentFrame_.height = f.height;
        currentFrame_.rectCount = f.rectCount;
        currentFrame_.byteCount = f.byteCount;
        currentFrame_.sessionId = sessionId_;  // P1-2：打戳，GUI 按 epoch 门控
        ++committedFrames_;
        lastFrameId_ = f.frameId;
        lastFrameType_ = static_cast<uint8_t>(f.frameType);
        sawAnyFrame_ = true;
        emit frameReady(currentFrame_);
        currentFrame_ = DisplayFrame{};
    };
    cb.onFrameDiscard = [this](proto::FrameDiscardReason reason) {
        ++discardedFrames_;
        ++discardByReason_[static_cast<size_t>(reason)];
        lastDiscardReason_ = static_cast<uint8_t>(reason);
        lastDiscardTimeMs_ = nowMs();
        currentFrame_ = DisplayFrame{};
        pushDiag(proto::Severity::kWarning, "frame",
                 std::string("frame discarded: ") + proto::toString(reason));
    };
    cb.onError = [this](proto::ErrorCode code, std::string_view text) {
        const QString msg = QString::fromUtf8(text.data(), static_cast<int>(text.size()));
        if (code == proto::ErrorCode::kNone) {
            // M5-B 修正：ESP32 统计通道复用 ERROR 消息（code=kNone，DESIGN.md §M4
            // “统计经 ERROR 文本通道上报”）。仅作 INFO 诊断，不视为连接错误：
            // 否则每 ~3s 一次的 stats 消息会触发 clearDisplay() 清空已提交画面，
            // 而会话实际仍 CONNECTED（不会重连 → 画面永久停留在 No signal）。
            pushDiag(proto::Severity::kInfo, "peer", "stats: " + msg.toStdString());
            return;
        }
        emitStatus(WorkerStatus::Error,
                   QString("ERROR msg code=%1: %2").arg(static_cast<unsigned>(code)).arg(msg));
        pushDiag(proto::Severity::kError, "peer",
                 "ERROR code=" + std::to_string(static_cast<unsigned>(code)) + ": " +
                     msg.toStdString());
    };
    // M7-D1：CAPABILITIES 解析成功 → 原样转发 GUI（queued；消费接线在 main.cpp）。
    cb.onCapabilities = [this](const proto::CapabilitiesInfo& caps) {
        emit capabilitiesReceived(caps);
    };
    // M7-C3：SET_MODE 是 v0.1 唯一 ACK_REQ 控制消息，因此所有 ACK 结果都是
    // Display Mode 的 ACK。onAckTimeout（重试耗尽）同样按失败上报。
    cb.onAck = [this](uint16_t ackSeq, uint8_t status, proto::ErrorCode errorCode) {
        (void)ackSeq;
        (void)errorCode;
        emit displayModeAck(status == 0);
    };
    cb.onAckTimeout = [this](uint16_t lastSeq) {
        (void)lastSeq;
        emit displayModeAck(false);
    };

    ep_ = std::make_unique<proto::ProtocolEndpoint>(cfg, sink, cb, steadyMs);

    // ---- Transport 回调桥接（Transport 类型无关，§十八）----
    transport_->setDataCallback([this](const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(rxMutex_);
        rxBuf_.insert(rxBuf_.end(), data, data + len);
    });
    transport_->setStateCallback([this](IPcTransport::State s) {
        {
            std::lock_guard<std::mutex> lk(tstateMutex_);
            transportState_ = s;
        }
        // Worker 线程外（Transport RX/内部线程）回调：诊断条目只在状态变化时发。
        if (s == IPcTransport::State::Connected) {
            pushDiag(proto::Severity::kInfo, "transport", "transport connected");
        } else if (s == IPcTransport::State::Disconnected) {
            pushDiag(proto::Severity::kWarning, "transport", "transport disconnected");
        } else if (s == IPcTransport::State::Error) {
            pushDiag(proto::Severity::kError, "transport", "transport error");
        }
    });

    // ---- 主循环（UART：open → pump → 断开重连；TCP：listen → accept → pump）----
    if (kind_ == TransportKind::kTcp) {
        runLoopTcp();
    } else {
        runLoopUart();
    }

    // ---- 收尾：close + join RX 线程 + 关 listener + 清会话（此后无任何 signal）----
    if (transport_) {
        transport_->close();
    }
    listener_.close();
    ep_->onTransportDisconnected();
    emitStatus(WorkerStatus::Disconnected, "Stopped");
    ep_.reset();
    transport_ = nullptr;
    uart_.reset();
    tcp_.reset();
    threadAlive_.store(false);  // M6-D：收尾完成，Worker 线程即将退出
}

void SerialWorker::runLoopUart() {
    while (!stop_.load()) {
        emitStatus(WorkerStatus::Connecting,
                   QString("Opening %1 @ %2 ...").arg(port_).arg(baud_));

        HostUartTransport::Config tcfg;
        tcfg.port = port_.toStdString();
        tcfg.baud = baud_;
        tcfg.read_timeout_ms = 50;
        tcfg.reset_on_open = !noReset_;  // 复位 ESP32 → 确定性 boot HELLO + FULL 重同步

        if (!transport_->open(tcfg)) {
            ++reconnectCount_;
            emitStatus(WorkerStatus::Error, "Open failed — retrying in 1.5s");
            pushDiag(proto::Severity::kError, "transport", "open failed — retrying");
            ep_->onTransportDisconnected();
            if (!waitMs(1500)) {
                break;
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(tstateMutex_);
            transportState_ = IPcTransport::State::Connected;
        }
        emitStatus(WorkerStatus::Connecting, "Waiting for HELLO");
        pumpLoop(nowMs());

        if (stop_.load()) {
            break;
        }
        ++reconnectCount_;
        emitStatus(WorkerStatus::Disconnected, "Link lost — reconnecting in 1.5s");
        pushDiag(proto::Severity::kWarning, "transport", "link lost — reconnecting");
        ep_->onTransportDisconnected();
        transport_->close();
        if (!waitMs(1500)) {
            break;
        }
    }
}

void SerialWorker::runLoopTcp() {
    // PC = TCP Server（§二/§九）。bind 失败：报告 WinSock error（§二十七），
    // 不自动改防火墙；状态面板可见监听地址/端口（§三十三）。
    TcpListener::Config lcfg;
    lcfg.bind = tcpBind_.toStdString();
    lcfg.port = tcpPort_;
    if (!listener_.bindListen(lcfg)) {
        const QString detail = QString::fromStdString(listener_.lastError());
        emitStatus(WorkerStatus::Error,
                   QString("TCP bind %1:%2 failed — %3")
                       .arg(tcpBind_)
                       .arg(tcpPort_)
                       .arg(detail));
        pushDiag(proto::Severity::kError, "transport",
                 "TCP bind failed: " + detail.toStdString());
        return;
    }
    pushDiag(proto::Severity::kInfo, "transport",
             "TCP listening " + tcpBind_.toStdString() + ":" + std::to_string(tcpPort_));

    while (!stop_.load()) {
        emitStatus(WorkerStatus::Connecting,
                   QString("TCP %1:%2 — waiting for ESP32").arg(tcpBind_).arg(tcpPort_));

        HostTcpTransport::Config tcfg;
        tcfg.rx_timeout_ms = 100;
        tcfg.send_timeout_ms = 5000;

        if (!listener_.acceptOne(*tcp_)) {
            if (stop_.load()) {
                break;
            }
            ++reconnectCount_;
            emitStatus(WorkerStatus::Error, "TCP accept failed — retrying");
            pushDiag(proto::Severity::kWarning, "transport", "TCP accept failed — retrying");
            tcp_->close();
            if (!waitMs(1000)) {
                break;
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(tstateMutex_);
            transportState_ = IPcTransport::State::Connected;
        }
        emitStatus(WorkerStatus::Connecting, "TCP client connected — waiting for HELLO");
        pumpLoop(nowMs());

        if (stop_.load()) {
            break;
        }
        ++reconnectCount_;
        emitStatus(WorkerStatus::Disconnected, "TCP link lost — waiting for reconnect");
        pushDiag(proto::Severity::kWarning, "transport", "TCP link lost — waiting for reconnect");
        ep_->onTransportDisconnected();
        tcp_->close();  // 关闭已 accept 的 socket；listener 保持监听（§十一 re-accept）
        if (!waitMs(1000)) {
            break;
        }
    }
}

void SerialWorker::pumpLoop(uint64_t openAtMs) {
    uint64_t lastHelloMs = 0;
    uint64_t lastStatsMs = 0;
    while (!stop_.load()) {
        drainAndTick();

        const uint64_t now = nowMs();
        // 被动握手回退：7s 未 CONNECTED → 主动发 HELLO；之后每 7s 重试
        // （与 com3_frame_test 的 helloInitiated 分支一致，M1-2 实测）。
        if (!ep_->isConnected() && now - openAtMs >= 7000 && now - lastHelloMs >= 7000) {
            lastHelloMs = now;
            emitStatus(WorkerStatus::Connecting, "Initiating HELLO (passive wait timeout)");
            ep_->onTransportConnected();
        }

        IPcTransport::State ts;
        {
            std::lock_guard<std::mutex> lk(tstateMutex_);
            ts = transportState_;
        }
        if (ts == IPcTransport::State::Disconnected ||
            ts == IPcTransport::State::Error) {
            return;  // 断开/错误 → runLoop 负责重连
        }

        // M3：输入队列（GUI → Worker）在此 drain；与 RX/心跳同一线程，天然串行，
        // 不引入额外锁。发送走 endpoint.sendMessage（sendMutex_ 原子整消息）。
        drainInputQueue();
        drainDisplayModeQueue();  // M7-C3：Display Mode（SET_MODE）队列同线程 drain

        if (now - lastStatsMs >= 500) {
            lastStatsMs = now;
            emitStats();
        }
        waitMs(5);
    }
}

void SerialWorker::drainAndTick() {
    std::vector<uint8_t> chunk;
    {
        std::lock_guard<std::mutex> lk(rxMutex_);
        if (!rxBuf_.empty()) {
            chunk.swap(rxBuf_);
        }
    }
    if (!chunk.empty() && ep_) {
        ep_->onTransportData(chunk.data(), chunk.size());
    }
    if (ep_) {
        ep_->tick();
    }
}

void SerialWorker::sendInput(const espview::input::InputEvent& ev) {
    std::lock_guard<std::mutex> lk(inputMutex_);
    inputQueue_.push_back(ev);
}

void SerialWorker::drainInputQueue() {
    std::vector<espview::input::InputEvent> queue;
    {
        std::lock_guard<std::mutex> lk(inputMutex_);
        if (inputQueue_.empty()) {
            return;
        }
        queue.swap(inputQueue_);
    }
    if (!ep_) {
        inputDropped_ += queue.size();
        return;
    }
    // 编码/校验上界：对端 HELLO 分辨率（未握手时用默认 320x240）。
    const uint16_t maxX = peerWidth_ > 0 ? static_cast<uint16_t>(peerWidth_ - 1) : 319u;
    const uint16_t maxY = peerHeight_ > 0 ? static_cast<uint16_t>(peerHeight_ - 1) : 239u;
    for (const espview::input::InputEvent& ev : queue) {
        // fire-and-forget（spec §13：不引入 ACK）；未连接时整包丢弃（重连后
        // ESP32 InputManager::resetState() 已清空状态，不会 stuck）。
        if (!ep_->isConnected()) {
            ++inputDropped_;
            continue;
        }
        const auto msg = espview::input::encodeInputEvent(ev, maxX, maxY);
        if (!msg.has_value()) {
            ++inputDropped_;  // 编码拒绝（防御：InputController 已 clamp/校验）
            continue;
        }
        if (ep_->sendMessage(*msg) == proto::SendResult::kOk) {
            ++inputSent_;
        } else {
            ++inputDropped_;
        }
    }
}

void SerialWorker::sendDisplayMode(uint8_t mode) {
    std::lock_guard<std::mutex> lk(modeMutex_);
    displayModeQueue_.push_back(mode);
}

void SerialWorker::clearDisplayModeQueue() {
    std::lock_guard<std::mutex> lk(modeMutex_);
    displayModeQueue_.clear();
}

void SerialWorker::drainDisplayModeQueue() {
    std::vector<uint8_t> queue;
    {
        std::lock_guard<std::mutex> lk(modeMutex_);
        if (displayModeQueue_.empty()) {
            return;
        }
        queue.swap(displayModeQueue_);
    }
    if (!ep_) {
        modeDropped_ += queue.size();
        return;
    }
    for (const uint8_t mode : queue) {
        // 未连接时整包丢弃并计数（同 sendInput 语义）；ACK 不会发出，
        // GUI 侧 DisplayUiState 已处于 "Waiting for connection" 标记。
        if (!ep_->isConnected()) {
            ++modeDropped_;
            continue;
        }
        // payload 仍为 [0] mode（1 byte，冻结语义）；makeSetMode 自动置
        // ACK_REQ，sendMessage 自动登记 pending ACK（超时重试 2 次后
        // onAckTimeout → displayModeAck(false)）。
        const proto::Message msg =
            proto::makeSetMode(static_cast<proto::DisplayMode>(mode));
        if (ep_->sendMessage(msg) == proto::SendResult::kOk) {
            ++modeSent_;
        } else {
            ++modeDropped_;
        }
    }
}

}  // namespace pc
}  // namespace espview
