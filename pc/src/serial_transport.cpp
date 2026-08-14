// ESPView — HostUartTransport（PC 侧 Win32 COM 实现，M1-3B）。

#include "serial_transport.h"

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
        case HostUartTransport::State::Disconnected:
            return "Disconnected";
        case HostUartTransport::State::Connecting:
            return "Connecting";
        case HostUartTransport::State::Connected:
            return "Connected";
        case HostUartTransport::State::Error:
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

HostUartTransport::~HostUartTransport() {
    close();
}

bool HostUartTransport::open(const PcTransportConfig& cfgBase) {
    const auto& cfg = static_cast<const Config&>(cfgBase);
    close();  // 幂等：先清理任何残留状态

    if (cfg.port.empty() || cfg.baud == 0) {
        setState(State::Error);
        return false;
    }

    setState(State::Connecting);

    // COM10+ 必须使用 \\\\.\\ 前缀；对低编号 COM 也安全。
    const std::string path = "\\\\.\\" + cfg.port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "[transport] CreateFile(%s) failed: %s\n", path.c_str(),
                     lastErrorText(GetLastError()).c_str());
        setState(State::Error);
        return false;
    }

    // 读超时：有数据立即返回（任意切分），空闲最多等 read_timeout_ms。
    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = cfg.read_timeout_ms;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = cfg.write_timeout_ms;
    if (!SetCommTimeouts(h, &to)) {
        std::fprintf(stderr, "[transport] SetCommTimeouts failed: %s\n",
                     lastErrorText(GetLastError()).c_str());
        CloseHandle(h);
        setState(State::Error);
        return false;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) {
        std::fprintf(stderr, "[transport] GetCommState failed: %s\n",
                     lastErrorText(GetLastError()).c_str());
        CloseHandle(h);
        setState(State::Error);
        return false;
    }
    dcb.BaudRate = cfg.baud;
    dcb.ByteSize = cfg.data_bits;
    dcb.Parity = (cfg.parity == 'E') ? EVENPARITY : (cfg.parity == 'O') ? ODDPARITY : NOPARITY;
    dcb.StopBits = (cfg.stop_bits == 2) ? TWOSTOPBITS : ONESTOPBIT;
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
        setState(State::Error);
        return false;
    }

    SetupComm(h, 8192, 8192);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    if (cfg.reset_on_open) {
        applyResetPulse(h);
        // 注意：复位后【不】PurgeComm —— ESP32 应用上电后约 0.5-1s 会主动发出
        // boot HELLO，此时 purge 会把它丢掉导致被动握手永远收不到对端 HELLO。
        // 复位期间 ROM bootloader/启动输出的非协议字节由上层 StreamDecoder /
        // 测试钩子的 badMagic 重同步过滤（Transport 只负责搬运原始字节）。
    }

    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        handle_ = h;
        mtu_ = 20 + 4096;  // kPacketHeaderSize + kMaxPacketPayload
        opened_ = true;
        stopRequested_.store(false);
    }

    {
        std::lock_guard<std::mutex> lock(readSizesMutex_);
        readSizes_.clear();
    }

    rxThread_ = std::thread([this]() { rxLoop(); });

    setState(State::Connected);
    std::printf("[transport] open OK: %s @ %u 8N1 (mtu=%zu)\n", cfg.port.c_str(), cfg.baud,
                mtu());
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

    HANDLE h = INVALID_HANDLE_VALUE;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        h = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        opened_ = false;
        mtu_ = 0;
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    setState(State::Disconnected);
}

bool HostUartTransport::isConnected() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return opened_ && state_ == State::Connected;
}

bool HostUartTransport::send(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }
    HANDLE h;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!opened_ || state_ != State::Connected) {
            return false;
        }
        h = handle_;
    }
    DWORD written = 0;
    // 分片写入：WriteFile 一次最多写一部分（COM 端口通常整写，但保守处理）。
    size_t off = 0;
    while (off < len) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(4096, len - off));
        if (!WriteFile(h, data + off, chunk, &written, nullptr) || written == 0) {
            std::fprintf(stderr, "[transport] WriteFile failed: %s\n",
                         lastErrorText(GetLastError()).c_str());
            return false;
        }
        off += written;
        txBytes_.fetch_add(written, std::memory_order_relaxed);
    }
    return true;
}

size_t HostUartTransport::mtu() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return mtu_;
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
            setState(State::Error);
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
