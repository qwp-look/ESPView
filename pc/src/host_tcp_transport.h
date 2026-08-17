// ESPView M6-A / M8-A3 — HostTcpTransport（PC 侧 TCP 客户端）+ TcpListener（TCP 服务端）。
//
// 规范来源：M6-A 任务书 §二/§六/§七/§九/§二十二/§二十三/§二十四/§二十七/§二十八
//   + M8-A3（§三十五：直接实现 shared/transport espview::transport::ITransport，
//   删除 IPcTransport/pc_transport.h）。
//
// HostTcpTransport：与 HostUartTransport 同构的 TCP 字节管道（§六）。TCP 是
//   byte stream（§七）：一次 recv() 可能得到半个/多个 Packet，本类原样转发
//   dataCallback，不做协议解析。send() 实现 sendAll（§二十三）：循环处理
//   short write，直到全部发送或 error/timeout/disconnect。
//   M8-A3：open() 在已 attach（服务端 accept 路径）时幂等返回 true ——
//   SerialWorker 先 acceptOne 再经 TransportManager::adopt() 建立发送门；
//   客户端路径（tcp_transport_test）仍走真实 connect。
//
// TcpListener：PC = TCP Server（§二），socket/bind/listen/accept；一次只接受
//   一个 ESP32（§九）：已接受一个连接后，listener 自动关闭 accept 路径，
//   后续连接被拒绝（第二个客户端收到 ECONNREFUSED，明确 BUSY 语义）。
//   保持独立类（§二十：HostTcpTransport 不兼任 Listener；reconnect 归 SerialWorker）。
//
// 线程模型（§二十二）：
//   - RX worker 线程：select(rx_timeout) → recv → dataCallback（仅调用期间有效）；
//   - send()：任意调用线程（互斥 + SO_SNDTIMEO），sendAll 处理 short write；
//   - close()：置停止标志 → 唤醒 RX worker（select 超时）→ join → closesocket；
//   - 所有 socket 操作互斥保护，无 double close / use-after-close。
//
// 平台：Windows + WinSock2（WSAStartup 一次性初始化，引用计数式线程安全）。
//   bind 失败时报告 WinSock error（§二十七），不自动改防火墙。

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "transport.h"  // shared/transport：唯一 canonical ITransport

namespace espview {
namespace pc {

// ---- 客户端 ----
class HostTcpTransport : public espview::transport::ITransport {
public:
    using State = espview::transport::ITransport::State;
    using DataCallback = espview::transport::ITransport::DataCallback;
    using StateCallback = espview::transport::ITransport::StateCallback;

    struct Config {
        std::string host = "127.0.0.1";
        uint16_t port = 8765;
        uint32_t connect_timeout_ms = 5000;  // 非阻塞 connect + select 超时
        uint32_t rx_timeout_ms = 100;        // RX select 周期（close 及时性）
        uint32_t send_timeout_ms = 5000;     // 单次 sendAll 总预算
        size_t rx_buf = 4096;                // RX chunk
    };

    HostTcpTransport();  // 默认配置（127.0.0.1:8765）
    explicit HostTcpTransport(Config cfg);
    ~HostTcpTransport() override;

    HostTcpTransport(const HostTcpTransport&) = delete;
    HostTcpTransport& operator=(const HostTcpTransport&) = delete;

    // 客户端：连接 cfg_.host:port。服务端：已 attach（acceptOne）后幂等返回 true。
    // 已连接且未 attach 时返回 false（明确失败）。
    bool open() override;
    void close() override;
    bool isConnected() const override;
    // kBackpressure = SO_SNDTIMEO 到期（would-block）；kNotConnected = 未连接；
    // kError = 参数/致命错误（对端关闭等，同时置 Disconnected）。
    espview::transport::SendStatus send(const uint8_t* data, size_t len) override;
    const espview::transport::TransportCapabilities& capabilities() const override {
        return caps_;
    }
    void setDataCallback(DataCallback cb) override;
    void setStateCallback(StateCallback cb) override;
    uint64_t rxBytes() const override { return rxBytes_.load(); }
    uint64_t txBytes() const override { return txBytes_.load(); }

    // M6-C：当前对端地址（attach 模式 = accept 的 peer；未连接 = 空串）。
    std::string peerAddress() const;

    // 供 TcpListener/测试：包装一个已 accept 的 socket（不发起 connect）。
    // 接管 socket 所有权：拒绝时由本方法 closesocket（调用方不得再次 close，CS-2）。
    bool attach(SOCKET sock, const Config& cfg);

private:
    void rxLoop();
    void setState(State s);
    void closeSocketLocked();  // 已持 mutex_：shutdown+closesocket

    mutable std::mutex mutex_;          // 保护 sock_/connected_/回调
    SOCKET sock_ = INVALID_SOCKET;
    bool connected_ = false;
    bool attached_ = false;             // attach 模式（服务端 accept）
    std::string peerAddr_;              // 对端 "ip:port"（attach 时 getpeername）
    Config cfg_;
    espview::transport::TransportCapabilities caps_;

    std::thread rxThread_;
    std::atomic<bool> stop_{false};
    DataCallback dataCb_;
    StateCallback stateCb_;
    State state_ = State::kDisconnected;
    std::atomic<uint64_t> rxBytes_{0};
    std::atomic<uint64_t> txBytes_{0};
};

// ---- 服务端（单客户端；§九）----
class TcpListener {
public:
    struct Config {
        std::string bind = "0.0.0.0";
        uint16_t port = 8765;
        uint32_t accept_poll_ms = 200;  // select 周期（可被 close 中断）
    };

    TcpListener() = default;
    ~TcpListener();

    // socket/bind/listen。失败返回 false 并设置 lastError（WinSock error，§二十七）。
    bool bindListen(const Config& cfg);
    // 阻塞接受一个客户端（select 轮询，可被 close()/cancel() 中断）。成功包装到 out。
    // 已有活跃客户端（out.isConnected() 为 true）时返回 false（BUSY，§九）；
    // 前一个客户端断开后自动允许重新 accept（§十一 PC 重连路径）。
    bool acceptOne(HostTcpTransport& out);
    // 关闭 listener；阻塞中的 acceptOne 返回 false。线程安全。
    void close();
    // 仅置取消标志（不碰 socket；由 worker 线程随后 close()），供 stop() 唤醒
    // 阻塞在 acceptOne 的线程，避免跨线程 closesocket/select 竞态。
    void cancel();
    bool isListening() const;
    uint16_t boundPort() const { return boundPort_; }
    std::string lastError() const { return lastError_; }

private:
    mutable std::mutex mutex_;       // 保护 listenSock_/acceptedOnce_/lastError_
    SOCKET listenSock_ = INVALID_SOCKET;
    std::atomic<bool> closed_{true};
    bool acceptedOnce_ = false;
    std::string lastError_;
    uint16_t boundPort_ = 0;
    Config cfg_{};
};

// ---- WinSock 一次性初始化（引用计数；线程安全）----
void ensureWsaStartup();

}  // namespace pc
}  // namespace espview
