// ESPView M2 — ConnectionManager：Worker 生命周期 + 串口连接管理（GUI 侧门面）。
//
// 规范来源：docs/DESIGN.md M2 节 + spec §15（ConnectionManager）。
//
// 职责：
//   - 默认 COM3 @ 115200（可经 start(port, baud) / CLI --port/--baud 覆盖）；
//   - 启动/停止 SerialWorker（含其内部 Worker 线程，干净 join，无句柄泄漏）；
//   - 把 Worker 的 queued signals 原样转发给 GUI（frameReady / statusChanged /
//     statsChanged）。
//
// 断线重连策略在 SerialWorker 内部实现（Transport 断开/错误 → 1.5s 后重开，
// 复位 ESP32 → 等 boot HELLO → 等新 FULL 重同步），ConnectionManager 只负责
// 生命周期与信号转发，不接触协议 / 串口细节。

#pragma once

#include <QObject>
#include <QString>

#include "display_frame.h"
#include "input_event.h"
#include "serial_worker.h"

namespace espview {
namespace pc {

class ConnectionManager : public QObject {
    Q_OBJECT
public:
    explicit ConnectionManager(QObject* parent = nullptr);
    ~ConnectionManager() override;  // 内部 stop()：join Worker 线程后再析构

    // 启动连接（默认调用方可传 COM3 / 115200）。幂等：先 stop 旧会话。
    // noReset：M6-C test-only（运行时 transport 切换验收：跳过复位脉冲）。
    void start(const QString& port = QStringLiteral("COM3"), quint32 baud = 115200,
               bool noReset = false);
    // M6-A：TCP Server 模式（PC = Server，§二/§九）。幂等：先 stop 旧会话。
    void startTcp(uint16_t port, const QString& bind = QStringLiteral("0.0.0.0"));
    // M6-D：正式运行时 Transport 切换（GUI Apply → 本方法）。
    // 语义：validateTransportConfig 本地校验 → 失败返回 false（不切换）；
    // 成功则 stop 旧会话（会话重置：seq/ACK/decoder/FrameAssembler/PARTIAL
    // base/Input 队列）→ 按目标 kind 启动新 Transport → HELLO → FULL resync。
    // 真实 open 失败（COM 占用 / bind 失败）由 Worker 状态回调 + reconnect
    // policy 异步呈现，不在本方法返回。
    bool switchTransport(const TransportConfig& cfg);
    // 当前（或目标）Transport 配置快照（GUI 状态栏显示）。
    TransportConfig currentConfig() const { return worker_.currentConfig(); }
    TransportKind currentKind() const { return currentConfig().kind; }
    // 停止并 join（阻塞直到 Worker 线程退出；退出后不再有 signal 到达）。
    void stop();
    bool isRunning() const;

    // M3：GUI 线程调用，转发给 SerialWorker 的输入队列（线程安全）。
    void sendInput(const espview::input::InputEvent& ev);

signals:
    void frameReady(const DisplayFrame& frame);
    void statusChanged(WorkerStatus status, const QString& text);
    void statsChanged(const WorkerStats& stats);
    // M4：诊断条目转发（timestampMs, severity, source, message）。
    void diagAdded(quint64 timestampMs, int severity, const QString& source,
                   const QString& message);

private:
    SerialWorker worker_;
};

}  // namespace pc
}  // namespace espview
