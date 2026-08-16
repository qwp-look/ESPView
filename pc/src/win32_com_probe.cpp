// ESPView M7-F — 最小 Win32-only serial probe（pc 侧，纯字节搬运）。
//
// 目标：为 M7-F 硬件实验提供最小、可测、结构化日志的串口探针：
//   1. 打开串口（默认 COM4 @ 115200，--port/--baud 可覆盖）；打开过程
//      【绝不】断言 DTR/RTS（fDtrControl=fRtsControl=DISABLE），不改变
//      ESP32 reset 状态；记录打开前/后的 GetCommState 关键字段。
//   2. 只读模式：持续 ReadFile，输出带相对时间戳的原始字节（hex + ASCII
//      摘要，量可控）；记录每次 ReadFile / ClearCommError 失败（错误码 +
//      GetLastError 文本）。
//   3. --pulse-reset：EscapeCommFunction 做 EN 复位脉冲（CLRDTR 保持
//      GPIO0 高，SETRTS -> 100ms -> CLRRTS），随后继续读；复位期间
//      【不】PurgeComm（避免丢 boot HELLO/日志）。
//   4. 掉线检测与重枚举：ReadFile 失败 -> disconnect 事件 -> close+reopen
//      （CreateFile 重试循环，间隔 --reopen-delay 默认 2s），记录重新枚举
//      成功时刻与丢失时长；--max-reopens 限制重开次数（默认无限）。
//   5. 输出格式统一：[evt] t=+<rel_ms> kind=<kind> k=v ...
//                   [rx] t=+<ms> n=<bytes> hex=... ascii=...
//   6. --timeout-ms 总运行上限（默认 30000）：到时退出 0；
//      重开超限（--max-reopens）退出 2；初始打开失败退出 3；用法错误退出 2。
//
// 安全：纯字节搬运，无协议解析，不涉及任何 Wi-Fi 密码。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr DWORD kReadTimeoutMs = 250;   // ReadFile 空闲超时（快速响应总超时检查）
constexpr DWORD kRxBufSize = 4096;      // ReadFile 缓冲
constexpr size_t kHexCapBytes = 64;     // [rx] hex 摘要上限（字节）
constexpr size_t kAsciiCapBytes = 64;   // [rx] ascii 摘要上限（字符）
constexpr int64_t kDefaultTimeoutMs = 30000;
constexpr int64_t kDefaultReopenDelayMs = 2000;
constexpr int64_t kUnlimitedReopens = -1;

int64_t g_t0Ms = 0;

int64_t nowMs() {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

int64_t relMs() { return nowMs() - g_t0Ms; }

std::string errText(DWORD err) {
    char buf[256] = {0};
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, err, 0, buf, static_cast<DWORD>(sizeof(buf) - 1),
                                   nullptr);
    std::string s(buf, n > 0 ? static_cast<size_t>(n) : 0);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

// [evt] t=+<rel_ms> kind=<kind> <k=v ...>
void logEvt(const char* kind, const char* fmt, ...) {
    char kv[512] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(kv, sizeof(kv) - 1, fmt, ap);
    va_end(ap);
    std::printf("[evt] t=+%lld kind=%s %s\n", static_cast<long long>(relMs()), kind, kv);
    std::fflush(stdout);
}

const char* dtrName(DWORD v) {
    switch (v) {
        case DTR_CONTROL_DISABLE:
            return "disabled";
        case DTR_CONTROL_ENABLE:
            return "enabled";
        case DTR_CONTROL_HANDSHAKE:
            return "handshake";
        default:
            return "?";
    }
}

const char* rtsName(DWORD v) {
    switch (v) {
        case RTS_CONTROL_DISABLE:
            return "disabled";
        case RTS_CONTROL_ENABLE:
            return "enabled";
        case RTS_CONTROL_HANDSHAKE:
            return "handshake";
        case RTS_CONTROL_TOGGLE:
            return "toggle";
        default:
            return "?";
    }
}

// 记录指定句柄的 DCB 关键字段（打开前/后各调用一次）。
void logCommState(const char* tag, HANDLE h) {
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        const DWORD err = GetLastError();
        logEvt("comm_state_error", "tag=%s err=%lu (%s)", tag,
               static_cast<unsigned long>(err), errText(err).c_str());
        return;
    }
    logEvt("comm_state",
           "tag=%s baud=%lu bytesize=%u parity=%u stopbits=%u "
           "dtr_control=%lu(%s) rts_control=%lu(%s)",
           tag, static_cast<unsigned long>(dcb.BaudRate),
           static_cast<unsigned int>(dcb.ByteSize), static_cast<unsigned int>(dcb.Parity),
           static_cast<unsigned int>(dcb.StopBits),
           static_cast<unsigned long>(dcb.fDtrControl), dtrName(dcb.fDtrControl),
           static_cast<unsigned long>(dcb.fRtsControl), rtsName(dcb.fRtsControl));
}

// 打开串口并配置 8N1。绝不断言 DTR/RTS：fDtrControl=fRtsControl=DISABLE，
// 且本函数全程不调用 SETDTR/SETRTS。返回句柄或 INVALID_HANDLE_VALUE
// （*errOut 填 GetLastError）。verbose=true 时记录打开事件与 comm_state
// before/after（重枚举静默，仅记录 reopen_ok）。
HANDLE openPort(const std::string& port, DWORD baud, bool verbose, DWORD* errOut) {
    *errOut = 0;
    const std::string path = "\\\\.\\" + port;
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *errOut = GetLastError();
        return h;
    }
    if (verbose) {
        logEvt("open", "port=%s path=%s", port.c_str(), path.c_str());
        logCommState("before", h);
    }

    COMMTIMEOUTS to = {};
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = kReadTimeoutMs;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 0;
    if (!SetCommTimeouts(h, &to)) {
        const DWORD err = GetLastError();
        if (verbose) {
            logEvt("setcommtimeouts_fail", "err=%lu (%s)", static_cast<unsigned long>(err),
                   errText(err).c_str());
        }
        CloseHandle(h);
        *errOut = err;
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        const DWORD err = GetLastError();
        if (verbose) {
            logEvt("getcommstate_fail", "err=%lu (%s)", static_cast<unsigned long>(err),
                   errText(err).c_str());
        }
        CloseHandle(h);
        *errOut = err;
        return INVALID_HANDLE_VALUE;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;  // 绝不断言 DTR（不动 GPIO0）
    dcb.fRtsControl = RTS_CONTROL_DISABLE;  // 绝不断言 RTS（不动 EN）
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        const DWORD err = GetLastError();
        if (verbose) {
            logEvt("setcommstate_fail", "err=%lu (%s)", static_cast<unsigned long>(err),
                   errText(err).c_str());
        }
        CloseHandle(h);
        *errOut = err;
        return INVALID_HANDLE_VALUE;
    }
    if (verbose) {
        logCommState("after", h);
    }

    SetupComm(h, 8192, 8192);
    // 打开时清一次陈旧字节；【复位后】绝不 PurgeComm（避免丢 boot HELLO/日志）。
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return h;
}

// EN 复位脉冲（与 pc/src/serial_transport.cpp::applyResetPulse 语义一致）：
// CLRDTR（GPIO0 高，正常启动）-> SETRTS（EN=0，复位）-> 100ms -> CLRRTS（EN=1，释放）。
// 调用方保证复位后不 PurgeComm。
void applyResetPulse(HANDLE h) {
    const bool dtrOk = EscapeCommFunction(h, CLRDTR);
    const bool setOk = EscapeCommFunction(h, SETRTS);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool clrOk = EscapeCommFunction(h, CLRRTS);
    if (!dtrOk || !setOk || !clrOk) {
        const DWORD err = GetLastError();
        logEvt("reset_escape_fail", "dtr_ok=%d set_ok=%d clr_ok=%d err=%lu (%s)", dtrOk ? 1 : 0,
               setOk ? 1 : 0, clrOk ? 1 : 0, static_cast<unsigned long>(err),
               errText(err).c_str());
    }
    logEvt("reset_pulse", "dtr=clr rts=set_100ms_then_clr");
}

// [rx] t=+<ms> n=<bytes> hex=... ascii=...（hex/ascii 均截断到摘要上限，
// 截断时带 *_extra=未显示字节数，保证量可控）。
void logRx(const uint8_t* d, size_t n) {
    std::string hex;
    size_t hexShown = 0;
    for (; hexShown < n && hexShown < kHexCapBytes; ++hexShown) {
        char b[4] = {0};
        std::snprintf(b, sizeof(b), "%02X", static_cast<unsigned int>(d[hexShown]));
        hex += b;
    }
    std::string ascii;
    size_t asciiShown = 0;
    for (; asciiShown < n && asciiShown < kAsciiCapBytes; ++asciiShown) {
        const unsigned int c = static_cast<unsigned int>(d[asciiShown]);
        ascii += (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.';
    }
    const size_t hexExtra = n - hexShown;
    const size_t asciiExtra = n - asciiShown;
    if (hexExtra > 0 || asciiExtra > 0) {
        std::printf("[rx] t=+%lld n=%llu hex=%s hex_extra=%llu ascii=%s ascii_extra=%llu\n",
                    static_cast<long long>(relMs()), static_cast<unsigned long long>(n),
                    hex.c_str(), static_cast<unsigned long long>(hexExtra), ascii.c_str(),
                    static_cast<unsigned long long>(asciiExtra));
    } else {
        std::printf("[rx] t=+%lld n=%llu hex=%s ascii=%s\n", static_cast<long long>(relMs()),
                    static_cast<unsigned long long>(n), hex.c_str(), ascii.c_str());
    }
    std::fflush(stdout);
}

struct Options {
    std::string port = "COM4";
    DWORD baud = 115200;
    bool pulseReset = false;
    int64_t timeoutMs = kDefaultTimeoutMs;
    int64_t reopenDelayMs = kDefaultReopenDelayMs;
    int64_t maxReopens = kUnlimitedReopens;
};

bool parseI64(const char* s, int64_t* out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long long v = std::strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *out = static_cast<int64_t>(v);
    return true;
}

// 返回 0=OK，1=用法错误，2=打印帮助后退出。
int parseArgs(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            return 2;
        }
        if (a == "--port") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --port requires a value\n");
                return 1;
            }
            opt->port = argv[++i];
        } else if (a == "--baud") {
            int64_t v = 0;
            if (i + 1 >= argc || !parseI64(argv[++i], &v) || v <= 0 || v > 4294967295LL) {
                std::fprintf(stderr, "error: --baud requires a positive integer\n");
                return 1;
            }
            opt->baud = static_cast<DWORD>(v);
        } else if (a == "--pulse-reset") {
            opt->pulseReset = true;
        } else if (a == "--timeout-ms") {
            int64_t v = 0;
            if (i + 1 >= argc || !parseI64(argv[++i], &v) || v <= 0) {
                std::fprintf(stderr, "error: --timeout-ms requires a positive integer\n");
                return 1;
            }
            opt->timeoutMs = v;
        } else if (a == "--reopen-delay") {
            int64_t v = 0;
            if (i + 1 >= argc || !parseI64(argv[++i], &v) || v < 0) {
                std::fprintf(stderr, "error: --reopen-delay requires a non-negative integer\n");
                return 1;
            }
            opt->reopenDelayMs = v;
        } else if (a == "--max-reopens") {
            int64_t v = 0;
            if (i + 1 >= argc || !parseI64(argv[++i], &v) || v < -1) {
                std::fprintf(stderr, "error: --max-reopens requires -1 or a non-negative integer\n");
                return 1;
            }
            opt->maxReopens = v;
        } else {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            return 1;
        }
    }
    return 0;
}

void printUsage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s [--port COM4] [--baud 115200] [--pulse-reset]\n"
                 "            [--timeout-ms 30000] [--reopen-delay 2000]\n"
                 "            [--max-reopens N] [--help]\n"
                 "  --port COM4      : 串口名（默认 COM4）\n"
                 "  --baud 115200    : 波特率（默认 115200）\n"
                 "  --pulse-reset    : EN 复位脉冲（CLRDTR + SETRTS 100ms + CLRRTS），随后继续读；\n"
                 "                     复位后不 PurgeComm（保留 boot HELLO/日志）\n"
                 "  --timeout-ms N   : 总运行上限 ms（默认 30000，到时退出 0）\n"
                 "  --reopen-delay N : 掉线重枚举重试间隔 ms（默认 2000）\n"
                 "  --max-reopens N  : 掉线重开次数上限（-1=无限，默认无限；超限退出 2）\n"
                 "  --help           : 打印本帮助\n"
                 "退出码：0 到时/正常结束；2 用法错误或重开超限；3 初始打开失败\n",
                 argv0);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    const int pr = parseArgs(argc, argv, &opt);
    if (pr == 2) {
        printUsage(argv[0]);
        return 0;
    }
    if (pr == 1) {
        printUsage(argv[0]);
        return 2;
    }

    g_t0Ms = nowMs();
    logEvt("cfg",
           "port=%s baud=%lu pulse_reset=%d timeout_ms=%lld reopen_delay_ms=%lld max_reopens=%lld",
           opt.port.c_str(), static_cast<unsigned long>(opt.baud), opt.pulseReset ? 1 : 0,
           static_cast<long long>(opt.timeoutMs), static_cast<long long>(opt.reopenDelayMs),
           static_cast<long long>(opt.maxReopens));

    DWORD openErr = 0;
    HANDLE h = openPort(opt.port, opt.baud, true, &openErr);
    if (h == INVALID_HANDLE_VALUE) {
        logEvt("open_fail", "port=%s err=%lu (%s)", opt.port.c_str(),
               static_cast<unsigned long>(openErr), errText(openErr).c_str());
        return 3;
    }
    logEvt("open_ok", "port=%s baud=%lu", opt.port.c_str(), static_cast<unsigned long>(opt.baud));

    if (opt.pulseReset) {
        applyResetPulse(h);
        // 复位后绝不 PurgeComm：保留 boot HELLO/日志字节。
    }

    std::vector<uint8_t> buf(kRxBufSize);
    int64_t reopens = 0;
    while (true) {
        if (relMs() >= opt.timeoutMs) {
            logEvt("timeout", "timeout_ms=%lld", static_cast<long long>(opt.timeoutMs));
            CloseHandle(h);
            return 0;
        }

        DWORD n = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr)) {
            const DWORD err = GetLastError();
            logEvt("read_fail", "err=%lu (%s)", static_cast<unsigned long>(err),
                   errText(err).c_str());

            DWORD commMask = 0;
            COMSTAT cs = {};
            if (!ClearCommError(h, &commMask, &cs)) {
                const DWORD cce = GetLastError();
                logEvt("clearcomm_fail", "err=%lu (%s)", static_cast<unsigned long>(cce),
                       errText(cce).c_str());
            } else if (commMask != 0) {
                logEvt("comm_error_flags", "mask=0x%08lx in_queue=%lu",
                       static_cast<unsigned long>(commMask), static_cast<unsigned long>(cs.cbInQue));
            }

            const int64_t lostAt = relMs();
            logEvt("disconnect", "err=%lu lost_at_ms=%lld", static_cast<unsigned long>(err),
                   static_cast<long long>(lostAt));
            CloseHandle(h);
            h = INVALID_HANDLE_VALUE;

            ++reopens;
            if (opt.maxReopens >= 0 && reopens > opt.maxReopens) {
                logEvt("reopen_limit", "reopens=%lld max_reopens=%lld",
                       static_cast<long long>(reopens), static_cast<long long>(opt.maxReopens));
                return 2;
            }

            int64_t attempts = 0;
            while (true) {
                if (relMs() >= opt.timeoutMs) {
                    logEvt("timeout", "timeout_ms=%lld disconnected=1",
                           static_cast<long long>(opt.timeoutMs));
                    return 0;
                }
                if (opt.reopenDelayMs > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(opt.reopenDelayMs));
                }
                ++attempts;
                DWORD reErr = 0;
                HANDLE nh = openPort(opt.port, opt.baud, false, &reErr);
                if (nh != INVALID_HANDLE_VALUE) {
                    h = nh;
                    logEvt("reopen_ok", "reopens=%lld attempts=%lld lost_ms=%lld",
                           static_cast<long long>(reopens), static_cast<long long>(attempts),
                           static_cast<long long>(relMs() - lostAt));
                    break;
                }
                logEvt("reopen_fail", "reopens=%lld attempts=%lld err=%lu (%s)",
                       static_cast<long long>(reopens), static_cast<long long>(attempts),
                       static_cast<unsigned long>(reErr), errText(reErr).c_str());
            }
            continue;
        }

        if (n > 0) {
            logRx(buf.data(), n);
        }
    }
}
