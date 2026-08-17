// ESPView — HostUartTransport（PC 侧 Win32 COM 实现，M1-3B / M8-A3）。

#include "serial_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

namespace espview {
namespace pc {

namespace {

constexpr DWORD kRxBufferSize = 4096;

std::string stateName(HostUartTransport::State s) {
    switch (s) {
        case HostUartTransport::State::kDisconnected:
            return "Disconnected";
        case HostUartTransport::State::kConnecting:
            return "Connecting";
        case HostUartTransport::State::kConnected:
            return "Connected";
        case HostUartTransport::State::kError:
            return "Error";
    }
    return "?";
}

std::string lastErrorText(DWORD err) {
    char buf[256] = {0};
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
    return std::string(buf, n > 0 ? n : 0);
}

}  // namespace

// 默认配置委托构造（.cpp 中展开 Config{}：嵌套 DMIs 须在封闭类完整后求值）。
HostUartTransport::HostUartTransport() : HostUartTransport(Config{}) {}

HostUartTransport::HostUartTransport(Config cfg) : cfg_(std::move(cfg)) {
    caps_.mtu = 20u + 4096u;  // kPacketHeaderSize + kMaxPacketPayload
    caps_.paced = true;       // UART：按 wire 速率节流/背压重试
}

HostUartTransport::~HostUartTransport() {
    close();
}

bool HostUartTransport::open() {
    if (cfg_.port.empty() || cfg_.baud == 0) {
        setState(State::kError);
        return false;
    }

    // M8-A3：重复 open（未 close）明确失败，不做隐式重开（canonical 契约）。
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (opened_) {
            return false;
        }
    }

    setState(State::kConnecting);

    // COM10+ 必须使用 \\\\.\\ 前缀；对低编号 COM 也安全。
    const std::string path = "\\\\.\\" + cfg_.port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "[transport] CreateFile(%s) failed: %s\n", path.c_str(),
                     lastErrorText(GetLastError()).c_str());
        setState(State::kError);
        return false;
    }

    // 读超时：有数据立即返回（任意切分），空闲最多等 read_timeout_ms。
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = cfg_.read_timeout_ms;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = cfg_.write_timeout_ms;
    if (!SetCommTimeouts(h, &to)) {
        std::fprintf(stderr, "[transport] SetCommTimeouts failed: %s\n",
                     lastErrorText(GetLastError()).c_str());
        CloseHandle(h);
        setState(State::kError);
        return false;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        std::fprintf(stderr, "[transport] GetCommState failed: %s\n",
                     lastErrorText(GetLastError()).c_str());
        CloseHandle(h);
        setState(State::kError);
        return false;
    }
    dcb.BaudRate = cfg_.baud;
    dcb.ByteSize = cfg_.data_bits;
    dcb.Parity = (cfg_.parity == 'E') ? EVENPARITY : (cfg_.parity == 'O') ? ODDPARITY : NOPARITY;
    dcb.StopBits = (cfg_.stop_bits == 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        std::fprintf(stderr, "[transport] SetCommState failed: %s\n",
                     lastErrorText(GetLastError()).c_str());
        CloseHandle(h);
        setState(State::kError);
        return false;
    }

    SetupComm(h, 8192, 8192);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    if (cfg_.reset_on_open) {
        applyResetPulse(h);
        // 注意：复位后【不】PurgeComm —— ESP32 应用上电后约 0.5-1s 会主动发出
        // boot HELLO，此时 purge 会把它丢掉导致被动握手永远收不到对端 HELLO。
        // 复位期间 ROM bootloader/启动输出的非协议字节由上层 StreamDecoder /
        // 测试钩子的 badMagic 重同步过滤（Transport 只负责搬运原始字节）。
    }

    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        std::lock_guard<std::mutex> slock(stateMutex_);
        handle_ = h;
        opened_ = true;
        stopRequested_.store(false);
    }

    {
        std::lock_guard<std::mutex> lock(readSizesMutex_);
        readSizes_.clear();
    }

    // M8-A5（HOSTUART-02）：先置 kConnected 再启动 RX 线程 —— rxLoop 启动即失败
    // （如 ReadFile 错误）的 kError 不会被 open 的 kConnected 覆盖（假 Connected 假象）。
    setState(State::kConnected);

    // M8-A5（HOSTUART-06）：线程构造异常 → 回滚句柄/标志，绝不返回假 open。
    try {
        rxThread_ = std::thread([this]() { rxLoop(); });
    } catch (...) {
        std::fprintf(stderr, "[transport] rx thread create failed\n");
        stopRequested_.store(true);
        {   // sendMutex_ → stateMutex_（与 close 同序，无锁序反转）
            std::lock_guard<std::mutex> lock(sendMutex_);
            std::lock_guard<std::mutex> slock(stateMutex_);
            handle_ = INVALID_HANDLE_VALUE;
            opened_ = false;
        }
        CloseHandle(h);
        setState(State::kError);
        return false;
    }

    std::printf("[transport] open OK: %s @ %u 8N1 (mtu=%zu)\n", cfg_.port.c_str(), cfg_.baud,
                caps_.mtu);
    return true;
}

void HostUartTransport::applyResetPulse(HANDLE h) {
    // 与 scripts/pc_com3_session_test.py 一致：DTR=False（GPIO0 高，正常启动），
    // RTS 脉冲 EN 100ms 后释放。
    EscapeCommFunction(h, CLRDTR);
    EscapeCommFunction(h, SETRTS);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EscapeCommFunction(h, CLRRTS);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
}

void HostUartTransport::close() {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!opened_ && handle_ == INVALID_HANDLE_VALUE) {
            return;  // 已关闭 / 从未打开
        }
    }

    stopRequested_.store(true);
    if (rxThread_.joinable()) {
        rxThread_.join();  // ReadFile 短超时保证及时退出
    }

    // M8-A3（审计 E）：在 sendMutex_ 内置 INVALID —— 此时无 send 持锁/WriteFile
    // 在飞（send 全程持锁），句柄竞争彻底消除；随后在锁内 CloseHandle。
    HANDLE h = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        std::lock_guard<std::mutex> slock(stateMutex_);
        h = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        opened_ = false;
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    setState(State::kDisconnected);
}

bool HostUartTransport::isConnected() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return opened_ && state_ == State::kConnected;
}

espview::transport::SendStatus HostUartTransport::send(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return espview::transport::SendStatus::kError;
    }
    // M8-A3（审计 E）：send 全程持 sendMutex_（覆盖整个 WriteFile 循环），
    // 与 close 的"锁内置 INVALID"互斥，消除句柄竞争。
    std::lock_guard<std::mutex> lock(sendMutex_);
    HANDLE h;
    {
        std::lock_guard<std::mutex> slock(stateMutex_);
        if (!opened_ || state_ != State::kConnected) {
            return espview::transport::SendStatus::kNotConnected;
        }
        if (len > caps_.mtu) {
            return espview::transport::SendStatus::kError;
        }
        h = handle_;
    }
    // 分片写入：WriteFile 一次最多写一部分（COM 端口通常整写，但保守处理）。
    size_t off = 0;
    while (off < len) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(4096, len - off));
        DWORD written = 0;
        if (!WriteFile(h, data + off, chunk, &written, nullptr) || written == 0) {
            const DWORD err = GetLastError();
            std::fprintf(stderr, "[transport] WriteFile failed: %s\n",
                         lastErrorText(err).c_str());
            // M8-A5（HOSTUART-01）：写入超时（driver 写缓冲满/线忙）= would-block（背压）；
            // 其余失败 = Transport 层错误 —— 必须驱动断开（setState kError），否则上层
            // 一直看到假 Connected，永不触发重连。注：send 持 sendMutex_，不得在此
            // 调 close()（join rxThread 会死锁）；pumpLoop 看到 kError 后由 runLoop
            // 在 Worker 线程执行 mgr_->close()。
            if (err != ERROR_SEM_TIMEOUT) {
                setState(State::kError);
            }
            return err == ERROR_SEM_TIMEOUT ? espview::transport::SendStatus::kBackpressure
                                            : espview::transport::SendStatus::kError;
        }
        off += written;
        txBytes_.fetch_add(written, std::memory_order_relaxed);
    }
    return espview::transport::SendStatus::kOk;
}

void HostUartTransport::setDataCallback(DataCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    dataCb_ = std::move(cb);
}

void HostUartTransport::setStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> lock(cbMutex_);
    stateCb_ = std::move(cb);
}

void HostUartTransport::rxLoop() {
    std::vector<uint8_t> buf(kRxBufferSize);
    while (!stopRequested_.load(std::memory_order_relaxed)) {
        DWORD n = 0;
        if (!ReadFile(handle_, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
            const DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED && stopRequested_.load(std::memory_order_relaxed)) {
                break;  // 关闭竞态：正常退出
            }
            std::fprintf(stderr, "[transport] ReadFile failed (err=%lu): %s\n",
                         static_cast<unsigned long>(err), lastErrorText(err).c_str());
            setState(State::kError);
            break;
        }
        if (n > 0) {
            rxBytes_.fetch_add(n, std::memory_order_relaxed);
            readCount_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(readSizesMutex_);
                readSizes_.push_back(static_cast<size_t>(n));
                if (readSizes_.size() > 256) {
                    readSizes_.erase(readSizes_.begin());
                }
            }
            DataCallback cb;
            {
                std::lock_guard<std::mutex> lock(cbMutex_);
                cb = dataCb_;
            }
            if (cb) {
                cb(buf.data(), static_cast<size_t>(n));
            }
        }
    }
}

void HostUartTransport::setState(State s) {
    StateCallback cb;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (state_ == s) {
            return;
        }
        state_ = s;
    }
    std::printf("[transport] state -> %s\n", stateName(s).c_str());
    {
        std::lock_guard<std::mutex> lock(cbMutex_);
        cb = stateCb_;
    }
    if (cb) {
        cb(s);
    }
}

uint64_t HostUartTransport::rxBytes() const { return rxBytes_.load(std::memory_order_relaxed); }
uint64_t HostUartTransport::txBytes() const { return txBytes_.load(std::memory_order_relaxed); }
uint64_t HostUartTransport::readCount() const { return readCount_.load(std::memory_order_relaxed); }

std::vector<size_t> HostUartTransport::lastReadSizes(size_t max) const {
    std::lock_guard<std::mutex> lock(readSizesMutex_);
    if (max >= readSizes_.size()) {
        return readSizes_;
    }
    return std::vector<size_t>(readSizes_.end() - static_cast<std::ptrdiff_t>(max),
                               readSizes_.end());
}

}  // namespace pc
}  // namespace espview
