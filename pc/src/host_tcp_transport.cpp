// ESPView M6-A — HostTcpTransport / TcpListener 实现（见 host_tcp_transport.h）。

#include "host_tcp_transport.h"

#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")  // MSVC：CMake 链接；MinGW 用 target_link_libraries(ws2_32)
#endif

namespace espview {
namespace pc {

namespace {

// WinSock 一次性初始化（引用计数；线程安全）。
std::once_flag g_wsaOnce;
bool g_wsaOk = false;

void wsaInitOnce() {
    WSADATA wsa;
    g_wsaOk = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

// 记录最近一次 WinSock 错误（§二十七：bind 失败报告 WinSock error）。
std::string winsockErrorText(const char* what) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: WSAGetLastError=%d", what, WSAGetLastError());
    return buf;
}

}  // namespace

void ensureWsaStartup() {
    std::call_once(g_wsaOnce, wsaInitOnce);
}

// ======================= HostTcpTransport =======================

HostTcpTransport::~HostTcpTransport() {
    close();
}

bool HostTcpTransport::attach(SOCKET sock, const Config& cfg) {
    ensureWsaStartup();
    if (sock == INVALID_SOCKET) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (connected_ || attached_) {
            closesocket(sock);
            return false;
        }
        cfg_ = cfg;
        mtu_ = 20 + 4096;
        sock_ = sock;
        attached_ = true;
        connected_ = true;
        stop_ = false;

        // 发送超时 + NODELAY（小包/控制消息低延迟）。
        DWORD tv = cfg_.send_timeout_ms;
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
                   sizeof(tv));
        BOOL one = TRUE;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                   sizeof(one));

        // M6-C：记录对端地址（GUI 状态栏 "Peer: ip:port"）。
        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        char peerIp[INET_ADDRSTRLEN] = {};
        if (getpeername(sock_, reinterpret_cast<sockaddr*>(&peer), &peerLen) == 0) {
            inet_ntop(AF_INET, &peer.sin_addr, peerIp, sizeof(peerIp));
            peerAddr_ = std::string(peerIp) + ":" + std::to_string(ntohs(peer.sin_port));
        }

        rxThread_ = std::thread(&HostTcpTransport::rxLoop, this);
    }
    // setState 会再锁 mutex_（回调路径），必须在锁外调用。
    setState(State::Connected);
    return true;
}

bool HostTcpTransport::open(const PcTransportConfig& cfg) {
    const auto& tcfg = static_cast<const Config&>(cfg);
    ensureWsaStartup();
    if (!g_wsaOk) {
        setState(State::Error);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (connected_ || attached_) {
            return false;  // 已在运行：明确失败
        }
    }
    cfg_ = tcfg;
    mtu_ = 20 + 4096;
    stop_ = false;

    // 解析地址（IPv4/IPv6 均可；默认 127.0.0.1 回环）。
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* result = nullptr;
    const std::string portStr = std::to_string(cfg_.port);
    const int gai = getaddrinfo(cfg_.host.c_str(), portStr.c_str(), &hints, &result);
    if (gai != 0) {
        setState(State::Error);
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        // 非阻塞 connect + select 超时。
        u_long nonblock = 1;
        ioctlsocket(sock, FIONBIO, &nonblock);
        const int rc = connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }
        // 等待可写。
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        timeval tv;
        tv.tv_sec = static_cast<long>(cfg_.connect_timeout_ms / 1000u);
        tv.tv_usec = static_cast<long>((cfg_.connect_timeout_ms % 1000u) * 1000u);
        const int sel = select(0, nullptr, &wfds, nullptr, &tv);
        if (sel <= 0) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }
        int soerr = 0;
        int slen = sizeof(soerr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr),
                       &slen) < 0 ||
            soerr != 0) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }
        break;  // 连接成功
    }
    freeaddrinfo(result);
    if (sock == INVALID_SOCKET) {
        setState(State::Error);
        return false;
    }

    // 恢复阻塞模式 + 超时 + NODELAY。
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);
    DWORD snd = cfg_.send_timeout_ms;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));
    BOOL one = TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (connected_ || attached_) {  // 与 close()/重复 open 竞态：防御
            closesocket(sock);
            return false;
        }
        sock_ = sock;
        connected_ = true;
        rxThread_ = std::thread(&HostTcpTransport::rxLoop, this);
    }
    // setState 会再锁 mutex_（回调路径），必须在锁外调用。
    setState(State::Connected);
    return true;
}

void HostTcpTransport::close() {
    std::thread rx;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // 幂等；但 RX 线程可能在运行（对端断开后 connected_ 已 false）——
        // 只要 rxThread_ 可 join 就必须 stop + join，不能直接 return。
        const bool wasActive = connected_ || attached_ || rxThread_.joinable();
        if (!wasActive) {
            return;
        }
        stop_ = true;
        connected_ = false;
        attached_ = false;
        rx = std::move(rxThread_);
        closeSocketLocked();  // shutdown 唤醒 RX select
    }
    if (rx.joinable()) {
        rx.join();
    }
    setState(State::Disconnected);
}

bool HostTcpTransport::isConnected() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return connected_;
}

bool HostTcpTransport::send(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }
    bool fatal = false;
    size_t sent = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!connected_ || sock_ == INVALID_SOCKET) {
            return false;
        }
        if (len > mtu_) {
            return false;
        }
        // sendAll（§二十三）：循环处理 short write。
        // 注意：::send（全局 winsock send）—— 成员函数名 send 会遮蔽同名全局函数。
        while (sent < len) {
            const int n = ::send(sock_, reinterpret_cast<const char*>(data + sent),
                                 static_cast<int>(len - sent), 0);
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (n == 0) {
                fatal = true;  // 对端关闭：致命（与 recv==0 同类）
                break;
            }
            const int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
                break;  // SO_SNDTIMEO 到期（非致命；保持现有超时语义）
            }
            fatal = true;  // ECONNRESET/EPIPE 等：致命
            break;
        }
        txBytes_.fetch_add(sent, std::memory_order_relaxed);
        if (fatal) {
            // 与 recv 路径一致（M6-D §十七）：锁内置断开标志；锁外 setState
            // 通知 Worker，否则 pumpLoop 永不返回（CS-1 回归）。
            connected_ = false;
        }
    }
    if (fatal) {
        setState(State::Disconnected);
        return false;
    }
    if (sent != len) {
        return false;
    }
    return true;
}

void HostTcpTransport::setDataCallback(DataCallback cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    dataCb_ = std::move(cb);
}

void HostTcpTransport::setStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> lk(mutex_);
    stateCb_ = std::move(cb);
}

size_t HostTcpTransport::mtu() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return mtu_;
}

void HostTcpTransport::setState(State s) {
    StateCallback cb;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (state_ == s) {
            return;
        }
        state_ = s;
        cb = stateCb_;
    }
    if (cb) {
        cb(s);
    }
}

void HostTcpTransport::closeSocketLocked() {
    if (sock_ != INVALID_SOCKET) {
        shutdown(sock_, SD_BOTH);
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    peerAddr_.clear();

}

void HostTcpTransport::rxLoop() {
    std::vector<uint8_t> buf(cfg_.rx_buf > 0 ? cfg_.rx_buf : 4096);
    while (!stop_.load()) {
        SOCKET fd = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!connected_ || sock_ == INVALID_SOCKET) {
                fd = INVALID_SOCKET;
            } else {
                fd = sock_;
            }
        }
        if (fd == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv;
        tv.tv_sec = static_cast<long>(cfg_.rx_timeout_ms / 1000u);
        tv.tv_usec = static_cast<long>((cfg_.rx_timeout_ms % 1000u) * 1000u);
        const int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (WSAGetLastError() == WSAEINTR) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(mutex_);
                connected_ = false;
            }
            setState(State::Disconnected);  // M6-C：传输级断开必须通知 Worker（否则 pumpLoop 永不返回）
            continue;
        }
        if (sel == 0) {
            continue;
        }

        const int n = recv(fd, reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0);
        if (n > 0) {
            rxBytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            DataCallback cb;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                cb = dataCb_;
            }
            if (cb) {
                cb(buf.data(), static_cast<size_t>(n));
            }
            continue;
        }
        // n==0：对端关闭；n<0：错误。置断开并通知 Worker（上层触发重连/重新 accept）。
        {
            std::lock_guard<std::mutex> lk(mutex_);
            connected_ = false;
        }
        setState(State::Disconnected);
    }
    // 通知 close()：RX 线程已退出（close 在 join 后清理）。
}

// ======================= TcpListener =======================

TcpListener::~TcpListener() {
    close();
}

bool TcpListener::bindListen(const Config& cfg) {
    ensureWsaStartup();
    if (!g_wsaOk) {
        lastError_ = "WSAStartup failed";
        return false;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    if (listenSock_ != INVALID_SOCKET) {
        return false;  // 已在监听
    }
    cfg_ = cfg;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo* result = nullptr;
    const std::string portStr = std::to_string(cfg_.port);
    const int gai = getaddrinfo(cfg_.bind.c_str(), portStr.c_str(), &hints, &result);
    if (gai != 0) {
        lastError_ = winsockErrorText("getaddrinfo");
        return false;
    }

    SOCKET ls = INVALID_SOCKET;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (ls == INVALID_SOCKET) {
            continue;
        }
        BOOL reuse = TRUE;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                   sizeof(reuse));
        if (bind(ls, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == SOCKET_ERROR) {
            lastError_ = winsockErrorText("bind");
            closesocket(ls);
            ls = INVALID_SOCKET;
            continue;
        }
        if (listen(ls, 1) == SOCKET_ERROR) {
            lastError_ = winsockErrorText("listen");
            closesocket(ls);
            ls = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(result);
    if (ls == INVALID_SOCKET) {
        return false;
    }

    listenSock_ = ls;
    closed_ = false;
    acceptedOnce_ = false;
    // 报告 bind 地址与端口（§二十七：启动时明确报告，失败报 WinSock error）。
    sockaddr_in local{};
    int slen = sizeof(local);
    getsockname(ls, reinterpret_cast<sockaddr*>(&local), &slen);
    boundPort_ = ntohs(local.sin_port);
    lastError_.clear();
    return true;
}

bool TcpListener::acceptOne(HostTcpTransport& out) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (listenSock_ == INVALID_SOCKET) {
            return false;
        }
        if (acceptedOnce_) {
            if (out.isConnected()) {
                return false;  // 已有活跃客户端（§九 BUSY）
            }
            // 前一个客户端已断开（§十一：PC 等待 → 接受新 ESP32 连接）
            acceptedOnce_ = false;
        }
    }
    while (!closed_.load()) {
        SOCKET ls;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            ls = listenSock_;
        }
        if (ls == INVALID_SOCKET) {
            return false;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        timeval tv;
        tv.tv_sec = static_cast<long>(cfg_.accept_poll_ms / 1000u);
        tv.tv_usec = static_cast<long>((cfg_.accept_poll_ms % 1000u) * 1000u);
        const int sel = select(0, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            return false;
        }
        if (sel == 0) {
            continue;  // 超时：检查 closed_/cancel
        }
        SOCKET client = accept(ls, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (closed_.load() || listenSock_ == INVALID_SOCKET) {
                closesocket(client);
                return false;
            }
            acceptedOnce_ = true;
        }
        HostTcpTransport::Config ccfg;
        ccfg.host = "accepted";
        ccfg.port = cfg_.port;
        ccfg.rx_timeout_ms = 100;
        ccfg.send_timeout_ms = 5000;
        // attach() 失败时由 attach() 负责 closesocket（attach 接管 socket 所有权；
        // 此处再 close 会 double close，CS-2）。
        if (!out.attach(client, ccfg)) {
            return false;
        }
        return true;
    }
    return false;
}

void TcpListener::close() {
    SOCKET ls;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ls = listenSock_;
        listenSock_ = INVALID_SOCKET;
        closed_ = true;
    }
    if (ls != INVALID_SOCKET) {
        closesocket(ls);
    }
}

void TcpListener::cancel() {
    // 只置标志（atomic），不碰 socket：由 worker 线程在 acceptOne 返回后执行
    // close()，避免跨线程 closesocket/select 竞态。
    closed_.store(true);
}

bool TcpListener::isListening() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return listenSock_ != INVALID_SOCKET && !closed_.load();
}


std::string HostTcpTransport::peerAddress() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return peerAddr_;
}

}  // namespace pc
}  // namespace espview

