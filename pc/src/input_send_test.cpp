// ESPView M3 — PC → ESP32 输入链路真实 COM3 验收工具（host-side，无 Qt）。
//
// 链路：脚本 → InputEvent（共享 input_codec）→ ProtocolEndpoint → HostUartTransport
//       → COM3 → ESP32 ProtocolEndpoint → InputManager → IInputListener
//
// 与 com3_frame_test 相同的会话模式：被动等 ESP32 boot HELLO；7s 未 CONNECTED
// 主动发起 HELLO；断线重连等对端超时后重新握手。
//
// 命令（--cmd 可重复，按序执行，间隔 150ms）：
//   key <name> down|up          a..z, 0..9, f1..f12, enter, escape, tab, backspace,
//                               space, left/right/up/down, home, end, pageup, pagedown,
//                               insert, delete, ctrl, shift, alt, gui,
//                               capslock, numlock, scrolllock
//   key <mods>+<name>           ctrl+a / shift+a / ctrl+shift+a（按下→组合→抬起）
//   mouse move x,y              逻辑显示坐标（0..319, 0..239）
//   mouse left|right|middle down|up
//   mouse left|right|middle click
//   wheel <n>                   滚轮格数（-128..127；Qt 归一化等价 120/格）
//   sleep <ms>                  等待
//
// 场景：
//   （默认）顺序执行命令；期间打印 ESP32 经 ERROR 消息上报的输入统计
//   （"inp rx=.. k=.. b=.. inv=.. u=.. r=.. sk=.. sb=.."），以及 PING→PONG RTT。
//   --reconnect            先发 "key ctrl down" + "mouse left down"（按住），
//                          断开 COM3 → 等对端超时 → 重连 → 验证 ESP32
//                          resetState() 后 pressed keys=0 / buttons=0。
//   --large-frame-check    等 TestPattern 的 153600B FULL 开始流式发送后，在
//                          帧传输期间发送按键，验证输入不被大帧阻塞，且帧仍提交。
//
// 协议数据全部由 shared/protocol（Encoder/Endpoint）+ shared/input（codec）产生，
// 本文件不复制任何 Packet/Message/输入编码逻辑。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "message.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "serial_transport.h"

#include "input_codec.h"
#include "input_event.h"
#include "keyboard_mapper.h"

using espview::pc::HostUartTransport;
using espview::proto::EndpointConfig;
using espview::proto::ErrorCode;
using espview::proto::HelloInfo;
using espview::proto::ProtocolEndpoint;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SessionError;
using espview::proto::SessionState;

namespace {

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ---- 线程安全字节队列（RX worker → 主线程）----
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
    size_t sizeBytes() {
        std::lock_guard<std::mutex> l(m_);
        return buf_.size();
    }

private:
    std::mutex m_;
    std::vector<uint8_t> buf_;
};

// ---- ESP32 输入统计（经 ERROR 消息文本上报）----
struct EspInputStats {
    uint64_t rx = 0;    // eventsReceived
    uint64_t k = 0;     // pressedKeys（当前）
    uint64_t b = 0;     // pressedButtons（当前）
    uint64_t inv = 0;   // invalidEvents
    uint64_t u = 0;     // unsupportedEvents
    uint64_t r = 0;     // resetCount
    uint64_t sk = 0;    // stuckKeysReleased
    uint64_t sb = 0;    // stuckButtonsReleased
    uint64_t st = 0;    // ESP32 session state (0=Disconnected .. 3=Connected)
    // ---- M4：ESP32 会话/心跳/协议统计（"sess" 行）----
    uint64_t txh = 0, rxh = 0;      // HELLO tx/rx
    uint64_t txp = 0, rxp = 0;      // PING tx/rx
    uint64_t tqo = 0, rqo = 0;      // PONG tx/rx
    uint64_t dec = 0, crc = 0, seqg = 0;  // decoderErrors / crcErrors / seqGaps
    bool seen = false;
};

bool parseEspStats(const std::string& text, EspInputStats& out) {
    // ESP32 reportInputStats 固定两种格式（两条消息是同一份统计的两半）：
    //   "inp rx=%llu k=%u b=%u inv=%llu u=%llu"
    //   "inp2 r=%llu sk=%llu sb=%llu"
    // 严格前缀匹配（"inp " 与 "inp2 " 不互相碰撞），缺失字段增量合并，
    // 避免 "inp2" 消息把 rx 等字段清零覆盖（否则 waitStats 永远等不到 rx>=N）。
    EspInputStats s = out;
    if (text.rfind("inp ", 0) == 0) {
        unsigned long long rx = 0, inv = 0, u = 0;
        unsigned k = 0, b = 0, st = 0;
        if (std::sscanf(text.c_str(), "inp rx=%llu k=%u b=%u inv=%llu u=%llu st=%u", &rx, &k,
                        &b, &inv, &u, &st) == 6) {
            s.rx = rx;
            s.k = k;
            s.b = b;
            s.inv = inv;
            s.u = u;
            s.st = st;
            s.seen = true;
            out = s;
            return true;
        }
        return false;
    }
    if (text.rfind("inp2 ", 0) == 0) {
        unsigned long long r = 0, sk = 0, sb = 0;
        if (std::sscanf(text.c_str(), "inp2 r=%llu sk=%llu sb=%llu", &r, &sk, &sb) == 3) {
            s.r = r;
            s.sk = sk;
            s.sb = sb;
            s.seen = true;
            out = s;
            return true;
        }
        return false;
    }
    // M4：ESP32 会话统计（ERROR 文本通道，≤64B/行）：
    //   "sess  st=%u h=txh/rxh p=txp/rxp"
    //   "sess2 q=txPong/rxPong e=decoderErrors c=crcErrors s=seqGaps"
    if (text.rfind("sess ", 0) == 0) {
        unsigned long long txh = 0, rxh = 0, txp = 0, rxp = 0;
        unsigned st = 0;
        if (std::sscanf(text.c_str(), "sess st=%u h=%llu/%llu p=%llu/%llu", &st, &txh,
                        &rxh, &txp, &rxp) == 5) {
            s.st = st;
            s.txh = txh;
            s.rxh = rxh;
            s.txp = txp;
            s.rxp = rxp;
            s.seen = true;
            out = s;
            return true;
        }
        return false;
    }
    if (text.rfind("sess2 ", 0) == 0) {
        unsigned long long tqo = 0, rqo = 0, dec = 0, crc = 0, seqg = 0;
        if (std::sscanf(text.c_str(), "sess2 q=%llu/%llu e=%llu c=%llu s=%llu", &tqo, &rqo,
                        &dec, &crc, &seqg) == 5) {
            s.tqo = tqo;
            s.rqo = rqo;
            s.dec = dec;
            s.crc = crc;
            s.seqg = seqg;
            s.seen = true;
            out = s;
            return true;
        }
        return false;
    }
    return false;
}

// ---- 命令脚本步骤 ----
struct Step {
    enum class Kind : uint8_t { kSendEvent = 0, kSleep = 1 };
    Kind kind = Kind::kSendEvent;
    espview::input::InputEvent ev;
    uint64_t sleepMs = 0;
};

// 键名 → HostKey（a-z / 0-9 / 功能 / 控制 / 方向 / 导航 / 修饰 / 锁）
espview::input::HostKey hostKeyFromName(const std::string& name) {
    using espview::input::HostKey;
    if (name.size() == 1) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<HostKey>(static_cast<int>(HostKey::kA) + (c - 'a'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<HostKey>(static_cast<int>(HostKey::k0) + (c - '0'));
        }
    }
    struct N {
        const char* name;
        HostKey key;
    };
    static const N table[] = {
        {"f1", HostKey::kF1},   {"f2", HostKey::kF2},   {"f3", HostKey::kF3},
        {"f4", HostKey::kF4},   {"f5", HostKey::kF5},   {"f6", HostKey::kF6},
        {"f7", HostKey::kF7},   {"f8", HostKey::kF8},   {"f9", HostKey::kF9},
        {"f10", HostKey::kF10}, {"f11", HostKey::kF11}, {"f12", HostKey::kF12},
        {"enter", HostKey::kEnter}, {"escape", HostKey::kEscape}, {"esc", HostKey::kEscape},
        {"tab", HostKey::kTab}, {"backspace", HostKey::kBackspace}, {"space", HostKey::kSpace},
        {"left", HostKey::kLeft}, {"right", HostKey::kRight}, {"up", HostKey::kUp},
        {"down", HostKey::kDown}, {"home", HostKey::kHome}, {"end", HostKey::kEnd},
        {"pageup", HostKey::kPageUp}, {"pagedown", HostKey::kPageDown},
        {"insert", HostKey::kInsert}, {"delete", HostKey::kDelete},
        {"ctrl", HostKey::kLeftCtrl}, {"control", HostKey::kLeftCtrl},
        {"shift", HostKey::kLeftShift}, {"alt", HostKey::kLeftAlt}, {"gui", HostKey::kLeftGui},
        {"capslock", HostKey::kCapsLock}, {"numlock", HostKey::kNumLock},
        {"scrolllock", HostKey::kScrollLock},
    };
    for (const N& n : table) {
        if (name == n.name) {
            return n.key;
        }
    }
    return HostKey::kUnknown;
}

uint16_t modifierBitOf(espview::input::HostKey hk) {
    using espview::input::HostKey;
    switch (hk) {
        case HostKey::kLeftCtrl: case HostKey::kRightCtrl: return espview::input::kModCtrl;
        case HostKey::kLeftShift: case HostKey::kRightShift: return espview::input::kModShift;
        case HostKey::kLeftAlt: case HostKey::kRightAlt: return espview::input::kModAlt;
        case HostKey::kLeftGui: case HostKey::kRightGui: return espview::input::kModGui;
        default: return 0;
    }
}

uint8_t mouseBitFromName(const std::string& name) {
    if (name == "left") return espview::input::kMouseLeft;
    if (name == "right") return espview::input::kMouseRight;
    if (name == "middle") return espview::input::kMouseMiddle;
    return 0;
}

// 解析一行 --cmd；成功返回 true 并把步骤追加到 out。
bool parseCommand(const std::string& cmd, std::vector<Step>& out, uint16_t& buttons,
                  uint16_t& mods, uint16_t& x, uint16_t& y) {
    const auto split = [](const std::string& s, char sep) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : s) {
            if (c == sep) {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        parts.push_back(cur);
        return parts;
    };
    const auto tok = split(cmd, ' ');
    auto push = [&out](espview::input::InputEvent e) {
        Step s;
        s.kind = Step::Kind::kSendEvent;
        s.ev = e;
        out.push_back(s);
    };

    if (tok.empty()) {
        return false;
    }
    if (tok[0] == "sleep" && tok.size() == 2) {
        Step s;
        s.kind = Step::Kind::kSleep;
        s.sleepMs = static_cast<uint64_t>(std::strtoull(tok[1].c_str(), nullptr, 10));
        out.push_back(s);
        return true;
    }
    if (tok[0] == "key") {
        if (tok.size() < 2) {
            return false;
        }
        const std::string name = tok[1];
        // 组合键：key ctrl+a（两段式）；普通键：key a down/up（三段式）
        const size_t plus = name.find('+');
        if (plus != std::string::npos) {
            if (tok.size() != 2) {
                return false;
            }
            const std::string modPart = name.substr(0, plus);
            const std::string keyPart = name.substr(plus + 1);
            const espview::input::HostKey keyHk = hostKeyFromName(keyPart);
            espview::input::KeyMapResult km;
            if (!espview::input::KeyboardMapper::mapKey(keyHk, km)) {
                std::fprintf(stderr, "  [cmd] unsupported key in combo: %s\n", keyPart.c_str());
                return false;
            }
            std::vector<espview::input::HostKey> modKeys;
            uint16_t modMask = 0;
            for (const std::string& m : split(modPart, '+')) {
                const espview::input::HostKey hk = hostKeyFromName(m);
                if (modifierBitOf(hk) == 0) {
                    std::fprintf(stderr, "  [cmd] bad modifier: %s\n", m.c_str());
                    return false;
                }
                modKeys.push_back(hk);
                modMask = static_cast<uint16_t>(modMask | modifierBitOf(hk));
            }
            // Ctrl Down → A Down → A Up → Ctrl Up（spec §8：独立 InputEvent，不拼协议事件）
            for (const espview::input::HostKey hk : modKeys) {
                espview::input::KeyMapResult mr;
                espview::input::KeyboardMapper::mapKey(hk, mr);
                push(espview::input::makeKeyEvent(espview::input::InputType::kKeyDown, mr.hidUsage,
                                                  modMask, 0));
            }
            push(espview::input::makeKeyEvent(espview::input::InputType::kKeyDown, km.hidUsage,
                                              modMask, 0));
            push(espview::input::makeKeyEvent(espview::input::InputType::kKeyUp, km.hidUsage,
                                              modMask, 0));
            for (auto it = modKeys.rbegin(); it != modKeys.rend(); ++it) {
                espview::input::KeyMapResult mr;
                espview::input::KeyboardMapper::mapKey(*it, mr);
                push(espview::input::makeKeyEvent(espview::input::InputType::kKeyUp, mr.hidUsage,
                                                  modMask, 0));
            }
            mods = modMask;
            return true;
        }
        if (tok.size() != 3) {
            return false;
        }
        const std::string dir = tok[2];
        const espview::input::HostKey hk = hostKeyFromName(name);
        espview::input::KeyMapResult km;
        if (!espview::input::KeyboardMapper::mapKey(hk, km)) {
            std::fprintf(stderr, "  [cmd] unsupported key: %s\n", name.c_str());
            return false;
        }
        const bool down = dir == "down";
        if (!down && dir != "up") {
            return false;
        }
        if (km.modifierBit != 0) {
            mods = down ? static_cast<uint16_t>(mods | km.modifierBit)
                        : static_cast<uint16_t>(mods & ~km.modifierBit);
        }
        push(espview::input::makeKeyEvent(down ? espview::input::InputType::kKeyDown
                                               : espview::input::InputType::kKeyUp,
                                          km.hidUsage, mods, 0));
        return true;
    }
    if (tok[0] == "mouse" && tok.size() >= 3) {
        if (tok[1] == "move") {
            const size_t comma = tok[2].find(',');
            if (comma == std::string::npos) {
                return false;
            }
            const int px = std::atoi(tok[2].substr(0, comma).c_str());
            const int py = std::atoi(tok[2].substr(comma + 1).c_str());
            if (px < 0 || px > 319 || py < 0 || py > 239) {
                std::fprintf(stderr, "  [cmd] mouse coord out of 320x240: %d,%d\n", px, py);
                return false;
            }
            x = static_cast<uint16_t>(px);
            y = static_cast<uint16_t>(py);
            push(espview::input::makeMouseMove(x, y, static_cast<uint8_t>(buttons), 0));
            return true;
        }
        const uint8_t bit = mouseBitFromName(tok[1]);
        if (bit == 0 || tok.size() < 3) {
            return false;
        }
        const std::string dir = tok[2];
        if (dir == "down") {
            buttons = static_cast<uint16_t>(buttons | bit);
            push(espview::input::makeMouseButton(espview::input::InputType::kMouseDown, x, y,
                                                 static_cast<uint8_t>(buttons), 0));
            return true;
        }
        if (dir == "up") {
            buttons = static_cast<uint16_t>(buttons & ~bit);
            push(espview::input::makeMouseButton(espview::input::InputType::kMouseUp, x, y,
                                                 static_cast<uint8_t>(buttons), 0));
            return true;
        }
        if (dir == "click") {
            buttons = static_cast<uint16_t>(buttons | bit);
            push(espview::input::makeMouseButton(espview::input::InputType::kMouseDown, x, y,
                                                 static_cast<uint8_t>(buttons), 0));
            buttons = static_cast<uint16_t>(buttons & ~bit);
            push(espview::input::makeMouseButton(espview::input::InputType::kMouseUp, x, y,
                                                 static_cast<uint8_t>(buttons), 0));
            return true;
        }
        return false;
    }
    if (tok[0] == "wheel" && tok.size() == 2) {
        const int n = std::atoi(tok[1].c_str());
        if (n < -128 || n > 127) {
            return false;
        }
        push(espview::input::makeMouseWheel(x, y, static_cast<int8_t>(n),
                                            static_cast<uint8_t>(buttons), 0));
        return true;
    }
    return false;
}

// ---- 测试上下文 ----
struct Args {
    std::string port = "COM3";
    uint32_t baud = 115200;
    std::vector<std::string> cmds;
    bool reconnect = false;
    bool largeFrameCheck = false;
    bool reset = true;
    uint32_t timeoutMs = 120000;
    uint32_t reconnectWaitMs = 8000;
    uint32_t stepDelayMs = 150;
};

struct Harness {
    Args args;
    HostUartTransport transport;
    ByteQueue queue;
    std::unique_ptr<ProtocolEndpoint> ep;

    bool connected = false;
    bool helloInitiated = false;
    uint64_t openMs = 0;
    uint64_t txEvents = 0;
    uint64_t txDropped = 0;
    std::vector<SessionState> states;
    EspInputStats lastStats;
    std::vector<EspInputStats> statsHistory;
    std::vector<std::string> errors;

    // 大帧检测（large-frame-check）
    bool largeFrameInFlight = false;
    uint64_t largeBeginMs = 0;
    uint64_t largeCommitMs = 0;
    uint64_t largeByteHint = 0;
    uint64_t commits = 0;
    bool sawLargeBegin = false;

    // M4 diagnosis: max gap between consecutive CRC-passed packets (RX stall detect).
    uint64_t lastPktSeenMs = 0;
    uint64_t maxRxGapMs = 0;
    uint64_t lastPktCount = 0;
    uint64_t maxRxGapAtMs = 0;

    void resetSessionFlags() {
        connected = false;
        helloInitiated = false;
    }
};

void initEndpoint(Harness& h) {
    EndpointConfig cfg;
    cfg.protocol_version = espview::proto::kProtocolVersion;
    cfg.device_class = 0;
    cfg.width = 320;
    cfg.height = 240;
    cfg.pixel_format = espview::proto::PixelFormat::kRgb565;
    cfg.mode_mask = 0b111;
    cfg.device_name = "espview-pc-input";

    auto sink = [&h](const uint8_t* d, size_t n) -> SendStatus {
        return h.transport.send(d, n) ? SendStatus::kOk : SendStatus::kError;
    };

    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&h](SessionState s) {
        h.states.push_back(s);
        if (s == SessionState::kConnected) {
            h.connected = true;
        } else if (s == SessionState::kDisconnected) {
            h.connected = false;
        }
    };
    cb.onProtocolError = [&h](SessionError e, std::string_view d) {
        std::printf("  [E] protocol error %d: %.*s\n", static_cast<int>(e),
                    static_cast<int>(d.size()), d.data());
        (void)h;
    };
    cb.onHello = [](const HelloInfo&) {};
    cb.onFrameBegin = [&h](const espview::proto::FrameBeginInfo& b) {
        if (b.byteHint >= 100000u) {  // 153600B 大帧
            h.sawLargeBegin = true;
            h.largeFrameInFlight = true;
            h.largeBeginMs = nowMs();
            h.largeByteHint = b.byteHint;
            std::printf("  [F] LARGE frame begin: id=%u hint=%u @ t=%llu ms\n",
                        static_cast<unsigned>(b.frameId), static_cast<unsigned>(b.byteHint),
                        static_cast<unsigned long long>(nowMs()));
        }
    };
    cb.onFrameCommit = [&h](const espview::proto::CommittedFrame& f) {
        ++h.commits;
        if (h.largeFrameInFlight) {
            h.largeFrameInFlight = false;
            h.largeCommitMs = nowMs();
            std::printf("  [F] LARGE frame committed: id=%u bytes=%u @ t=%llu ms\n",
                        static_cast<unsigned>(f.frameId), static_cast<unsigned>(f.byteCount),
                        static_cast<unsigned long long>(nowMs()));
        }
    };
    cb.onFrameDiscard = [&h](espview::proto::FrameDiscardReason r) {
        std::printf("  [F] frame discard reason=%d\n", static_cast<int>(r));
        (void)h;
    };
    cb.onError = [&h](ErrorCode code, std::string_view text) {
        std::printf("  [E] ERROR code=%u text=%.*s\n", static_cast<unsigned>(code),
                    static_cast<int>(text.size()), text.data());
        // 直接增量合并到 h.lastStats：inp / inp2 两条消息是同一份统计的两半，
        // inp2 只更新 r/sk/sb，不能把 rx 等字段清零。
        if (parseEspStats(std::string(text), h.lastStats)) {
            h.statsHistory.push_back(h.lastStats);
        }
        h.errors.emplace_back(text);
    };
    h.ep = std::make_unique<ProtocolEndpoint>(cfg, sink, cb, nowMs);
}

void wireTransport(Harness& h) {
    h.transport.setDataCallback([&h](const uint8_t* d, size_t n) { h.queue.push(d, n); });
    h.transport.setStateCallback([&h](HostUartTransport::State s) {
        if (s == HostUartTransport::State::Disconnected || s == HostUartTransport::State::Error) {
            if (h.ep) {
                h.ep->onTransportDisconnected();
            }
        }
        // Connected 不触发 onTransportConnected：被动握手（等对端 HELLO）。
    });
}

bool openPort(Harness& h, bool resetPulse) {
    HostUartTransport::Config cfg;
    cfg.port = h.args.port;
    cfg.baud = h.args.baud;
    cfg.read_timeout_ms = 50;
    cfg.reset_on_open = resetPulse;
    h.resetSessionFlags();
    h.openMs = nowMs();
    return h.transport.open(cfg);
}

void closePort(Harness& h) {
    h.transport.close();
    h.resetSessionFlags();
}

// 主循环：drain + tick + 连接维持 + 场景阶段谓词。
bool pumpLoop(Harness& h, uint64_t timeoutMs, const std::function<bool()>& phase,
              const char* what) {
    const uint64_t start = nowMs();
    std::printf("  [wait] %s (%.1fs)\n", what, static_cast<double>(timeoutMs) / 1000.0);
    while (nowMs() - start < timeoutMs) {
        std::vector<uint8_t> chunk;
        while (h.queue.popAll(chunk)) {
            h.ep->onTransportData(chunk.data(), chunk.size());
        }
        // M4 diagnosis: RX gap tracking (packetsRx increments on every CRC-passed packet).
        {
            const uint64_t pkts = h.ep->stats().packetsRx;
            if (pkts != h.lastPktCount) {
                const uint64_t now2 = nowMs();
                if (h.lastPktCount != 0 && h.lastPktSeenMs != 0 && now2 > h.lastPktSeenMs) {
                    const uint64_t gap = now2 - h.lastPktSeenMs;
                    if (gap > h.maxRxGapMs) {
                        h.maxRxGapMs = gap;
                        h.maxRxGapAtMs = now2;
                    }
                    if (gap >= 1000) {
                        std::printf("  [gap] %.3f s without a valid packet @ t=%llu ms\n",
                                    static_cast<double>(gap) / 1000.0,
                                    static_cast<unsigned long long>(now2));
                    }
                }
                h.lastPktCount = pkts;
                h.lastPktSeenMs = nowMs();
            }
        }
        h.ep->tick();
        if (!h.connected && !h.helloInitiated && nowMs() - h.openMs >= 7000) {
            h.helloInitiated = true;
            h.ep->onTransportConnected();
            std::printf("  [H] passive HELLO not seen in 7s -> initiating HELLO\n");
        }
        if (phase()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::printf("  [wait] TIMEOUT: %s\n", what);
    return false;
}

bool waitConnected(Harness& h, uint64_t timeoutMs) {
    return pumpLoop(h, timeoutMs, [&h]() { return h.connected; }, "HELLO handshake -> CONNECTED");
}

// 发送一个 InputEvent（经共享 codec + endpoint；fire-and-forget）。
bool sendEvent(Harness& h, const espview::input::InputEvent& e) {
    if (!h.connected) {
        ++h.txDropped;
        return false;
    }
    const auto msg = espview::input::encodeInputEvent(e, 319, 239);
    if (!msg.has_value()) {
        ++h.txDropped;
        return false;
    }
    if (h.ep->sendMessage(*msg) == SendResult::kOk) {
        ++h.txEvents;
        return true;
    }
    ++h.txDropped;
    return false;
}

bool runScript(Harness& h) {
    uint16_t buttons = 0;
    uint16_t mods = 0;
    uint16_t x = 160;
    uint16_t y = 120;
    std::vector<Step> steps;
    for (const std::string& cmd : h.args.cmds) {
        if (!parseCommand(cmd, steps, buttons, mods, x, y)) {
            std::fprintf(stderr, "  [cmd] bad command: %s\n", cmd.c_str());
            return false;
        }
    }
    std::printf("  [run] executing %zu steps (delay %u ms)\n", steps.size(), h.args.stepDelayMs);
    for (const Step& s : steps) {
        if (!h.connected) {
            std::printf("  [run] aborted: session lost mid-script\n");
            return false;
        }
        if (s.kind == Step::Kind::kSleep) {
            const uint64_t end = nowMs() + s.sleepMs;
            while (nowMs() < end) {
                std::vector<uint8_t> chunk;
                while (h.queue.popAll(chunk)) {
                    h.ep->onTransportData(chunk.data(), chunk.size());
                }
                h.ep->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            continue;
        }
        std::printf("  [send] %s %s", s.ev.isKey() ? "KEY" : "MOUSE",
                    s.ev.type == espview::input::InputType::kKeyDown ? "down" :
                    s.ev.type == espview::input::InputType::kKeyUp ? "up" :
                    s.ev.type == espview::input::InputType::kMouseDown ? "down" :
                    s.ev.type == espview::input::InputType::kMouseUp ? "up" :
                    s.ev.type == espview::input::InputType::kMouseWheel ? "wheel" : "move");
        if (s.ev.isKey()) {
            std::printf(" keycode=0x%02X mods=0x%02X\n", static_cast<unsigned>(s.ev.keycode),
                        static_cast<unsigned>(s.ev.modifiers));
        } else {
            std::printf(" x=%u y=%u buttons=0x%02X wheel=%d\n", static_cast<unsigned>(s.ev.x),
                        static_cast<unsigned>(s.ev.y), static_cast<unsigned>(s.ev.buttons),
                        static_cast<int>(s.ev.wheelDelta));
        }
        if (!sendEvent(h, s.ev)) {
            std::fprintf(stderr, "  [send] FAILED\n");
            return false;
        }
        const uint64_t end = nowMs() + h.args.stepDelayMs;
        while (nowMs() < end) {
            std::vector<uint8_t> chunk;
            while (h.queue.popAll(chunk)) {
                h.ep->onTransportData(chunk.data(), chunk.size());
            }
            h.ep->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    std::printf("  [run] script done: txEvents=%llu dropped=%llu\n",
                static_cast<unsigned long long>(h.txEvents),
                static_cast<unsigned long long>(h.txDropped));
    return true;
}

// 打印 ESP32 最近一次输入统计 + RTT。
void printStatsSnapshot(Harness& h, const char* tag) {
    const EspInputStats& s = h.lastStats;
    std::printf("  [stats:%s] rx=%llu k=%llu b=%llu inv=%llu u=%llu r=%llu sk=%llu sb=%llu"
                " st=%llu rtt=%u ms\n",
                tag, static_cast<unsigned long long>(s.rx),
                static_cast<unsigned long long>(s.k), static_cast<unsigned long long>(s.b),
                static_cast<unsigned long long>(s.inv), static_cast<unsigned long long>(s.u),
                static_cast<unsigned long long>(s.r), static_cast<unsigned long long>(s.sk),
                static_cast<unsigned long long>(s.sb), static_cast<unsigned long long>(s.st),
                static_cast<unsigned>(h.ep->stats().rtt.lastMs.value_or(0)));
}

bool waitStats(Harness& h, uint64_t timeoutMs, const std::function<bool(const EspInputStats&)>& p,
               const char* what) {
    return pumpLoop(h, timeoutMs, [&h, &p]() { return h.lastStats.seen && p(h.lastStats); }, what);
}

// ---- 场景：默认脚本 ----
bool scenarioScript(Harness& h) {
    if (!runScript(h)) {
        std::printf("  [S] FAIL: script execution failed\n");
        return false;
    }
    // 等待下一次 ESP32 统计上报，确认事件已到达。
    const uint64_t expectRx = h.txEvents;
    bool ok = waitStats(h, 15000, [&h, expectRx](const EspInputStats& s) {
        return s.rx >= expectRx && s.k == 0 && s.b == 0;
    }, "ESP32 input stats reflect sent events (keys/buttons released)");
    if (!ok) {
        printStatsSnapshot(h, "script");
        std::printf("  [S] FAIL: ESP32 stats not confirmed\n");
        return false;
    }
    printStatsSnapshot(h, "script");
    std::printf("  [S] PASS: %llu input events received by ESP32, no stuck state\n",
                static_cast<unsigned long long>(h.lastStats.rx));
    return true;
}

// ---- 场景：断线重连 stuck-input 恢复 ----
bool scenarioReconnect(Harness& h) {
    // 按住 Ctrl + 左键（不发 up）
    if (!sendEvent(h, espview::input::makeKeyEvent(espview::input::InputType::kKeyDown, 0xE0,
                                                   espview::input::kModCtrl, 0)) ||
        !sendEvent(h, espview::input::makeMouseButton(espview::input::InputType::kMouseDown, 160,
                                                      120, espview::input::kMouseLeft, 0))) {
        std::printf("  [R] FAIL: hold-down send failed\n");
        return false;
    }
    std::printf("  [R] Ctrl+Left held (sent down only)\n");
    // 等 ESP32 确认收到（k=1 b=1）
    bool ok = waitStats(h, 25000, [](const EspInputStats& s) { return s.k == 1 && s.b == 1; },
                        "ESP32 sees ctrl+left held");
    if (!ok) {
        printStatsSnapshot(h, "hold");
        std::printf("  [R] FAIL: ESP32 did not confirm held state\n");
        return false;
    }
    printStatsSnapshot(h, "hold");

    closePort(h);
    std::printf("  [R] closed COM3; waiting %.1fs for peer timeout + resetState ...\n",
                static_cast<double>(h.args.reconnectWaitMs) / 1000.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(h.args.reconnectWaitMs));
    if (!openPort(h, /*resetPulse=*/false)) {
        std::printf("  [R] FAIL: reopen failed\n");
        return false;
    }
    if (!waitConnected(h, 15000)) {
        std::printf("  [R] FAIL: reconnect handshake timeout\n");
        return false;
    }
    std::printf("  [R] PASS: HELLO re-handshake OK\n");

    // 重连后：ESP32 InputManager.resetState() 已执行（r>=1），pressed=0 / buttons=0
    ok = waitStats(h, 25000, [](const EspInputStats& s) { return s.r >= 1 && s.k == 0 && s.b == 0; },
                   "ESP32 resetState: keys=0 buttons=0");
    if (!ok) {
        printStatsSnapshot(h, "reconnect");
        std::printf("  [R] FAIL: stuck input not recovered after reconnect\n");
        return false;
    }
    printStatsSnapshot(h, "reconnect");
    std::printf("  [R] PASS: reconnect -> resetState() -> pressed keys=0, buttons=0\n");
    return true;
}

// ---- 场景：大帧传输期间输入不被阻塞 ----
bool scenarioLargeFrame(Harness& h) {
    bool ok = pumpLoop(h, h.args.timeoutMs, [&h]() { return h.sawLargeBegin; },
                       "TestPattern large FULL begin");
    if (!ok) {
        std::printf("  [L] FAIL: no large frame observed\n");
        return false;
    }
    const uint64_t tLargeBegin = h.largeBeginMs;
    // 大帧流式期间发送两组按键（115200 下 153600B ≈ 13s）
    const bool sentA = sendEvent(h, espview::input::makeKeyEvent(
                                        espview::input::InputType::kKeyDown, 0x04, 0, 0)) &&
                       sendEvent(h, espview::input::makeKeyEvent(
                                        espview::input::InputType::kKeyUp, 0x04, 0, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const bool sentB = sendEvent(h, espview::input::makeKeyEvent(
                                        espview::input::InputType::kKeyDown, 0x05, 0, 0)) &&
                       sendEvent(h, espview::input::makeKeyEvent(
                                        espview::input::InputType::kKeyUp, 0x05, 0, 0));
    if (!sentA || !sentB) {
        std::printf("  [L] FAIL: key send during large frame failed\n");
        return false;
    }
    std::printf("  [L] sent KEY A + KEY B during large frame @ t=%llu ms\n",
                static_cast<unsigned long long>(nowMs()));

    // 大帧必须仍然完整提交（输入穿插不破坏显示路径）。
    ok = pumpLoop(h, 30000, [&h]() { return h.largeCommitMs != 0; }, "large frame commits");
    if (!ok) {
        std::printf("  [L] FAIL: large frame did not commit\n");
        return false;
    }
    const uint64_t tLargeCommit = h.largeCommitMs;

    // ESP32 统计必须显示在帧传输期间已收到输入（rx >= 4：A down/up + B down/up）。
    ok = waitStats(h, 15000, [&h](const EspInputStats& s) { return s.rx >= 4 && s.k == 0 && s.b == 0; },
                   "ESP32 received inputs during large frame (no block)");
    if (!ok) {
        printStatsSnapshot(h, "large");
        std::printf("  [L] FAIL: inputs not received by ESP32 during large frame\n");
        return false;
    }
    printStatsSnapshot(h, "large");
    std::printf("  [L] PASS: large frame %.1f KB streamed %llu ms; inputs interleaved without"
                " blocking; frame committed\n",
                static_cast<double>(h.largeByteHint) / 1024.0,
                static_cast<unsigned long long>(tLargeCommit - tLargeBegin));
    return true;
}

void usage(const char* prog) {
    std::printf(
        "usage: %s --port COM3 --baud 115200 [--cmd <cmd> ...] [options]\n"
        "  --cmd <cmd>          脚本命令（可重复；见文件头命令列表）\n"
        "  --reconnect          断线重连 stuck-input 恢复验收\n"
        "  --large-frame-check  大帧传输期间输入不阻塞验收\n"
        "  --no-reset           不触发 DTR/RTS 复位\n"
        "  --timeout-ms N       场景总超时（默认 120000）\n"
        "  --reconnect-wait-ms N 断线后等对端超时时长（默认 8000）\n"
        "  --step-delay-ms N    脚本步骤间隔（默认 150）\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--port") {
            a.port = next("--port");
        } else if (arg == "--baud") {
            a.baud = static_cast<uint32_t>(std::stoul(next("--baud")));
        } else if (arg == "--cmd") {
            a.cmds.push_back(next("--cmd"));
        } else if (arg == "--reconnect") {
            a.reconnect = true;
        } else if (arg == "--large-frame-check") {
            a.largeFrameCheck = true;
        } else if (arg == "--no-reset") {
            a.reset = false;
        } else if (arg == "--timeout-ms") {
            a.timeoutMs = static_cast<uint32_t>(std::stoul(next("--timeout-ms")));
        } else if (arg == "--reconnect-wait-ms") {
            a.reconnectWaitMs = static_cast<uint32_t>(std::stoul(next("--reconnect-wait-ms")));
        } else if (arg == "--step-delay-ms") {
            a.stepDelayMs = static_cast<uint32_t>(std::stoul(next("--step-delay-ms")));
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 2;
        }
    }
    if (a.cmds.empty() && !a.reconnect && !a.largeFrameCheck) {
        std::fprintf(stderr, "no --cmd / --reconnect / --large-frame-check given\n");
        usage(argv[0]);
        return 2;
    }

    Harness h;
    h.args = a;
    initEndpoint(h);
    wireTransport(h);

    const uint64_t t0 = nowMs();
    std::printf("== M3 input_send_test: port=%s baud=%u reset=%s reconnect=%d large=%d ==\n",
                a.port.c_str(), a.baud, a.reset ? "yes" : "no", a.reconnect ? 1 : 0,
                a.largeFrameCheck ? 1 : 0);

    if (!openPort(h, a.reset)) {
        std::printf("FAIL: cannot open %s\n", a.port.c_str());
        return 1;
    }
    if (!waitConnected(h, 15000)) {
        std::printf("FAIL: initial HELLO handshake timeout\n");
        return 1;
    }
    std::printf("  [H] PASS: HELLO handshake, peer=%ux%u fmt=%d name=%s\n",
                h.ep->peerHello().width, h.ep->peerHello().height,
                static_cast<int>(h.ep->peerHello().pixel_format),
                h.ep->peerHello().device_name.c_str());

    bool pass = false;
    if (a.reconnect) {
        pass = scenarioReconnect(h);
    } else if (a.largeFrameCheck) {
        pass = scenarioLargeFrame(h);
    } else {
        pass = scenarioScript(h);
    }

    // 收尾 drain + 最终统计
    std::vector<uint8_t> chunk;
    while (h.queue.popAll(chunk)) {
        h.ep->onTransportData(chunk.data(), chunk.size());
    }
    h.ep->tick();
    closePort(h);

    const auto& s = h.ep->stats();
    std::printf("\n==== M3 统计 ====\n");
    std::printf("  elapsed: %.3f s\n", static_cast<double>(nowMs() - t0) / 1000.0);
    std::printf("  input events tx: %llu   tx dropped: %llu\n",
                static_cast<unsigned long long>(h.txEvents),
                static_cast<unsigned long long>(h.txDropped));
    std::printf("  endpoint: rxMessages=%llu txMessages=%llu decoderErrors=%llu"
                " sessionErrors=%llu\n",
                static_cast<unsigned long long>(s.rxMessages),
                static_cast<unsigned long long>(s.txMessages),
                static_cast<unsigned long long>(s.decoderErrors),
                static_cast<unsigned long long>(s.errors));
    // M4：PC 侧心跳可观察（大帧期间 PONG best-effort → rtt 无测量，但绝无误断线）
    std::printf("  heartbeat: pingTx=%llu pingRx=%llu pongTx=%llu pongRx=%llu timeouts=%llu"
                " rttValid=%d rtt=%u min=%u avg=%u max=%u n=%llu\n",
                static_cast<unsigned long long>(s.txPing),
                static_cast<unsigned long long>(s.rxPing),
                static_cast<unsigned long long>(s.txPong),
                static_cast<unsigned long long>(s.rxPong),
                static_cast<unsigned long long>(s.heartbeatTimeouts),
                s.rtt.lastMs.has_value() ? 1 : 0,
                static_cast<unsigned>(s.rtt.lastMs.value_or(0)),
                static_cast<unsigned>(s.rtt.minMs), static_cast<unsigned>(s.rtt.avgMs),
                static_cast<unsigned>(s.rtt.maxMs),
                static_cast<unsigned long long>(s.rtt.samples));
    std::printf("  packets: rx=%llu crcErrors=%llu seqGaps=%llu chunkErrors=%llu\n",
                static_cast<unsigned long long>(s.packetsRx),
                static_cast<unsigned long long>(s.crcErrors),
                static_cast<unsigned long long>(s.seqGaps),
                static_cast<unsigned long long>(s.chunkErrors));
    std::printf("  rx-gap: max=%llu ms @ t=%llu ms\n",
                static_cast<unsigned long long>(h.maxRxGapMs),
                static_cast<unsigned long long>(h.maxRxGapAtMs));
    if (h.lastStats.st == 3) {
        std::printf("  esp32 sess: st=%llu hello=%llu/%llu ping=%llu/%llu pong=%llu/%llu"
                    " dec=%llu crc=%llu seq=%llu\n",
                    static_cast<unsigned long long>(h.lastStats.st),
                    static_cast<unsigned long long>(h.lastStats.txh),
                    static_cast<unsigned long long>(h.lastStats.rxh),
                    static_cast<unsigned long long>(h.lastStats.txp),
                    static_cast<unsigned long long>(h.lastStats.rxp),
                    static_cast<unsigned long long>(h.lastStats.tqo),
                    static_cast<unsigned long long>(h.lastStats.rqo),
                    static_cast<unsigned long long>(h.lastStats.dec),
                    static_cast<unsigned long long>(h.lastStats.crc),
                    static_cast<unsigned long long>(h.lastStats.seqg));
    }
    printStatsSnapshot(h, "final");
    std::printf("  RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
