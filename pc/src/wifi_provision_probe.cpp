// ESPView M7-D6 — Wi-Fi provisioning UART 硬件探测工具（host-side，真实串口）。
//
// 链路：ESP32（D6 firmware）← UART ← CH340 ← COMx
//       → HostUartTransport → ProtocolEndpoint → WIFI_SCAN_RESULT / WIFI_STATUS / ACK
//
// 流程：
//   1. 打开串口（默认 COM4 @ 115200；可选 --no-reset 跳过复位脉冲）；
//   2. 被动等待 ESP32 HELLO（复位后自动发送；12s 超时）；
//   3. 发送 WIFI_SCAN_REQ（ACK_REQ，maxEntries=32）；
//   4. 等待 ACK + WIFI_SCAN_RESULT（真实空口扫描）+ WIFI_STATUS；
//   5. 打印扫描记录与状态相位；PASS = 收到 SCAN_RESULT（任意 count）
//     或 ACK ERR kInvalidParam（老固件不支持，探针降级语义）。
//
// D6 实测（2026-08-16，COM4/CH340，115200）：
//   - UART 控制路径已验证：HELLO 握手、CAPABILITIES、SET_MODE ACK、
//     WIFI_SCAN_REQ -> ACK(OK) 全部通过；
//   - 该板卡的 Wi-Fi RF 上电（esp_wifi_start 校准）会触发 CH340 USB 掉线
//     （ReadFile err=5）并可能挂死 ESP32（欠压）——即使 PASSIVE 扫描 +
//     80MHz + TX 功率 2dBm 也无法避免；属硬件电源/EMI 限制。
//   - 因此该板卡上本工具预期 FAIL（timeout/掉线）；固件扫描逻辑已就绪，
//     换用供电充足的 USB 口/带供电 HUB/外接 5V 后应能收到 SCAN_RESULT。
//
// 纯 C++17 + Win32 COM API，无 Qt / ESP-IDF；协议数据全部由 shared/protocol 产生。
// 安全：本工具不接收/不打印任何 Wi-Fi 凭据（SSID 为非秘密 metadata）。
//
// 用法：wifi_provision_probe.exe [--port COM4] [--baud 115200]
//                              [--timeout-ms 40000] [--no-reset] [-h]
// 退出码：0 PASS / 1 FAIL（超时/无结果）/ 2 用法错误 / 3 打开串口失败

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "message.h"
#include "protocol_endpoint.h"
#include "serial_transport.h"

using espview::pc::HostUartTransport;
using espview::proto::EndpointConfig;
using espview::proto::ErrorCode;
using espview::proto::HelloInfo;
using espview::proto::Message;
using espview::proto::MessageType;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::SessionError;
using espview::proto::SessionState;
using espview::proto::WifiScanResultInfo;
using espview::proto::WifiStatusInfo;

namespace {

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// 线程安全字节队列（RX worker → 主线程；endpoint 只在主线程触碰）。
class ByteQueue {
public:
    void push(const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> l(m_);
        buf_.insert(buf_.end(), d, d + n);
    }
    bool popAll(std::vector<uint8_t>& out) {
        out.clear();
        std::lock_guard<std::mutex> l(m_);
        if (buf_.empty()) {
            return false;
        }
        out.swap(buf_);
        return true;
    }

private:
    std::mutex m_;
    std::vector<uint8_t> buf_;
};

struct Args {
    std::string port = "COM4";
    uint32_t baud = 115200;
    uint32_t timeoutMs = 40000;
    bool reset = true;
};

bool parseArgs(int argc, char** argv, Args& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("  ERROR: %s requires a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--port") {
            const char* v = need("--port");
            if (!v) return false;
            out.port = v;
        } else if (a == "--baud") {
            const char* v = need("--baud");
            if (!v) return false;
            out.baud = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--timeout-ms") {
            const char* v = need("--timeout-ms");
            if (!v) return false;
            out.timeoutMs = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        } else if (a == "--no-reset") {
            out.reset = false;
        } else if (a == "-h" || a == "--help") {
            std::printf("Usage: wifi_provision_probe.exe [--port COM4] [--baud 115200]\n"
                        "       [--timeout-ms 40000] [--no-reset]\n"
                        "  Sends WIFI_SCAN_REQ over UART and prints the real scan result.\n");
            return false;
        } else {
            std::printf("  ERROR: unknown argument: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

const char* phaseName(uint8_t phase) {
    switch (static_cast<espview::proto::WifiStatusPhase>(phase)) {
        case espview::proto::WifiStatusPhase::kIdle: return "IDLE";
        case espview::proto::WifiStatusPhase::kScanning: return "SCANNING";
        case espview::proto::WifiStatusPhase::kConfigApplying: return "CONFIG_APPLYING";
        case espview::proto::WifiStatusPhase::kWifiConnecting: return "WIFI_CONNECTING";
        case espview::proto::WifiStatusPhase::kWifiConnected: return "WIFI_CONNECTED";
        case espview::proto::WifiStatusPhase::kGotIp: return "GOT_IP";
        case espview::proto::WifiStatusPhase::kTcpConnecting: return "TCP_CONNECTING";
        case espview::proto::WifiStatusPhase::kTcpConnected: return "TCP_CONNECTED";
        case espview::proto::WifiStatusPhase::kError: return "ERROR";
        case espview::proto::WifiStatusPhase::kCleared: return "CLEARED";
    }
    return "UNKNOWN";
}

struct Harness {
    Args args;
    HostUartTransport transport;
    ByteQueue queue;
    std::unique_ptr<ProtocolEndpoint> ep;

    bool sawHello = false;
    bool helloSent = false;
    bool scanRequested = false;
    bool scanReceived = false;
    bool ackOk = false;
    bool ackErr = false;
    ErrorCode ackErrCode = ErrorCode::kNone;
    WifiScanResultInfo scan;
    std::vector<WifiStatusInfo> statuses;
    std::vector<SessionState> states;
    uint64_t openMs = 0;
};

void initEndpoint(Harness& h) {
    EndpointConfig cfg;
    cfg.protocol_version = espview::proto::kProtocolVersion;
    cfg.device_class = 0;
    cfg.width = 320;
    cfg.height = 240;
    cfg.pixel_format = PixelFormat::kRgb565;
    cfg.mode_mask = 0b1111;
    cfg.device_name = "espview-probe";

    auto sink = [&h](const uint8_t* d, size_t n) -> espview::proto::SendStatus {
        return h.transport.send(d, n) ? espview::proto::SendStatus::kOk
                                      : espview::proto::SendStatus::kError;
    };

    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&h](SessionState s) {
        h.states.push_back(s);
        std::printf("  [session] state=%d\n", static_cast<int>(s));
        if (s == SessionState::kConnected && !h.scanRequested) {
            auto req = espview::proto::makeWifiScanReq(0, 32);
            if (!req.has_value()) {
                std::printf("[scan] internal error: makeWifiScanReq failed\n");
                return;
            }
            const espview::proto::SendResult sr = h.ep->sendMessage(*req);
            h.scanRequested = true;
            std::printf("[scan] WIFI_SCAN_REQ sent (sendResult=%d)\n",
                        static_cast<int>(sr));
        }
    };
    cb.onProtocolError = [&h](SessionError e, std::string_view detail) {
        std::printf("  [E] protocol error code=%d detail=%.*s\n", static_cast<int>(e),
                    static_cast<int>(detail.size()), detail.data());
    };
    cb.onHello = [&h](const HelloInfo& hi) {
        h.sawHello = true;
        std::printf("  [hello] version=%u class=%u %ux%u fmt=%u mask=0x%02x name=%.*s\n",
                    hi.protocol_version, hi.device_class, hi.width, hi.height, static_cast<unsigned>(hi.pixel_format),
                    hi.mode_mask, static_cast<int>(hi.device_name.size()), hi.device_name.data());
    };
    cb.onAck = [&h](uint16_t ackSeq, uint8_t status, ErrorCode errorCode) {
        h.ackOk = (status == 0);
        h.ackErr = (status != 0);
        h.ackErrCode = errorCode;
        std::printf("  [ack] seq=%u status=%u errorCode=%u\n", ackSeq, status,
                    static_cast<unsigned>(errorCode));
    };
    cb.onAckTimeout = [&h](uint16_t lastSeq) {
        std::printf("  [ack] TIMEOUT seq=%u (retries exhausted)\n", lastSeq);
    };
    cb.onWifiScanResult = [&h](const WifiScanResultInfo& r) {
        h.scan = r;
        h.scanReceived = true;
    };
    cb.onWifiStatus = [&h](const WifiStatusInfo& s) { h.statuses.push_back(s); };
    cb.onError = [](ErrorCode code, std::string_view text) {
        std::printf("  [E] ERROR code=%u text=%.*s\n", static_cast<unsigned>(code),
                    static_cast<int>(text.size()), text.data());
    };
    h.ep = std::make_unique<ProtocolEndpoint>(cfg, sink, cb, nowMs);
}

void wireTransport(Harness& h) {
    h.transport.setDataCallback([&h](const uint8_t* d, size_t n) { h.queue.push(d, n); });
    h.transport.setStateCallback([&h](HostUartTransport::State s) {
        if (s == HostUartTransport::State::Disconnected ||
            s == HostUartTransport::State::Error) {
            h.ep->onTransportDisconnected();
        }
    });
}

bool openPort(Harness& h) {
    HostUartTransport::Config cfg;
    cfg.port = h.args.port;
    cfg.baud = h.args.baud;
    cfg.read_timeout_ms = 50;
    cfg.reset_on_open = h.args.reset;
    h.openMs = nowMs();
    return h.transport.open(cfg);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView M7-D6 wifi provisioning UART probe ==\n");

    Args args;
    if (!parseArgs(argc, argv, args)) {
        return 2;
    }

    Harness h;
    h.args = args;
    initEndpoint(h);
    wireTransport(h);

    std::printf("[open] port=%s baud=%u reset=%d\n", args.port.c_str(), args.baud,
                args.reset ? 1 : 0);
    if (!openPort(h)) {
        std::printf("[open] FAILED: cannot open %s\n", args.port.c_str());
        return 3;
    }

    // 1) 被动等 HELLO（复位后 ESP32 自动发送）
    const uint64_t helloDeadline = nowMs() + 12000;
    while (!h.sawHello && nowMs() < helloDeadline) {
        std::vector<uint8_t> chunk;
        while (h.queue.popAll(chunk)) {
            h.ep->onTransportData(chunk.data(), chunk.size());
        }
        h.ep->tick();
        // 7s 内未见对端 HELLO（如无复位重连场景）→ 主动发起握手
        if (!h.sawHello && !h.helloSent && nowMs() - h.openMs >= 7000) {
            h.helloSent = true;
            h.ep->onTransportConnected();
            std::printf("  [hello] passive HELLO not seen in 7s -> initiating\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!h.sawHello) {
        std::printf("[hello] TIMEOUT: no HELLO within 12s\n");
        h.transport.close();
        return 1;
    }

    // 2) 会话 kConnected 后已自动发送 WIFI_SCAN_REQ（见 onSessionState）；
    //    等待 ACK + SCAN_RESULT（真实空口扫描可能数秒）
    const uint64_t scanDeadline = nowMs() + args.timeoutMs;
    while (!h.scanReceived && !h.ackErr && nowMs() < scanDeadline) {
        std::vector<uint8_t> chunk;
        while (h.queue.popAll(chunk)) {
            h.ep->onTransportData(chunk.data(), chunk.size());
        }
        h.ep->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    std::printf("\n---- result ----\n");
    if (h.scanReceived) {
        std::printf("[scan] PASS: count=%u flags=0x%02x total=%u truncated=%s\n",
                    h.scan.count, h.scan.flags, h.scan.total,
                    (h.scan.flags & 0x01) ? "yes" : "no");
        for (size_t i = 0; i < h.scan.records.size(); ++i) {
            const auto& r = h.scan.records[i];
            std::printf("  #%02zu ssid=%.*s rssi=%d dBm ch=%u auth=%u\n", i,
                        static_cast<int>(r.ssid.size()), r.ssid.data(),
                        static_cast<int>(r.rssi), static_cast<unsigned>(r.channel),
                        static_cast<unsigned>(r.authmode));
        }
    } else if (h.ackErr && h.ackErrCode == ErrorCode::kInvalidParam) {
        std::printf("[scan] PASS (degraded): firmware rejects WIFI_SCAN_REQ "
                    "(ACK ERR kInvalidParam) -> old firmware without provisioning\n");
    } else if (h.ackErr) {
        std::printf("[scan] FAIL: ACK ERR code=%u\n", static_cast<unsigned>(h.ackErrCode));
    } else {
        std::printf("[scan] FAIL: timeout %.1fs without scan result\n",
                    static_cast<double>(args.timeoutMs) / 1000.0);
    }

    for (size_t i = 0; i < h.statuses.size(); ++i) {
        const auto& s = h.statuses[i];
        std::printf("[status] #%zu phase=%s(%u) err=%u rssi=%d ch=%u ssid=%.*s\n", i,
                    phaseName(s.phase), s.phase, static_cast<unsigned>(s.errorCode),
                    static_cast<int>(s.rssi), static_cast<unsigned>(s.channel),
                    static_cast<int>(s.ssid.size()), s.ssid.data());
    }

    h.transport.close();
    const int rc = (h.scanReceived || (h.ackErr && h.ackErrCode == ErrorCode::kInvalidParam))
                       ? 0
                       : 1;
    std::printf("probe exit=%d\n", rc);
    return rc;
}
