// ESPView M6-A / M8-A3 — TcpTransport 实现（见 tcp_transport.hpp）。

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
// M8-A3：RX join 预算。open() 校验 rx_timeout_ms ≤ 1000，保证 select 周期
// （+调度余量）必在此预算内退出；仍超时 → 泄漏防护（不 deinit/不删除原语）。
constexpr TickType_t kTaskJoinTicks = pdMS_TO_TICKS(1500);
constexpr UBaseType_t kLinkTaskPriority = 6;
constexpr UBaseType_t kRxTaskPriority = 7;

}  // namespace

TcpTransport::TcpTransport(TcpTransportConfig cfg)
    : TransportBase(kTag), cfg_(std::move(cfg)) {
    caps_.mtu = 20u + 4096u;  // Packet Header(20) + 最大 payload(4096)
    caps_.paced = false;      // TCP：依赖 send() 自身背压（socket send buffer）
}

TcpTransport::~TcpTransport() {
    close();
    if (leaked_) {
        // link/RX 任务未退出：全部同步原语保留存活（泄漏，避免 UAF）。
        ESP_LOGE(kTag, "TCP semaphores kept alive (task join timeout)");
        return;
    }
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

bool TcpTransport::open() {
    // 参数校验（Kconfig 范围 + M8-A3 join/超时安全约束）：
    //   - connect_timeout_ms ≥ 100：waitForIp(0) 为 portMAX_DELAY（永等），
    //     close() 的 link join 预算按 connect_timeout 计算，必须覆盖；
    //   - rx_timeout_ms ≤ 1000：RX select 周期必须在 kTaskJoinTicks(1500) 内退出；
    //   - send_timeout_ms > 0：SO_SNDTIMEO=0 表示无限阻塞 send（发送路径无界）。
    if (cfg_.server_ip == nullptr || cfg_.server_ip[0] == '\0' || cfg_.server_port == 0 ||
        cfg_.rx_buf < 512 || cfg_.rx_buf > 16384 || cfg_.connect_timeout_ms < 100 ||
        cfg_.rx_timeout_ms > 1000 || cfg_.send_timeout_ms == 0) {
        setState(State::kError);
        return false;
    }
    if (leaked_) {
        // M8-A3（B M1）：泄漏后不可 reopen —— link/RX 任务可能仍存活，对象被
        // 自持保活（同步原语与 Wi-Fi 驱动保留）。重开需先销毁对象（泄漏）。
        ESP_LOGE(kTag, "open rejected: tasks leaked (object pinned alive)");
        setState(State::kError);
        return false;
    }

    if (sockMutex_ == nullptr) {
        sockMutex_ = xSemaphoreCreateMutex();
    }
    if (linkWake_ == nullptr) {
        linkWake_ = xSemaphoreCreateBinary();
    }
    if (taskExit_ == nullptr) {
        taskExit_ = xSemaphoreCreateCounting(4, 0);
    }
    if (!ensureStateMutex()) {
        setState(State::kError);
        return false;
    }
    if (sockMutex_ == nullptr || linkWake_ == nullptr || taskExit_ == nullptr) {
        setState(State::kError);
        return false;
    }

    {
        ScopedLock lock(stateMutex_);
        if (running_) {
            return false;  // 重复 open 明确报错
        }
        running_ = true;
    }

    setState(State::kConnecting);

    // Wi-Fi STA 初始化（凭据来自 Kconfig/未跟踪 sdkconfig；失败即 Error）。
    // M7-G：adopt 模式跳过 —— 驱动由 WifiProvisioning 持有（含凭据 + GOT_IP），
    // 本类只做 TCP client（UART bootstrap → TCP handoff 不重复 init）。
    if (!cfg_.adopt_existing_wifi) {
        const esp_err_t werr = wifi_.init(cfg_.wifi);
        if (werr != ESP_OK) {
            setState(State::kError);
            {
                ScopedLock lock(stateMutex_);
                running_ = false;  // CS-5：open 失败完全回滚（close() 幂等兜底）
            }
            wifi_.deinit();  // 幂等：确保半初始化的 Wi-Fi 也释放
            return false;
        }
    }

    const BaseType_t terr = xTaskCreate(linkTaskEntry, "espview_tcp_link", kLinkTaskStackWords,
                                        this, kLinkTaskPriority, &linkTask_);
    if (terr != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate(link) failed");
        setState(State::kError);
        {
            ScopedLock lock(stateMutex_);
            running_ = false;  // CS-5：open 失败完全回滚
        }
        if (!cfg_.adopt_existing_wifi) {
            wifi_.deinit();  // 回滚已初始化的 Wi-Fi（TransportManager close() 兜底安全）
        }
        return false;
    }
    ESP_LOGI(kTag, "open: target=%s:%u timeout=%u delay=%u", cfg_.server_ip,
             static_cast<unsigned>(cfg_.server_port),
             static_cast<unsigned>(cfg_.connect_timeout_ms),
             static_cast<unsigned>(cfg_.reconnect_delay_ms));
    return true;
}

void TcpTransport::close() {
    if (sockMutex_ == nullptr || stateMutex_ == nullptr) {
        return;  // 从未 open
    }
    TaskHandle_t link = nullptr;
    {
        ScopedLock lock(stateMutex_);
        if (!running_) {
            return;  // 幂等
        }
        running_ = false;
        link = linkTask_;
    }

    // 唤醒 link 任务（可能在等待通知/退避中）→ 等待 link 退出；link 退出前会
    // shutdown+close socket（RX select 立即返回）并唤醒 RX 任务退出。
    notifyLink();
    bool linkGone = (link == nullptr);
    if (link != nullptr && taskExit_ != nullptr) {
        // link 任务可能在 wifi_.waitForIp()（最长 connect_timeout）中：
        // join 预算必须覆盖 connect_timeout + reconnect_delay，否则 close() 与
        // wifi_.deinit() 会与仍在跑的任务竞态。Kconfig 允许两项各 60000ms →
        // close() 最坏约 122s（有界；正常路径远小于此，M8-A3 B M3）。
        const TickType_t linkJoinTicks =
            pdMS_TO_TICKS(cfg_.connect_timeout_ms + cfg_.reconnect_delay_ms + 2000u);
        linkGone = xSemaphoreTake(taskExit_, linkJoinTicks) == pdTRUE;
        if (!linkGone) {
            ESP_LOGW(kTag, "link task did not exit in time");
        }
    }

    // link 可能在退出前启动了 RX：重新读取并等待 RX 退出。
    TaskHandle_t rx = nullptr;
    {
        ScopedLock lock(stateMutex_);
        rx = rxTask_;
    }
    bool rxGone = (rx == nullptr);
    if (rx != nullptr && taskExit_ != nullptr) {
        rxGone = xSemaphoreTake(taskExit_, kTaskJoinTicks) == pdTRUE;
        if (!rxGone) {
            ESP_LOGW(kTag, "RX task did not exit in time");
        }
    }

    // M8-A3（审计 H1/M1 / B2）：join 超时 → 任务仍存活。此时不得 deinit Wi-Fi、
    // 不得清理任务句柄、析构不得删除同步原语（清理即 UAF）。标记泄漏、清回调 +
    // 状态落定（泄漏后不可 reopen，B M2）并经自持 shared_ptr 保活（对象与任务
    // 同寿命）；这是降级路径（正常路径 join 必成功）。
    if (!linkGone || !rxGone) {
        leaked_ = true;
        {
            ScopedLock lock(stateMutex_);
            dataCb_ = nullptr;
            stateCb_ = nullptr;
            state_ = State::kError;
        }
#if defined(__cpp_exceptions) && __cpp_exceptions
        try {
            selfRef_ = shared_from_this();  // 从 close 调用者的引用续命
        } catch (const std::bad_weak_ptr&) {
            // close() 在析构路径调用且任务未退出：无法保活（违反
            // close-before-destroy 契约 + 泄漏双故障），保守记录。
            ESP_LOGE(kTag, "leak while destructing: cannot pin object");
        }
#else
        // -fno-exceptions：bad_weak_ptr 会 terminate，不能尝试续命，仅记录。
        ESP_LOGE(kTag, "leak while destructing: cannot pin object");
#endif
        ESP_LOGE(kTag, "task join timeout; leaking TCP/Wi-Fi resources (UAF guard)");
        return;
    }

    {
        ScopedLock lock(stateMutex_);
        linkTask_ = nullptr;
        rxTask_ = nullptr;
    }
    if (!cfg_.adopt_existing_wifi) {
        wifi_.deinit();  // M7-G：adopt 模式下驱动归 WifiProvisioning 所有，不卸载
    }
    setState(State::kDisconnected);
    ESP_LOGI(kTag, "closed");
}

bool TcpTransport::isConnected() const {
    ScopedLock lock(sockMutex_);
    return connected_;
}

espview::transport::SendStatus TcpTransport::send(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return espview::transport::SendStatus::kError;
    }
    if (sockMutex_ == nullptr) {
        return espview::transport::SendStatus::kNotConnected;  // 从未 open
    }
    if (xSemaphoreTake(sockMutex_, kMutexTakeTicks) != pdTRUE) {
        return espview::transport::SendStatus::kBackpressure;  // 门忙：would-block
    }
    if (!connected_ || sock_ < 0) {
        xSemaphoreGive(sockMutex_);
        return espview::transport::SendStatus::kNotConnected;
    }
    if (len > caps_.mtu) {
        xSemaphoreGive(sockMutex_);
        return espview::transport::SendStatus::kError;
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

    if (fatal) {
        notifyLink();
    }
    return mapSend(result);
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
        setState(State::kConnecting);
        // M7-G：adopt 模式跳过 Wi-Fi 阶段 —— 驱动由 WifiProvisioning 持有且已
        // 取得 GOT_IP（handoff 触发点）；Wi-Fi 掉线由 provisioning 自行重连，
        // 本阶段在 IP 恢复前表现为 TCP connect 失败并退避重试。
        if (!cfg_.adopt_existing_wifi) {
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
        }

        // ---- 阶段 2：TCP connect ----
        int fd = -1;
        if (!connectOnce(fd)) {
            reconnectCount_.fetch_add(1, std::memory_order_relaxed);
            xSemaphoreTake(linkWake_, pdMS_TO_TICKS(cfg_.reconnect_delay_ms));
            continue;
        }
        bool closeNow = false;
        {
            ScopedLock lock(stateMutex_);
            closeNow = !running_;  // close() 已请求：丢弃刚建立的连接
        }
        if (closeNow) {
            // fd 尚未移交 sock_（本地变量）：直接关闭；锁统一归 sockMutex_
            // （M8-A3：勿在 stateMutex_ 下关 fd）。
            ScopedLock lock(sockMutex_);
            lwip_shutdown(fd, SHUT_RDWR);
            lwip_close(fd);
            break;
        }
        {
            ScopedLock lock(sockMutex_);
            sock_ = fd;
            connected_ = true;
        }
        startRxTask();
        {
            // M8-A3（B M8）：发布 kConnected 前再查一次 running_ —— close() 可在
            // 连接建立窗口内置位，防止关闭期间出现 spurious Connected→Disconnected。
            ScopedLock lock(stateMutex_);
            closeNow = !running_;
        }
        if (closeNow) {
            ScopedLock lock(sockMutex_);
            closeSocketLocked();
            break;
        }
        setState(State::kConnected);
        wasConnected = true;
        ESP_LOGI(kTag, "TCP CONNECTED (reconnect=%u)",
                 static_cast<unsigned>(reconnectCount_.load(std::memory_order_relaxed)));

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
            closeSocketLocked();
        }
        {
            ScopedLock lock(stateMutex_);
            exitNow = !running_;
        }
        if (exitNow) {
            break;
        }
        if (wasConnected) {
            setState(State::kDisconnected);
            wasConnected = false;
            reconnectCount_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGW(kTag, "TCP DISCONNECTED; reconnecting in %u ms",
                     static_cast<unsigned>(cfg_.reconnect_delay_ms));
        }
        xSemaphoreTake(linkWake_, pdMS_TO_TICKS(cfg_.reconnect_delay_ms));
    }

    // ---- link 退出路径：关闭 socket，唤醒 RX 退出 ----
    {
        ScopedLock lock(sockMutex_);
        closeSocketLocked();
    }
    {
        // M8-A3（B3）：锁内清 linkTask_ + notify RX —— RX 任务退出路径在同一锁下
        // 清 rxTask_ 后再 vTaskDelete，杜绝对已删除 TCB 的 notify（stale TCB）。
        ScopedLock lock(stateMutex_);
        running_ = false;
        linkTask_ = nullptr;
        if (rxTask_ != nullptr) {
            xTaskNotifyGive(rxTask_);
        }
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
        {
            // M8-A3（B3）：退出前锁内清句柄（close 同锁 notify，避免 stale TCB）。
            ScopedLock lock(stateMutex_);
            rxTask_ = nullptr;
        }
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
            notifyLink();
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
        notifyLink();
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    vPortFree(buf);
    {
        // M8-A3（B3）：退出前锁内清句柄（close 同锁 notify，避免 stale TCB）。
        ScopedLock lock(stateMutex_);
        rxTask_ = nullptr;
    }
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
    // M8-A3（B3）：直接写入 rxTask_ —— RX 优先级高于 link，任务可能先于后续
    // 赋值退出；滞后赋值会让 link 持有 stale TCB（与 UartTransport 同策略）。
    const BaseType_t rc = xTaskCreate(rxTaskEntry, "espview_tcp_rx", kRxTaskStackWords, this,
                                      kRxTaskPriority, &rxTask_);
    if (rc != pdPASS) {
        ESP_LOGE(kTag, "xTaskCreate(rx) failed");
        {
            ScopedLock lock(stateMutex_);
            rxTask_ = nullptr;
        }
        return;
    }
}

void TcpTransport::closeSocketLocked() {
    if (sock_ >= 0) {
        lwip_shutdown(sock_, SHUT_RDWR);
        lwip_close(sock_);
        sock_ = -1;
    }
    connected_ = false;
}

void TcpTransport::notifyLink() {
    if (linkWake_ != nullptr) {
        xSemaphoreGive(linkWake_);
    }
}

}  // namespace espview
