// ESPView M2/M6-A — SerialWorker：Transport → 协议 → DisplayFrame 工作线程。
//
// 规范来源：docs/DESIGN.md M2 节（Serial Worker 线程模型 + 被动 HELLO 握手）
//   + M6-A 任务书 §三十二/§三十三（ConnectionManager 支持 UART + TCP）。
//
// 线程模型：SerialWorker 本身是 QObject（GUI 线程亲和），内部持有一条
// std::thread 执行 runLoop()。Worker 线程独占：
//   IPcTransport（UART: HostUartTransport / TCP: HostTcpTransport）
//   + ProtocolEndpoint（内部含 StreamDecoder + FrameAssembler）。
// Transport 是编译期/启动时选择的（start() = UART，startTcp() = TCP Server）；
// 上层 ProtocolEndpoint / DisplayFrame 完全不知道 Transport 类型（§十八）。
// 所有 Qt signal 从 Worker 线程 emit，经 Qt::QueuedConnection 投递到 GUI；
// Worker 不创建 QWidget / QImage，不做任何 GUI 操作。
//
// 接线（与 pc/src/com3_frame_test.cpp 相同模式，M1-2/M1-3B 实测）：
//   - UART：被动握手（open 后先等 ESP32 boot HELLO；7s 未 CONNECTED →
//     主动 onTransportConnected() 发 HELLO，之后每 7s 重试）；
//   - TCP（PC = Server，§二）：bindListen → acceptOne → 等对端 HELLO → pump；
//     断开后 re-accept（§十一），不需要复位 ESP32；
//   - Transport dataCallback（Transport 内部 RX 线程）→ 互斥字节队列 →
//     runLoop 单线程 drain → ep->onTransportData()；ep->tick() 每 ~5ms；
//   - Transport 断开/Error → ep->onTransportDisconnected() → close → 重连；
//   - stop() 置停止标志（TCP 下额外 cancel() 唤醒 acceptOne）并 join 线程；
//     join 返回后不再有任何回调 / signal。

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QObject>
#include <QString>

#include "display_frame.h"
#include "host_tcp_transport.h"
#include "input_event.h"
#include "pc_transport.h"
#include "runtime_stats.h"  // proto::Severity / proto::DiagnosticsRing（M4 诊断）
#include "serial_transport.h"
#include "transport_config.h"

namespace espview {
namespace proto {
class ProtocolEndpoint;
}

namespace pc {

// GUI 可显示的连接状态（Worker → MainWindow，queued）。
enum class WorkerStatus : uint8_t {
    Disconnected = 0,
    Connecting = 1,
    Connected = 2,
    Error = 3,
};

// 状态栏统计快照（Worker → MainWindow，queued，约每 500ms 一次）。
// M4：分域（Connection / Display / Protocol / Heartbeat / Input）；统计层不改变协议。
struct WorkerStats {
    // ---- Connection（Transport 与 Protocol Session 分层，spec §四）----
    uint8_t transportState = 0;   // 0=Disconnected 1=Connecting 2=Connected 3=Error
    uint8_t sessionState = 0;     // proto::SessionState 数值
    bool transportConnected = false;
    uint64_t reconnectCount = 0;  // 断线重连/重试次数

    // ---- Display（帧统计 + 吞吐/帧率）----
    uint64_t committedFrames = 0;     // 成功提交帧数
    uint64_t discardedFrames = 0;     // 作废帧总数
    std::array<uint64_t, 11> discardByReason{};  // 按 FrameDiscardReason 计数
    uint8_t lastDiscardReason = 0;    // 最近一次作废原因（FrameDiscardReason）
    uint64_t lastDiscardTimeMs = 0;
    uint16_t peerWidth = 0;           // 对端 HELLO 分辨率
    uint16_t peerHeight = 0;
    uint8_t peerPixelFormat = 0;      // 0 = RGB565
    uint16_t lastFrameId = 0;
    uint8_t lastFrameType = 0;        // 0 = FULL，1 = PARTIAL
    double effectiveFps = 0.0;        // 最近统计窗口的提交帧率

    // ---- Protocol（Packet/字节/错误分类，spec §七）----
    uint64_t rxBytes = 0;             // Transport 收到的原始字节
    uint64_t txBytes = 0;             // Transport 发出的原始字节
    double rxBytesPerSec = 0.0;       // 最近窗口接收吞吐
    double txBytesPerSec = 0.0;       // 最近窗口发送吞吐
    uint64_t rxMessages = 0;
    uint64_t txMessages = 0;
    uint64_t packetsRx = 0;           // 收到并通过 CRC 的 Packet 数
    uint64_t decoderErrors = 0;       // CRC / seq / 重同步（StreamDecoder 错误总量）
    uint64_t crcErrors = 0;
    uint64_t seqGaps = 0;
    uint64_t sessionErrors = 0;       // 会话/协议错误（SessionError）

    // ---- Heartbeat（spec §五/§六；RTT 无测量 ≠ 0ms）----
    uint64_t pingSent = 0;
    uint64_t pingReceived = 0;
    uint64_t pongSent = 0;
    uint64_t pongReceived = 0;
    uint64_t heartbeatTimeouts = 0;
    uint64_t lastPingTimeMs = 0;
    uint64_t lastPongTimeMs = 0;
    bool rttValid = false;            // 最近一次 PING→PONG 是否有有效测量
    uint32_t rttMs = 0;               // lastRtt（rttValid 时有效）
    uint32_t rttMinMs = 0;
    uint32_t rttMaxMs = 0;
    uint32_t rttAvgMs = 0;
    uint64_t rttSamples = 0;

    // ---- Input（PC → ESP32 反向通道，M3）----
    uint64_t inputSent = 0;           // 成功经 ProtocolEndpoint 送出的输入消息
    uint64_t inputDropped = 0;        // 未连接 / 编码拒绝被丢弃

    // ---- M6-C：Transport 类型与对端（状态栏/诊断）----
    uint8_t transportKind = 0;        // 0=UART 1=TCP（SerialWorker 启动时选择）
    char transportPeer[48] = {};      // TCP 对端 "ip:port"（UART 为空串）
};

class SerialWorker : public QObject {
    Q_OBJECT
public:
    explicit SerialWorker(QObject* parent = nullptr);
    ~SerialWorker() override;  // 内部 stop() + join，安全析构

    // 启动/停止（幂等）。start() 若正在运行会先 stop()。
    // UART：open COM port（复位 ESP32 → boot HELLO → FULL 重同步）。
    // M6-C test-only：noReset=true 时跳过 DTR/RTS 复位脉冲（运行时 transport
    // 切换验收用：UART session 已由对端建立，复位会打断 ESP32 当前 transport）。
    void start(const QString& port, quint32 baud, bool noReset = false);
    // TCP Server（M6-A，PC = Server，§二/§九）：监听 bind:port，接受单个 ESP32，
    // 断开后自动 re-accept（§十一）。不修改上层协议行为。
    void startTcp(uint16_t port, const QString& bind = QStringLiteral("0.0.0.0"));

    // M6-D：正式运行时 Transport 切换（GUI Apply → ConnectionManager → 本方法）。
    // 语义（docs/DESIGN.md W 节）：
    //   1) validateTransportConfig 本地校验（非法配置返回 false，不做任何切换）；
    //   2) stop() 旧会话（join Worker 线程 → 会话重置：seq/ACK/decoder/
    //      FrameAssembler/PARTIAL base 随新 ProtocolEndpoint 重建而清零）；
    //   3) 清输入队列 + 待处理 RX 字节 + 会话级统计（input reset / stale 清理）；
    //   4) 按目标 kind 启动新 Transport（UART：可 uartNoReset 跳过复位脉冲；
    //      TCP：startTcp 监听 → accept → HELLO → FULL resync）。
    // 真实 open 失败（COM 占用 / bind 失败）不在此返回：由 Worker 状态回调 +
    // 既有 reconnect policy（UART 1.5s / TCP re-accept）异步呈现。
    bool switchTransport(const TransportConfig& cfg);
    // 当前（或目标）Transport 配置快照（GUI 状态栏显示；GUI 线程读取安全）。
    TransportConfig currentConfig() const;
    void stop();  // 置停止标志 + cancel 唤醒 acceptOne + join Worker 线程；join 后无任何 signal
    // M6-D：线程是否真正在跑（runLoop 入口置 true、收尾置 false）。
    // 注意：std::thread::joinable() 在线程函数返回后仍为 true，不能单独用于
    // “Worker 是否存活”（TCP bind 失败/收尾后线程已死但 joinable）。
    bool isRunning() const;

    // M3：GUI 线程调用，把 InputEvent 交给 Worker 线程异步发送（fire-and-forget）。
    // 内部只入队（互斥保护），Worker 在 pumpLoop 中 drain 并经
    // input_codec + ProtocolEndpoint::sendMessage 送出；未连接时丢弃并计数。
    void sendInput(const espview::input::InputEvent& ev);

signals:
    void frameReady(const DisplayFrame& frame);  // 已提交帧（queued 投递 GUI）
    void statusChanged(WorkerStatus status, const QString& text);
    void statsChanged(const WorkerStats& stats);
    // M4：诊断条目（timestampMs, severity=proto::Severity, source, message）。
    // 纯基本类型信号，无需额外 metatype 注册；GUI 侧维护最近 50 条。
    void diagAdded(quint64 timestampMs, int severity, const QString& source,
                   const QString& message);

private:
    void runLoop();  // Worker 线程入口
    void runLoopUart();  // UART：open → pump → 断开重连（1.5s）
    void runLoopTcp();   // TCP Server：bindListen → acceptOne → pump → re-accept
    void pumpLoop(uint64_t openAtMs);
    void drainAndTick();
    void drainInputQueue();  // 仅 Worker 线程：编码并发送队列中的 InputEvent
    void resetSessionCounters();  // M6-D：切换后清会话级统计（仅 stop() join 后调用）
    void clearInputQueue();       // M6-D：清空 GUI→Worker 输入队列（切换前调用）
    void emitStatus(WorkerStatus status, const QString& text);
    void emitStats();
    void pushDiag(proto::Severity severity, std::string source, std::string message);
    bool waitMs(uint64_t ms);  // 可被 stop 中断的睡眠
    uint64_t nowMs() const;

    // ---- 仅 Worker 线程访问（runLoop/pumpLoop 及其回调）----
    std::unique_ptr<proto::ProtocolEndpoint> ep_;
    std::unique_ptr<HostUartTransport> uart_;  // kind_==kUart 时持有
    std::unique_ptr<HostTcpTransport> tcp_;    // kind_==kTcp 时持有
    TcpListener listener_;                     // TCP Server listener（worker 线程）
    IPcTransport* transport_ = nullptr;        // 指向当前激活 Transport（worker 线程）
    DisplayFrame currentFrame_;   // onFrameBegin/onFrameRect/onFrameCommit 累积
    uint64_t committedFrames_ = 0;
    uint64_t discardedFrames_ = 0;
    std::array<uint64_t, 11> discardByReason_{};
    uint8_t lastDiscardReason_ = 0;
    uint64_t lastDiscardTimeMs_ = 0;
    uint16_t peerWidth_ = 0;
    uint16_t peerHeight_ = 0;
    uint8_t peerPixelFormat_ = 0;
    uint16_t lastFrameId_ = 0;
    uint8_t lastFrameType_ = 0;
    bool sawAnyFrame_ = false;
    // M4：吞吐/帧率窗口（最近一次 emitStats 的快照）
    uint64_t statsWindowMs_ = 0;
    uint64_t lastRxBytes_ = 0;
    uint64_t lastTxBytes_ = 0;
    uint64_t framesAtLastStats_ = 0;
    uint64_t reconnectCount_ = 0;
    proto::DiagnosticsRing diag_;

    // ---- Transport RX 线程 → Worker 线程 ----
    std::mutex rxMutex_;
    std::vector<uint8_t> rxBuf_;
    std::mutex tstateMutex_;
    IPcTransport::State transportState_ = IPcTransport::State::Disconnected;

    // ---- 跨线程（GUI 可调用 start/stop / sendInput）----
    std::atomic<bool> stop_{false};
    std::atomic<bool> threadAlive_{false};  // M6-D：runLoop 存活标志
    std::thread thread_;
    TransportKind kind_ = TransportKind::kUart;
    QString port_;
    quint32 baud_ = 115200;
    bool noReset_ = false;  // M6-C test-only：跳过 UART 复位脉冲
    uint16_t tcpPort_ = 8765;
    QString tcpBind_ = QStringLiteral("0.0.0.0");

    // 输入队列（GUI → Worker；互斥保护）。
    std::mutex inputMutex_;
    std::vector<espview::input::InputEvent> inputQueue_;
    uint64_t inputSent_ = 0;
    uint64_t inputDropped_ = 0;
};

}  // namespace pc
}  // namespace espview

Q_DECLARE_METATYPE(espview::pc::WorkerStatus)
Q_DECLARE_METATYPE(espview::pc::WorkerStats)
