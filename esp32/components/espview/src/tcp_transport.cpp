// ESPView M6-A — TcpTransport 实现（见 tcp_transport.hpp）。

#include "espview/tcp_transport.hpp"

#include <cstring>
#include <utility>

#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"

namespace espview {

namespace {

const char* kTag = "espview_tcp";

constexpr size_t kLinkTaskStackWords = 4096;
constexpr size_t kRxTaskStackWords = 4096;
constexpr TickType_t kMutexTakeTicks = pdMS_TO_TICKS(500);
constexpr TickType_t kTaskJoinTicks = pdMS_TO_TICKS(1500);
constexpr UBaseType_t kLinkTaskPriority = 6;
constexpr UBaseType_t kRxTaskPriority = 7;

// 简单 RAII 锁（FreeRTOS mutex）。
class ScopedLock {
public:
    explicit ScopedLock(SemaphoreHandle_t m) : m_(m) {
        if (m_ != nullptr) {
            xSemaphoreTake(m_, portMAX_DELAY);
        }
    }
    ~ScopedLock() {
        if (m_ != nullptr) {
            xSemaphoreGive(m_);
        }
    }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    SemaphoreHandle_t m_;
};

const char* stateName(ITransport::State s) {
    switch (s) {
        case ITransport::State::Disconnected:
            return "Disconnected";
        case ITransport::State::Connecting:
            return "Connecting";
        case ITransport::State::Connected:
            return "Connected";
        case ITransport::State::Error:
            return "Error";
    }
    return "?";
}

}  // namespace

TcpTransport::~TcpTransport() {
    close();
    if (sockMutex_ != nullptr) {
        vSemaphoreDelete(sockMutex_);
    }
    if (stateMutex_ != nullptr) {
        vSemaphoreDelete(stateMutex_);
    }
    if (linkWake_ != nullptr) {
        vSemaphoreDelete(linkWake_);
    }
    if (taskExit_ != nullptr) {
        vSemaphoreDelete(taskExit_);
    }
}

esp_err_t TcpTransport::open(const TransportConfig& cfg) {
    const auto* tcfg = static_cast<const TcpTransportConfig*>(&cfg);
    if (tcfg->server_ip == nullptr || tcfg->server_ip[0] == '\0' || tcfg->server_port == 0 ||
        tcfg->rx_buf < 512 || tcfg->rx_buf > 16384) {
        setState(State::Error);
        return ESP_ERR_INVALID_ARG;
    }

    if (sockMutex_ == nullptr) {
        sockMutex_ = xSemaphoreCreateMutex();
    }
    if (stateMutex_ == nullptr) {
        stateMutex_ = xSemaphoreCreateMutex();
    }
    if (linkWake_ == nullptr) {
        linkWake_ = xSemaphoreCreateBinary();
    }
    if (taskExit_ == nullptr) {
        taskExit_ = xSemaphoreCreateCounting(4, 0);
    }
    if (sockMutex_ == nullptr || stateMutex_ == nullptr || linkWake_ == nullptr ||
        taskExit_ == nullptr) {
        setState(State::Error);
        return ESP_ERR_NO_MEM;
    }

    cfg_ = *tcfg;
    mtu_ = 20 + 4096;  // Packet Header(20) + 最大 payload(4096)

    {
        ScopedLock lock(stateMutex_);
        if (running_) {
            return ESP_ERR_INVALID_STATE;  // 重复 open 明确报错
        }
        running_ = true;
    }

    setState(State::Connecting);

    // Wi-Fi STA 初始化（凭据来自 Kconfig/未跟踪 sdkconfig；失败即 Error）。
    const esp_err_t werr = wifi_.init(cfg_.wifi);
    if (werr != ESP_OK) {
        setState(State::Error);
        {
            ScopedLock lock(stateMutex_);
            running_ = false;  // CS-5：open 失败完全回滚（close() 幂等兜底）
        }
        wifi_.deinit();  // 幂等：确保半初始化的 Wi-Fi 也释放
        return werr;
    }

    const BaseType_t terr = xTaskCreate(linkTaskEntry, "espview_tcp_link", kLinkTaskStackWords,
                                        this, kLinkTaskPriority, &linkTask_);
    if (terr != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate(link) failed");
        setState(State::Error);
        {
            ScopedLock lock(stateMutex_);
            running_ = false;  // CS-5：open 失败完全回滚
        }
        wifi_.deinit();  // 回滚已初始化的 Wi-Fi（TransportManager close() 兜底安全）
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(kTag, "open: target=%s:%u timeout=%u delay=%u", cfg_.server_ip,
             static_cast<unsigned>(cfg_.server_port),
             static_cast<unsigned>(cfg_.connect_timeout_ms),
             static_cast<unsigned>(cfg_.reconnect_delay_ms));
    return ESP_OK;
}

void TcpTransport::close() {
    if (sockMutex_ == nullptr || stateMutex_ == nullptr) {
        return;  // 从未 open
    }
    TaskHandle_t link = nullptr;
    TaskHandle_t rx = nullptr;
    {
        ScopedLock lock(stateMutex_);
        if (!running_) {
            return;  // 幂等
        }
        running_ = false;
        link = linkTask_;
        rx = rxTask_;
    }

    // 唤醒 link 任务（可能在等待通知/退避中）→ 等待 link 退出；link 退出前会
    // shutdown+close socket（RX select 立即返回）并唤醒 RX 任务退出。
    if (linkWake_ != nullptr) {
        xSemaphoreGive(linkWake_);
    }
    bool linkGone = false;
    if (link != nullptr && taskExit_ != nullptr) {
        // link 任务可能在 wifi_.waitForIp()（最长 connect_timeout）中：
        // join 预算必须覆盖 connect_timeout + reconnect_delay，否则 close() 与
        // wifi_.deinit() 会与仍在跑的任务竞态。
        const TickType_t linkJoinTicks =
            pdMS_TO_TICKS(cfg_.connect_timeout_ms + cfg_.reconnect_delay_ms + 2000u);
        linkGone = xSemaphoreTake(taskExit_, linkJoinTicks) == pdTRUE;
        if (!linkGone) {
            ESP_LOGW(kTag, "link task did not exit in time");
        }
    }

    // link 可能在退出前启动了 RX：重新读取并等待 RX 退出。
    {
        ScopedLock lock(stateMutex_);
        rx = rxTask_;
    }
    bool rxGone = false;
    if (rx != nullptr && taskExit_ != nullptr) {
        rxGone = xSemaphoreTake(taskExit_, kTaskJoinTicks) == pdTRUE;
        if (!rxGone) {
            ESP_LOGW(kTag, "RX task did not exit in time");
        }
    }

    {
        ScopedLock lock(stateMutex_);
        linkTask_ = nullptr;
        rxTask_ = nullptr;
    }
    wifi_.deinit();
    setState(State::Disconnected);
    ESP_LOGI(kTag, "closed");
}

bool TcpTransport::isConnected() const {
    ScopedLock lock(sockMutex_);
    return connected_;
}

esp_err_t TcpTransport::send(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sockMutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;  // 从未 open
    }
    if (xSemaphoreTake(sockMutex_, kMutexTakeTicks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!connected_ || sock_ < 0) {
        xSemaphoreGive(sockMutex_);
        return ESP_ERR_INVALID_STATE;
    }
    if (len > mtu_) {
        xSemaphoreGive(sockMutex_);
        return ESP_ERR_INVALID_ARG;
    }

    // sendAll（§二十三）：循环处理 short write，直到全部发送或 error/timeout。
    size_t sent = 0;
    esp_err_t result = ESP_OK;
    bool fatal = false;
    while (sent < len) {
        const int n = lwip_send(sock_, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {
            fatal = true;  // 对端关闭
            break;
        }
        const int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
            // SO_SNDTIMEO 到期/被中断：视为背压超时（可重试；协议层整帧丢弃）。
            result = ESP_ERR_TIMEOUT;
            break;
        }
        fatal = true;  // ECONNRESET/EPIPE/ENOTCONN 等
        break;
    }
    txBytes_.fetch_add(sent, std::memory_order_relaxed);

    if (fatal) {
        // 致命错误：断开并唤醒 link 任务立即重连（协议层随后按会话断开处理）。
        connected_ = false;
        result = ESP_FAIL;
    }
    xSemaphoreGive(sockMutex_);

    if (fatal && linkWake_ != nullptr) {
        xSemaphoreGive(linkWake_);
    }
    return result;
}

void TcpTransport::setDataCallback(DataCallback cb) {
    ScopedLock lock(stateMutex_);
    dataCb_ = std::move(cb);
}

void TcpTransport::setStateCallback(StateCallback cb) {
    ScopedLock lock(stateMutex_);
    stateCb_ = std::move(cb);
}

size_t TcpTransport::mtu() const {
    ScopedLock lock(stateMutex_);
    return mtu_;
}

void TcpTransport::setState(State s) {
    StateCallback cb;
    {
        ScopedLock lock(stateMutex_);
        if (state_ == s) {
            return;
        }
        state_ = s;
        cb = stateCb_;
    }
    ESP_LOGI(kTag, "state -> %s", stateName(s));
    if (cb) {
        cb(s);
    }
}

void TcpTransport::linkTaskEntry(void* arg) {
    auto* self = static_cast<TcpTransport*>(arg);
    self->linkLoop();
}

void TcpTransport::linkLoop() {
    bool wasConnected = false;
    while (true) {
        {
            ScopedLock lock(stateMutex_);
            if (!running_) {
                break;
            }
        }

        // ---- 阶段 1：Wi-Fi 连接 + GOT_IP ----
        setState(State::Connecting);
        if (!wifi_.hasIp()) {
            wifi_.startConnect();
            const bool got = wifi_.waitForIp(cfg_.connect_timeout_ms);
            {
                ScopedLock lock(stateMutex_);
                if (!running_) {
                    break;
                }
            }
            if (!got) {
                ESP_LOGW(kTag, "wifi wait for IP timed out; retrying");
                xSemaphoreTake(linkWake_, pdMS_TO_TICKS(cfg_.reconnect_delay_ms));
                continue;
            }
        }

        // ---- 阶段 2：TCP connect ----
        int fd = -1;
        if (!connectOnce(fd)) {
            ++reconnectCount_;
            xSemaphoreTake(linkWake_, pdMS_TO_TICKS(cfg_.reconnect_delay_ms));
            continue;
        }
        {
            ScopedLock lock(stateMutex_);
            if (!running_) {
                // close() 已请求：丢弃刚建立的连接。
                lwip_shutdown(fd, SHUT_RDWR);
                lwip_close(fd);
                break;
            }
        }
        {
            ScopedLock lock(sockMutex_);
            sock_ = fd;
            connected_ = true;
        }
        startRxTask();
        setState(State::Connected);
        wasConnected = true;
        ESP_LOGI(kTag, "TCP CONNECTED (reconnect=%u)", static_cast<unsigned>(reconnectCount_));

        // ---- 阶段 3：等待断开/关闭通知（linkWake_ 事件 + 500ms 轮询兜底）----
        bool exitNow = false;
        while (!exitNow) {
            bool stillRunning = false;
            bool stillConnected = false;
            {
                ScopedLock lock(stateMutex_);
                stillRunning = running_;
            }
            {
                ScopedLock lock(sockMutex_);
                stillConnected = connected_;
            }
            if (!stillRunning || !stillConnected) {
                break;
            }
            // 事件唤醒（RX 断开 / send 致命错误 / close）或超时轮询。
            xSemaphoreTake(linkWake_, pdMS_TO_TICKS(500));
        }

        // ---- 断开/关闭：清理 fd（link 独占 fd 生命周期）----
        {
            ScopedLock lock(sockMutex_);
            if (sock_ >= 0) {
                lwip_shutdown(sock_, SHUT_RDWR);
                lwip_close(sock_);
                sock_ = -1;
            }
            connected_ = false;
        }
        {
            ScopedLock lock(stateMutex_);
            exitNow = !running_;
        }
        if (exitNow) {
            break;
        }
        if (wasConnected) {
            setState(State::Disconnected);
            wasConnected = false;
            ++reconnectCount_;
            ESP_LOGW(kTag, "TCP DISCONNECTED; reconnecting in %u ms",
                     static_cast<unsigned>(cfg_.reconnect_delay_ms));
        }
        xSemaphoreTake(linkWake_, pdMS_TO_TICKS(cfg_.reconnect_delay_ms));
    }

    // ---- link 退出路径：关闭 socket，唤醒 RX 退出 ----
    {
        ScopedLock lock(sockMutex_);
        if (sock_ >= 0) {
            lwip_shutdown(sock_, SHUT_RDWR);
            lwip_close(sock_);
            sock_ = -1;
        }
        connected_ = false;
    }
    TaskHandle_t rx = nullptr;
    {
        ScopedLock lock(stateMutex_);
        running_ = false;
        rx = rxTask_;
    }
    if (rx != nullptr) {
        xTaskNotifyGive(rx);
    }
    if (taskExit_ != nullptr) {
        xSemaphoreGive(taskExit_);
    }
    vTaskDelete(nullptr);
}

void TcpTransport::rxTaskEntry(void* arg) {
    auto* self = static_cast<TcpTransport*>(arg);
    self->rxLoop();
}

void TcpTransport::rxLoop() {
    uint8_t* buf = static_cast<uint8_t*>(pvPortMalloc(cfg_.rx_buf));
    if (buf == nullptr) {
        ESP_LOGE(kTag, "rx buffer alloc failed");
        if (taskExit_ != nullptr) {
            xSemaphoreGive(taskExit_);
        }
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        int fd = -1;
        {
            ScopedLock lock(stateMutex_);
            if (!running_) {
                break;
            }
        }
        {
            ScopedLock lock(sockMutex_);
            if (connected_ && sock_ >= 0) {
                fd = sock_;
            }
        }
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(50));  // 未连接：短轮询（close 唤醒不依赖此路径）
            continue;
        }

        // select 超时轮询：close() 后及时退出，同时不忙轮询。
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = static_cast<long>(cfg_.rx_timeout_ms / 1000u);
        tv.tv_usec = static_cast<long>((cfg_.rx_timeout_ms % 1000u) * 1000u);
        const int sel = lwip_select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) {
            if (errno == EINTR) {
                continue;
            }
            // fd 错误（可能已被 close）：交给 link 任务处理。
            if (linkWake_ != nullptr) {
                xSemaphoreGive(linkWake_);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (sel == 0) {
            continue;  // 超时：回到循环检查 running/connected
        }

        const int n = lwip_recv(fd, buf, cfg_.rx_buf, 0);
        if (n > 0) {
            rxBytes_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
            DataCallback cb;
            {
                ScopedLock lock(stateMutex_);
                cb = dataCb_;
            }
            if (cb) {
                // 指针仅在回调期间有效（上层不得缓存）。
                cb(buf, static_cast<size_t>(n));
            }
            continue;
        }
        if (n == 0) {
            ESP_LOGW(kTag, "peer closed connection");
        } else {
            ESP_LOGW(kTag, "recv error errno=%d", errno);
        }
        {
            ScopedLock lock(sockMutex_);
            connected_ = false;  // link 任务看到后清理 fd 并重连
        }
        if (linkWake_ != nullptr) {
            xSemaphoreGive(linkWake_);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    vPortFree(buf);
    if (taskExit_ != nullptr) {
        xSemaphoreGive(taskExit_);
    }
    vTaskDelete(nullptr);
}

bool TcpTransport::connectOnce(int& outFd) {
    const int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(kTag, "socket() failed errno=%d", errno);
        return false;
    }

    // 非阻塞 connect + select 超时。
    int flags = lwip_fcntl(fd, F_GETFL, 0);
    if (lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(kTag, "fcntl(O_NONBLOCK) failed errno=%d", errno);
        lwip_close(fd);
        return false;
    }

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.server_port);
    if (lwip_inet_pton(AF_INET, cfg_.server_ip, &addr.sin_addr) != 1) {
        ESP_LOGE(kTag, "invalid server IP: %s", cfg_.server_ip);
        lwip_close(fd);
        return false;
    }

    const int rc = lwip_connect(fd, reinterpret_cast<const struct sockaddr*>(&addr),
                                sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ESP_LOGW(kTag, "connect() failed errno=%d", errno);
        lwip_close(fd);
        return false;
    }

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv;
    tv.tv_sec = static_cast<long>(cfg_.connect_timeout_ms / 1000u);
    tv.tv_usec = static_cast<long>((cfg_.connect_timeout_ms % 1000u) * 1000u);
    const int sel = lwip_select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (sel <= 0) {
        ESP_LOGW(kTag, "connect timeout (sel=%d errno=%d)", sel, errno);
        lwip_close(fd);
        return false;
    }

    int soerr = 0;
    socklen_t slen = sizeof(soerr);
    if (lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) {
        ESP_LOGW(kTag, "connect failed soerr=%d", soerr);
        lwip_close(fd);
        return false;
    }

    // 恢复阻塞模式 + 发送超时 + NODELAY（小包/控制消息低延迟）。
    if (lwip_fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        lwip_close(fd);
        return false;
    }
    struct timeval snd;
    snd.tv_sec = static_cast<long>(cfg_.send_timeout_ms / 1000u);
    snd.tv_usec = static_cast<long>((cfg_.send_timeout_ms % 1000u) * 1000u);
    lwip_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    int one = 1;
    lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    outFd = fd;
    return true;
}

void TcpTransport::startRxTask() {
    TaskHandle_t existing = nullptr;
    {
        ScopedLock lock(stateMutex_);
        existing = rxTask_;
    }
    if (existing != nullptr) {
        return;  // 已在运行（重连复用同一 RX 任务）
    }
    TaskHandle_t rx = nullptr;
    if (xTaskCreate(rxTaskEntry, "espview_tcp_rx", kRxTaskStackWords, this, kRxTaskPriority,
                    &rx) != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate(rx) failed");
        return;
    }
    ScopedLock lock(stateMutex_);
    rxTask_ = rx;
}

}  // namespace espview
