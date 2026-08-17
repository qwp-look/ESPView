// ESPView M2 — SerialWorker 实现（见 serial_worker.h 的线程模型说明）。

#include "serial_worker.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
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
    clearWifiQueue();  // M8-A3（J M1）：析构清理残留命令（含未发送的密码副本，AF.4）
}

void SerialWorker::start(const QString& port, quint32 baud, bool noReset) {
    stop();  // 幂等：先清理残留
    clearWifiQueue();  // M8-A3（J M2）：新会话不携带旧 Wi-Fi 命令（含密码副本，AF.4）
    kind_ = TransportKind::kUart;
    port_ = port;
    baud_ = baud;
    noReset_ = noReset;
    stop_.store(false);
    {
        std::lock_guard<std::mutex> lk(tstateMutex_);
        transportState_ = espview::transport::ITransport::State::kDisconnected;
    }
    thread_ = std::thread([this]() { runLoop(); });
}

void SerialWorker::startTcp(uint16_t port, const QString& bind) {
    stop();  // 幂等：先清理残留
    clearWifiQueue();  // M8-A3（J M2）：新会话不携带旧 Wi-Fi 命令（含密码副本，AF.4）
    kind_ = TransportKind::kTcp;
    tcpPort_ = port;
    tcpBind_ = bind;
    stop_.store(false);
    {
        std::lock_guard<std::mutex> lk(tstateMutex_);
        transportState_ = espview::transport::ITransport::State::kDisconnected;
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
    clearWifiQueue();  // M7-D3：清 Wi-Fi 命令（含未发送的密码副本）
    {
        std::lock_guard<std::mutex> lk(rxMutex_);
        rxBuf_.clear();
        (void)rxBuf_.takeDropped();  // 切换：溢出计数清零（新会话重新计数）
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
    (void)inputQueue_.takeDropped();  // 切换：丢弃计数清零
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
    lastStatus_.store(static_cast<int>(status));  // M7-G：状态快照（GUI 可读）
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
        {
            // M8-A3：transportState_ 由 Transport 内部线程写（tstateMutex_ 保护），
            // emitStats 在 Worker 线程读 —— 必须同锁读（消除无锁读竞态）。
            std::lock_guard<std::mutex> lk(tstateMutex_);
            st.transportState = static_cast<uint8_t>(transportState_);
        }
        st.sessionState = static_cast<uint8_t>(ep_->state());
        // M8-A3：Transport 统计经 TransportManager 诊断快照（值语义，线程安全）。
        const espview::transport::TransportDiagSnapshot snap =
            mgr_ ? mgr_->diagSnapshot() : espview::transport::TransportDiagSnapshot{};
        st.transportConnected = snap.connected;
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
        st.rxBytes = snap.rxBytes;
        st.txBytes = snap.txBytes;
        // M8-A3：rxBuf_ 溢出丢弃字节数（诊断；队列 bound 证据）。
        st.rxOverflowDropped = rxOverflowDropped_;
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
    // ---- TransportManager（M8-A3）：UART 经工厂创建（每次 open 新建）；----
    // TCP 走 accept → adopt（不重复 open）。上层协议完全不知道 Transport 类型。
    espview::transport::TransportManager::TransportFactory factory =
        [this](espview::transport::TransportType t)
            -> std::shared_ptr<espview::transport::ITransport> {
            if (t != espview::transport::TransportType::kUart) {
                return nullptr;  // TCP：由 acceptOne → adopt 提供（不使用工厂）
            }
            HostUartTransport::Config ucfg;
            ucfg.port = port_.toStdString();
            ucfg.baud = baud_;
            ucfg.read_timeout_ms = 50;
            ucfg.reset_on_open = !noReset_;  // 复位 ESP32 → 确定性 boot HELLO + FULL 重同步
            return std::make_shared<HostUartTransport>(ucfg);
        };
    mgr_ = std::make_unique<espview::transport::TransportManager>(factory, kind_);

    // ---- Endpoint 回调（全部在 Worker 线程内触发）----
    proto::EndpointConfig cfg;
    cfg.protocol_version = proto::kProtocolVersion;
    cfg.device_class = 0;
    cfg.width = 320;
    cfg.height = 240;
    cfg.pixel_format = proto::PixelFormat::kRgb565;
    cfg.mode_mask = 0b1111;  // M7-C2：WINDOW | DEVICE | MIRROR | SPLIT
    cfg.device_name = "espview-pc";

    // M8-A3：TransportSink 统一收口（paced/unpaced + 发送门 + 背压重试）。
    // alive：会话 DISCONNECTED 时放弃发送（与 ESP32 侧 transportSink 一致）。
    // sleep：UART pacing 重试间隔（waitMs 可被 stop() 中断）。
    sink_ = std::make_unique<espview::transport::TransportSink>(
        *mgr_,
        [this]() { return ep_ != nullptr && ep_->state() != proto::SessionState::kDisconnected; },
        [this]() { return steadyMs(); },
        [this](uint32_t ms) { waitMs(ms); });

    // PacketSink（正常帧流：阻塞式，paced 时按 TxPolicy 重试）。
    auto sink = [this](const uint8_t* d, size_t n) -> proto::SendStatus {
        return sink_ != nullptr ? sink_->send(d, n) : proto::SendStatus::kError;
    };
    // TrySink（PONG/ACK/PING/ACK 重试专用：单次尽力，绝不阻塞 RX/会话线程）。
    auto trySink = [this](const uint8_t* d, size_t n) -> proto::SendStatus {
        return sink_ != nullptr ? sink_->trySend(d, n) : proto::SendStatus::kError;
    };

    proto::ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [this](proto::SessionState s) {
        if (s == proto::SessionState::kConnected) {
            // M7-F（AE.3）：会话建立 → 预览可用（原先缺失，导致物理预览
            // 永远显示 No Preview）。
            previewState_.setSessionConnected(true);
            emitStatus(WorkerStatus::Connected,
                       QString("CONNECTED (HELLO done) — peer %1x%2")
                           .arg(peerWidth_)
                           .arg(peerHeight_));
            pushDiag(proto::Severity::kInfo, "session", "CONNECTED (HELLO done)");
        } else if (s == proto::SessionState::kDisconnected) {
            previewState_.setSessionConnected(false);
            emitStatus(WorkerStatus::Connecting, "Session disconnected — waiting for HELLO");
            pushDiag(proto::Severity::kWarning, "session", "session disconnected");
            // M7-D2（AE.3）：断线清空预览位图（发 No Preview 快照给 GUI）。
            previewState_.onDisconnected();
            emit previewFrame(previewState_);
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
        // M7-G：缓存最近一次能力快照（向导可能在能力事件后打开，需自动前进）。
        {
            std::lock_guard<std::mutex> lk(capsMutex_);
            lastCaps_ = caps;
        }
        emit capabilitiesReceived(caps);
    };
    // M7-D2：PHYSICAL_PREVIEW 解析成功 → 写入预览模型（去重/过期在模型内）
    // 并转发 GUI（queued；消费接线在 main.cpp）。
    cb.onPhysicalPreview = [this](const proto::PhysicalPreviewInfo& info,
                                  const uint8_t* pixels, size_t pixelBytes) {
        if (pixels == nullptr || pixelBytes == 0) {
            return;
        }
        std::vector<uint8_t> px(pixels, pixels + pixelBytes);
        if (previewState_.setFrame(info.frameId, info.width, info.height,
                                   static_cast<uint8_t>(info.pixelFormat),
                                   std::move(px), nowMs())) {
            emit previewFrame(previewState_);
        }
    };
    // M7-D3：WIFI_SCAN_RESULT / WIFI_STATUS 解析成功 → 原样转发 GUI（queued；
    // 消费接线在 main.cpp/ConnectionManager，D4 向导消费）。
    cb.onWifiScanResult = [this](const proto::WifiScanResultInfo& result) {
        emit wifiScanResult(result);
    };
    cb.onWifiStatus = [this](const proto::WifiStatusInfo& status) {
        emit wifiStatus(status);
    };
    // M7-D3：ACK 按最近一次发送的 ACK_REQ 类型分派（endpoint 单槽 pending ACK：
    // 最近发送者即等待 ACK 者；有序字节流保证先到先 ACK，无跨类型串扰）。
    // SET_MODE → displayModeAck；WIFI_SCAN_REQ → wifiScanReqAck（探针语义）；
    // WIFI_CONFIG → wifiConfigAck。onAckTimeout（重试耗尽）同样按类型分派失败。
    cb.onAck = [this](uint16_t ackSeq, uint8_t status, proto::ErrorCode errorCode) {
        (void)ackSeq;
        const uint8_t kind = pendingAckKind_;
        pendingAckKind_ = 0;  // 单槽：ACK 到达即清（防重复 ACK 二次上报）
        if (kind == static_cast<uint8_t>(proto::MessageType::kSetMode)) {
            emit displayModeAck(status == 0);
        } else if (kind == static_cast<uint8_t>(proto::MessageType::kWifiScanReq)) {
            emit wifiScanReqAck(status == 0, static_cast<quint16>(errorCode));
        } else if (kind == static_cast<uint8_t>(proto::MessageType::kWifiConfig)) {
            emit wifiConfigAck(status == 0, static_cast<quint16>(errorCode));
        }
    };
    cb.onAckTimeout = [this](uint16_t lastSeq) {
        (void)lastSeq;
        const uint8_t kind = pendingAckKind_;
        pendingAckKind_ = 0;
        if (kind == static_cast<uint8_t>(proto::MessageType::kSetMode)) {
            emit displayModeAck(false);
        } else if (kind == static_cast<uint8_t>(proto::MessageType::kWifiScanReq)) {
            emit wifiScanReqAck(false, 0);
        } else if (kind == static_cast<uint8_t>(proto::MessageType::kWifiConfig)) {
            emit wifiConfigAck(false, 0);
        }
    };

    ep_ = std::make_unique<proto::ProtocolEndpoint>(cfg, sink, trySink, cb, steadyMs);

    // ---- Transport 回调桥接（Transport 类型无关，§十八）----
    mgr_->setDataCallback([this](const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lk(rxMutex_);
        rxBuf_.append(data, len);  // M8-A3：有界（64KB，drop-newest）
    });
    mgr_->setStateCallback([this](espview::transport::ITransport::State s) {
        {
            std::lock_guard<std::mutex> lk(tstateMutex_);
            transportState_ = s;
        }
        // Worker 线程外（Transport RX/内部线程）回调：诊断条目只在状态变化时发。
        if (s == espview::transport::ITransport::State::kConnected) {
            pushDiag(proto::Severity::kInfo, "transport", "transport connected");
        } else if (s == espview::transport::ITransport::State::kDisconnected) {
            pushDiag(proto::Severity::kWarning, "transport", "transport disconnected");
        } else if (s == espview::transport::ITransport::State::kError) {
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
    if (mgr_) {
        mgr_->close();
    }
    listener_.close();
    ep_->onTransportDisconnected();
    emitStatus(WorkerStatus::Disconnected, "Stopped");
    ep_.reset();
    sink_.reset();
    mgr_.reset();
    tcp_.reset();
    threadAlive_.store(false);  // M6-D：收尾完成，Worker 线程即将退出
}

void SerialWorker::runLoopUart() {
    while (!stop_.load()) {
        emitStatus(WorkerStatus::Connecting,
                   QString("Opening %1 @ %2 ...").arg(port_).arg(baud_));

        // M8-A3：经 TransportManager 打开（工厂创建 HostUartTransport + open；
        // open 内部状态回调 Connecting/Connected 已转发到 transportState_）。
        if (!mgr_->open()) {
            ++reconnectCount_;
            emitStatus(WorkerStatus::Error, "Open failed — retrying in 1.5s");
            pushDiag(proto::Severity::kError, "transport", "open failed — retrying");
            ep_->onTransportDisconnected();
            if (!waitMs(1500)) {
                break;
            }
            continue;
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
        mgr_->close();
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

        // M8-A3：每次 accept 使用全新 HostTcpTransport（复用状态残留清零）；
        // acceptOne 成功（attach）后 adopt 进 TransportManager 建立发送门。
        tcp_ = std::make_shared<HostTcpTransport>();
        if (!listener_.acceptOne(*tcp_)) {
            if (stop_.load()) {
                break;
            }
            ++reconnectCount_;
            emitStatus(WorkerStatus::Error, "TCP accept failed — retrying");
            pushDiag(proto::Severity::kWarning, "transport", "TCP accept failed — retrying");
            tcp_->close();
            tcp_.reset();
            if (!waitMs(1000)) {
                break;
            }
            continue;
        }
        if (!mgr_->adopt(TransportKind::kTcp, tcp_)) {
            ++reconnectCount_;
            emitStatus(WorkerStatus::Error, "TCP adopt failed — retrying");
            pushDiag(proto::Severity::kError, "transport", "TCP adopt failed — retrying");
            tcp_->close();
            tcp_.reset();
            if (!waitMs(1000)) {
                break;
            }
            continue;
        }
        pushDiag(proto::Severity::kInfo, "transport", "transport connected");
        {
            // M8-A3（C/E）：条件置位 —— adopt 窗口内若对端已断开（accept 后立即
            // close），不得覆盖为 Connected（否则 pumpLoop 永不退出）。adopt 已
            // 校验 t->isConnected()，此处仅防御窗口竞态。
            std::lock_guard<std::mutex> lk(tstateMutex_);
            transportState_ = tcp_->isConnected()
                                  ? espview::transport::ITransport::State::kConnected
                                  : espview::transport::ITransport::State::kDisconnected;
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
        mgr_->close();  // 关闭已 accept 的 socket；listener 保持监听（§十一 re-accept）
        tcp_.reset();
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

        espview::transport::ITransport::State ts;
        {
            std::lock_guard<std::mutex> lk(tstateMutex_);
            ts = transportState_;
        }
        if (ts == espview::transport::ITransport::State::kDisconnected ||
            ts == espview::transport::ITransport::State::kError) {
            return;  // 断开/错误 → runLoop 负责重连
        }

        // M3：输入队列（GUI → Worker）在此 drain；与 RX/心跳同一线程，天然串行，
        // 不引入额外锁。发送走 endpoint.sendMessage（sendMutex_ 原子整消息）。
        drainInputQueue();
        drainDisplayModeQueue();  // M7-C3：Display Mode（SET_MODE）队列同线程 drain
        drainWifiQueue();     // M7-D3：Wi-Fi 命令（SCAN_REQ / CONFIG）同线程 drain

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
        rxOverflowDropped_ += rxBuf_.takeDropped();  // M8-A3：溢出计数（诊断）
        if (!rxBuf_.empty()) {
            chunk = rxBuf_.takeAll();
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
    inputQueue_.push(ev, false);  // M8-A3：有界 256，drop-newest（丢新计数在 drain 汇入）
}

void SerialWorker::drainInputQueue() {
    std::vector<espview::input::InputEvent> queue;
    {
        std::lock_guard<std::mutex> lk(inputMutex_);
        inputDropped_ += inputQueue_.takeDropped();  // 队列溢出（drop-newest）
        if (inputQueue_.empty()) {
            return;
        }
        queue = inputQueue_.takeAll();
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
    displayModeQueue_.push(mode, true);  // M8-A3：有界 8，latest-wins（满时丢最旧）
}

void SerialWorker::clearDisplayModeQueue() {
    std::lock_guard<std::mutex> lk(modeMutex_);
    displayModeQueue_.clear();
    (void)displayModeQueue_.takeDropped();  // 会话切换：丢弃计数清零
}

// ---- M7-D3：Wi-Fi 命令队列 ----

void SerialWorker::sendWifiScanRequest(uint8_t maxEntries) {
    WifiCommand cmd;
    cmd.kind = 0;  // scan
    cmd.maxEntries = maxEntries;
    std::lock_guard<std::mutex> lk(wifiMutex_);
    pushWifiLocked(std::move(cmd));
}

void SerialWorker::sendWifiConfig(const std::string& ssid, const std::string& password,
                                  uint32_t serverIp, uint16_t serverPort) {
    WifiCommand cmd;
    cmd.kind = 1;  // config
    cmd.ssid = ssid;
    cmd.password = password;  // 仅内存驻留（AF.4）；发送后 drain 清零
    cmd.serverIp = serverIp;
    cmd.serverPort = serverPort;
    std::lock_guard<std::mutex> lk(wifiMutex_);
    pushWifiLocked(std::move(cmd));
}

void SerialWorker::sendWifiClear() {
    WifiCommand cmd;
    cmd.kind = 2;  // clear
    std::lock_guard<std::mutex> lk(wifiMutex_);
    pushWifiLocked(std::move(cmd));
}

bool SerialWorker::pushWifiLocked(WifiCommand&& cmd) {
    // M8-A3（C/F）：latest-wins（有界 4）—— 满时丢最旧；被丢项若含密码先清零
    // （AF.4 凭据红线）。丢弃计数由 BoundedQueue 单点累计（drop 回调内驱逐），
    // drain 时经 takeDropped() 汇入 wifiDropped_（避免跨线程计数竞争）。
    return wifiQueue_.push(std::move(cmd), true,
                           [this](WifiCommand& old) { zeroWifiPassword(old); });
}

void SerialWorker::zeroWifiPassword(WifiCommand& cmd) {
    if (!cmd.password.empty()) {
        std::fill(cmd.password.begin(), cmd.password.end(), '\0');
    }
}

bool SerialWorker::lastCapabilities(espview::proto::CapabilitiesInfo& out) const {
    std::lock_guard<std::mutex> lk(capsMutex_);
    if (!lastCaps_.has_value()) {
        return false;
    }
    out = *lastCaps_;
    return true;
}

void SerialWorker::clearWifiQueue() {
    std::lock_guard<std::mutex> lk(wifiMutex_);
    std::vector<WifiCommand> q = wifiQueue_.takeAll();
    (void)wifiQueue_.takeDropped();  // 会话切换：丢弃计数清零
    for (WifiCommand& cmd : q) {
        zeroWifiPassword(cmd);
    }
}

void SerialWorker::drainWifiQueue() {
    std::vector<WifiCommand> queue;
    {
        std::lock_guard<std::mutex> lk(wifiMutex_);
        wifiDropped_ += wifiQueue_.takeDropped();  // M8-A3：队列溢出（latest-wins）
        if (wifiQueue_.empty()) {
            return;
        }
        queue = wifiQueue_.takeAll();
    }
    if (!ep_) {
        wifiDropped_ += queue.size();
        for (WifiCommand& cmd : queue) {
            zeroWifiPassword(cmd);  // M8-A3（J M2）：未发送也要清零密码副本（AF.4）
        }
        return;
    }
    for (WifiCommand& cmd : queue) {
        // 未连接时丢弃并计数（同 sendInput 语义）；密码副本仍须清零。
        bool sent = false;
        if (ep_->isConnected()) {
            std::optional<proto::Message> msg;
            if (cmd.kind == 0) {
                msg = proto::makeWifiScanReq(0, cmd.maxEntries);
            } else if (cmd.kind == 1) {
                // M7-G（B3，AF.4 凭据红线）：WIFI_CONFIG 只经 UART bootstrap
                // 下发；TCP / 其他传输一律丢弃（密码副本随后清零），绝不发送
                // 真实凭据。扫描/清除不携带凭据，任何传输均允许。
                if (kind_ != TransportKind::kUart) {
                    ++wifiDropped_;
                    pushDiag(proto::Severity::kError, "wifi",
                             "WIFI_CONFIG rejected: credentials are UART-bootstrap only");
                    if (!cmd.password.empty()) {
                        zeroWifiPassword(cmd);
                    }
                    continue;
                }
                msg = proto::makeWifiConfig(cmd.ssid, cmd.password, cmd.serverIp,
                                            cmd.serverPort);
            } else {
                msg = proto::makeWifiClear();
            }
            if (msg.has_value() &&
                ep_->sendMessage(*msg) == proto::SendResult::kOk) {
                sent = true;
                ++wifiSent_;
                // 记录最近 ACK_REQ 类型（单槽 pending ACK 归属）。
                pendingAckKind_ = msg->type;
            }
        }
        if (!sent) {
            ++wifiDropped_;
        }
        // 安全（AF.4）：发送/丢弃后立即清零密码副本（不进入任何持久化路径）。
        if (!cmd.password.empty()) {
            zeroWifiPassword(cmd);
        }
    }
}

void SerialWorker::drainDisplayModeQueue() {
    std::vector<uint8_t> queue;
    {
        std::lock_guard<std::mutex> lk(modeMutex_);
        modeDropped_ += displayModeQueue_.takeDropped();  // M8-A3：队列溢出（latest-wins）
        if (displayModeQueue_.empty()) {
            return;
        }
        queue = displayModeQueue_.takeAll();
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
