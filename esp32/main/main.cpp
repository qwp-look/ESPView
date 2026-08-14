// ESPView ESP32 演示程序（M1-2 会话层 + M3 输入 + M5-A LVGL / TestPattern 应用选择）。
//
// 会话层（ProtocolEndpoint）职责：
//   UartTransport → StreamDecoder → Message → ProtocolEndpoint → 本文件（Application 回调）
//   Application → ProtocolEndpoint.sendMessage → MessageEncoder → UartTransport
//
// 应用选择（menuconfig "ESPView Application"）：
//   - CONFIG_ESPVIEW_APP_LVGL（默认，M5-A）：LVGL 真实 UI（background + button +
//     label + 1Hz counter），dirty rect 经 RemoteDisplay → 协议 → PC 窗口；
//   - CONFIG_ESPVIEW_APP_TESTPATTERN（M1-3C/M2/M3/M4 硬件回归）：确定性帧脚本，
//     com3_frame_test / input_send_test 验收工具依赖。
//
// 注意：console UART 已禁用（GPIO9/10 安全审查结论），ESP_LOGI 输出被丢弃；
//   验收数据由 PC 脚本读取协议字节 / ERROR 文本通道完成。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "espview/tcp_transport.hpp"
#include "espview/uart_transport.hpp"
#include "transport_manager.h"           // shared/transport：TransportManager
#include "transport_sink.h"              // shared/transport：TransportSink（paced/unpaced 统一收口）
#include "espview/transport_manager.hpp" // ESP32 UART/TCP adapter（旧 ITransport -> 新 ITransport）
#include "encoder.h"  // IMessagePayloadSource（StreamingSender）
#include "input_codec.h"
#include "input_manager.h"
#include "message.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#if CONFIG_ESPVIEW_APP_LVGL
#include "lvgl_port/lvgl_port.hpp"
#else
#include "testpattern/test_pattern.hpp"
#endif

#include <memory>

using namespace espview;
using namespace espview::proto;

static const char* kTag = "espview_main";

namespace {

// M6-C：运行时 Transport 选择（§三/§五）。initial 仍由 menuconfig 决定
// （编译期初始选择）；运行时经 TransportManager::switchTo() 在 UART/TCP 间切换。
// 上层（ProtocolEndpoint / LVGL / InputManager / 会话状态机）只面对
// shared/transport 抽象（ITransport + capabilities + 发送门），完全不知道
// 当前是 UART 还是 TCP（§十八/§七）。
espview::transport::TransportManager g_mgr(
    [](espview::transport::TransportType t)
        -> std::shared_ptr<espview::transport::ITransport> {
        if (t == espview::transport::TransportType::kUart) {
            ::espview::UartTransportConfig cfg;  // menuconfig 默认值（CONFIG_ESPVIEW_UART_*）
            return std::make_shared<espview::transport::Esp32UartAdapter>(cfg);
        }
        ::espview::TcpTransportConfig cfg;
#if CONFIG_ESPVIEW_TRANSPORT_TCP
        cfg.server_ip = CONFIG_ESPVIEW_TCP_SERVER_IP;
        cfg.server_port = static_cast<uint16_t>(CONFIG_ESPVIEW_TCP_SERVER_PORT);
        cfg.connect_timeout_ms = CONFIG_ESPVIEW_TCP_CONNECT_TIMEOUT_MS;
        cfg.reconnect_delay_ms = CONFIG_ESPVIEW_TCP_RECONNECT_DELAY_MS;
        cfg.rx_buf = CONFIG_ESPVIEW_TCP_RX_BUF;
        cfg.wifi.ssid = CONFIG_ESPVIEW_WIFI_SSID;
        cfg.wifi.password = CONFIG_ESPVIEW_WIFI_PASSWORD;
#if defined(CONFIG_ESPVIEW_WIFI_PS_NONE) && CONFIG_ESPVIEW_WIFI_PS_NONE
        cfg.wifi.ps_none = true;
#endif
#endif
        if (cfg.wifi.ssid[0] == '\0') {
            ESP_LOGE(kTag, "Wi-Fi SSID not configured; TCP unavailable (menuconfig / local sdkconfig)");
            return nullptr;
        }
        return std::make_shared<espview::transport::Esp32TcpAdapter>(cfg);
    },
#if CONFIG_ESPVIEW_TRANSPORT_TCP
    espview::transport::TransportType::kTcp
#else
    espview::transport::TransportType::kUart
#endif
);
DisplayMode g_currentMode = DisplayMode::kWindow;
// M6-E §22：最近一次 Transport 状态快照（onTransportState 更新；供 statsLoop 诊断行）。
std::atomic<uint8_t> g_transportState{0};  // ITransport::State::kDisconnected == 0
// M3：ESP32 侧输入汇聚（RX 任务 feed / 会话任务 resetState，内部互斥）。
espview::input::InputManager g_inputManager(320, 240);

// M6-C/M6-D test-only：F12（HID 0x45）按下 → 请求运行时 Transport 切换（UART/TCP）
// 的验收钩子（CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH，生产固件置 n）。
// 切换请求由 sessionLoop 消费执行（避免在 RX/输入处理栈内关闭当前 Transport）。
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
std::atomic<bool> g_debugSwitchPending{false};
// M6-D 诊断（ERROR 文本通道，非 wire 格式）：发送门/切换活性标记。
std::atomic<uint64_t> g_sinkEntryMs{0};
std::atomic<uint64_t> g_sinkExitMs{0};
std::atomic<bool> g_sinkActive{false};
std::atomic<uint64_t> g_switchEntryMs{0};
std::atomic<uint64_t> g_switchExitMs{0};
std::atomic<bool> g_switchActive{false};
TaskHandle_t g_sessionTask = nullptr;
TaskHandle_t g_statsTask = nullptr;
#endif

// g_endpoint / g_testPattern / g_lvgl / monotonicMs 定义在文件下方；回调与 sink 提前引用需要前向声明。
extern ProtocolEndpoint g_endpoint;
#if CONFIG_ESPVIEW_APP_LVGL
extern std::unique_ptr<espview::LvglPort> g_lvgl;
#else
extern std::unique_ptr<TestPattern> g_testPattern;
#endif
uint64_t monotonicMs();

// M6-C：paced/unpaced 发送统一收口（§八–§十）。所有发送经 TransportManager
// 发送门串行化；paced（UART）背压时按 wire 速率重试，unpaced（TCP）单次尝试
// （send() 内部 sendAll 已按 socket 缓冲/超时背压）。alive：会话已死则放弃。
espview::transport::TransportSink g_sink(
    g_mgr,
    []() { return g_endpoint.state() != SessionState::kDisconnected; },
    monotonicMs,
    [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); });

// ---- M3 Test IInputListener：安全 debug channel（console 禁用时无输出；
//      统计经 ERROR 消息上报，不污染协议 UART）----
class TestInputListener : public espview::input::IInputListener {
public:
    void onInputEvent(const espview::input::InputEvent& e) override {
        if (e.isKey()) {
            ESP_LOGI(kTag, "INPUT: type=%s key=0x%02X mods=0x%02X",
                     e.type == espview::input::InputType::kKeyDown ? "KeyDown" : "KeyUp",
                     static_cast<unsigned>(e.keycode), static_cast<unsigned>(e.modifiers));
        } else if (e.type == espview::input::InputType::kMouseWheel) {
            ESP_LOGI(kTag, "INPUT: type=MouseWheel x=%u y=%u buttons=%u wheel=%d",
                     static_cast<unsigned>(e.x), static_cast<unsigned>(e.y),
                     static_cast<unsigned>(e.buttons), static_cast<int>(e.wheelDelta));
        } else {
            const char* t = e.type == espview::input::InputType::kMouseDown
                                ? "MouseDown"
                                : (e.type == espview::input::InputType::kMouseUp ? "MouseUp"
                                                                                  : "MouseMove");
            ESP_LOGI(kTag, "INPUT: type=%s x=%u y=%u buttons=%u",
                     t, static_cast<unsigned>(e.x), static_cast<unsigned>(e.y),
                     static_cast<unsigned>(e.buttons));
        }
    }
};

// ---- Application 回调（ProtocolEndpoint → 本文件）----
void onSessionState(SessionState s) {
    ESP_LOGI(kTag, "session state -> %d", static_cast<int>(s));
    if (s == SessionState::kDisconnected) {
        // M3 spec §18：断线/会话重置 → 本地安全恢复（pressed keys/buttons 全部
        // 补发 release，清空状态；绝不回发 PC）。
        g_inputManager.resetState();
    }
#if CONFIG_ESPVIEW_APP_LVGL
    if (g_lvgl) {
        g_lvgl->onSessionState(s);
    }
#else
    if (g_testPattern) {
        g_testPattern->onSessionState(s);
    }
#endif
}

void onProtocolError(SessionError e, std::string_view detail) {
    ESP_LOGE(kTag, "protocol error %d: %.*s", static_cast<int>(e),
             static_cast<int>(detail.size()), detail.data());
}

void onHello(const HelloInfo& hello) {
    ESP_LOGI(kTag, "peer HELLO: ver=%d %ux%u fmt=%d mask=0x%02X name=%.*s",
             static_cast<int>(hello.protocol_version), static_cast<int>(hello.width),
             static_cast<int>(hello.height), static_cast<int>(hello.pixel_format),
             static_cast<int>(hello.mode_mask), static_cast<int>(hello.device_name.size()),
             hello.device_name.data());
}

// SET_MODE 请求（ACK_REQ）：Application 校验后回 ACK（M1-2 无真实 Display backend）。
void onSetModeRequest(uint8_t type, const std::vector<uint8_t>& payload, uint16_t ackSeq) {
    if (type != static_cast<uint8_t>(MessageType::kSetMode) || payload.size() != 1) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        return;
    }
    const uint8_t mode = payload[0];
    if (mode > static_cast<uint8_t>(DisplayMode::kMirror)) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        ESP_LOGW(kTag, "SET_MODE invalid mode=%u -> ACK ERR", static_cast<unsigned>(mode));
        return;
    }
    g_currentMode = static_cast<DisplayMode>(mode);
    ESP_LOGI(kTag, "SET_MODE OK mode=%u -> ACK", static_cast<unsigned>(mode));
    g_endpoint.acknowledge(ackSeq, 0, ErrorCode::kNone);
}

void onAck(uint16_t ackSeq, uint8_t status, ErrorCode errorCode) {
    ESP_LOGI(kTag, "ACK seq=%u status=%u err=%u", static_cast<unsigned>(ackSeq),
             static_cast<unsigned>(status), static_cast<unsigned>(errorCode));
}

void onError(ErrorCode code, std::string_view text) {
    ESP_LOGI(kTag, "ERROR code=%u: %.*s", static_cast<unsigned>(code),
             static_cast<int>(text.size()), text.data());
}

// M3：ProtocolEndpoint 未消费的消息（INPUT_KEY / INPUT_MOUSE）→ InputManager。
void onOtherMessage(const Message& msg) {
    const auto ev = espview::input::decodeInputMessage(msg, 319, 239);  // 320x240
    if (ev.has_value()) {
        espview::input::InputEvent e = *ev;
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
        // M6-D test-only：F12 按下 → 请求运行时 Transport 切换（UART↔TCP，
        // 验收钩子；生产固件 CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH=n 时该键
        // 走正常输入链路）。调试键不进入输入链路。
        if (e.type == espview::input::InputType::kKeyDown && e.keycode == 0x45u) {
            g_debugSwitchPending.store(true);
            return;
        }
#endif
        e.timestampMs = monotonicMs();  // ESP32 本地接收时间（RTT 记录，不上 wire）
        g_inputManager.feed(e);
    } else if (msg.type == static_cast<uint8_t>(MessageType::kInputKey) ||
               msg.type == static_cast<uint8_t>(MessageType::kInputMouse)) {
        g_inputManager.noteInvalidInput();  // 输入消息本身非法（payload/flags/坐标）
    }
}

// M3：输入统计经 ERROR 消息上报（≤64B/条；供 PC input_send_test 解析）。
void reportInputStats() {
    const espview::input::InputStats st = g_inputManager.stats();
    char buf[64];
    const auto clamp = [](uint64_t v) -> uint64_t { return v < 99999u ? v : 99999u; };
    std::snprintf(buf, sizeof(buf), "inp rx=%llu k=%u b=%u inv=%llu u=%llu st=%u",
                  static_cast<unsigned long long>(clamp(st.eventsReceived)),
                  static_cast<unsigned>(st.pressedKeys < 255u ? st.pressedKeys : 255u),
                  static_cast<unsigned>(st.pressedButtons),
                  static_cast<unsigned long long>(clamp(st.invalidEvents)),
                  static_cast<unsigned long long>(clamp(st.unsupportedEvents)),
                  static_cast<unsigned>(g_endpoint.state()));
    const auto m1 = makeError(ErrorCode::kNone, buf);
    if (m1.has_value()) {
        g_endpoint.sendMessage(*m1);
    }
    std::snprintf(buf, sizeof(buf), "inp2 r=%llu sk=%llu sb=%llu",
                  static_cast<unsigned long long>(clamp(st.resetCount)),
                  static_cast<unsigned long long>(clamp(st.stuckKeysReleased)),
                  static_cast<unsigned long long>(clamp(st.stuckButtonsReleased)));
    const auto m2 = makeError(ErrorCode::kNone, buf);
    if (m2.has_value()) {
        g_endpoint.sendMessage(*m2);
    }
    // M4：会话/心跳/协议统计（ERROR 文本通道，wire format 未修改）。
    //   ERROR 文本 ≤64B：拆成两行——
    //   "sess  st= h=txHello/rxHello p=txPing/rxPing"
    //   "sess2 q=txPong/rxPong e=decoderErrors c=crcErrors s=seqGaps"
    const SessionStats& sst = g_endpoint.stats();
    std::snprintf(buf, sizeof(buf), "sess st=%u h=%llu/%llu p=%llu/%llu",
                  static_cast<unsigned>(g_endpoint.state()),
                  static_cast<unsigned long long>(clamp(sst.txHello)),
                  static_cast<unsigned long long>(clamp(sst.rxHello)),
                  static_cast<unsigned long long>(clamp(sst.txPing)),
                  static_cast<unsigned long long>(clamp(sst.rxPing)));
    const auto m3 = makeError(ErrorCode::kNone, buf);
    if (m3.has_value()) {
        g_endpoint.sendMessage(*m3);
    }
    std::snprintf(buf, sizeof(buf), "sess2 q=%llu/%llu e=%llu c=%llu s=%llu",
                  static_cast<unsigned long long>(clamp(sst.txPong)),
                  static_cast<unsigned long long>(clamp(sst.rxPong)),
                  static_cast<unsigned long long>(clamp(sst.decoderErrors)),
                  static_cast<unsigned long long>(clamp(sst.crcErrors)),
                  static_cast<unsigned long long>(clamp(sst.seqGaps)));
    const auto m4 = makeError(ErrorCode::kNone, buf);
    if (m4.has_value()) {
        g_endpoint.sendMessage(*m4);
    }

    // M6-D 诊断（ERROR 文本通道）：发送门/切换活性 + 会话/统计任务状态。
    //   dbg2 s=sinkActive sinkAgeMs w=switchActive switchAgeMs ts=sessionTaskState tr=statsTaskState
    const uint64_t nowDbg = monotonicMs();
    const auto clampA = [](uint64_t v) -> unsigned { return v < 99999u ? static_cast<unsigned>(v) : 99999u; };
    const uint64_t sinkAge = g_sinkActive.load() && g_sinkEntryMs.load() != 0
                                 ? (nowDbg > g_sinkEntryMs.load() ? nowDbg - g_sinkEntryMs.load() : 0)
                                 : 0;
    const uint64_t swAge = g_switchActive.load() && g_switchEntryMs.load() != 0
                               ? (nowDbg > g_switchEntryMs.load() ? nowDbg - g_switchEntryMs.load() : 0)
                               : 0;
    std::snprintf(buf, sizeof(buf), "dbg2 s%d %us w%d %us ts%d tr%d q%u x%u y%u",
                  g_sinkActive.load() ? 1 : 0, clampA(sinkAge),
                  g_switchActive.load() ? 1 : 0, clampA(swAge),
                  g_sessionTask != nullptr ? static_cast<int>(eTaskGetState(g_sessionTask)) : -1,
                  g_statsTask != nullptr ? static_cast<int>(eTaskGetState(g_statsTask)) : -1,
#if CONFIG_ESPVIEW_APP_LVGL
                  g_lvgl ? clampA(static_cast<uint64_t>(g_lvgl->debugState().queued)) : 0u,
                  g_lvgl ? clampA(g_lvgl->lastFlushCbExitMs() / 1000u) : 0u,
                  g_lvgl ? clampA(g_lvgl->lastPumpEntryMs() / 1000u) : 0u);
#else
                  0u, 0u, 0u);
#endif
    const auto m5 = makeError(ErrorCode::kNone, buf);
    if (m5.has_value()) {
        g_endpoint.sendMessage(*m5);
    }
}

void onFrameCommit(const CommittedFrame& frame) {
    ESP_LOGI(kTag, "frame commit id=%u type=%d %ux%u rects=%u bytes=%u",
             static_cast<unsigned>(frame.frameId), static_cast<int>(frame.frameType),
             static_cast<unsigned>(frame.width), static_cast<unsigned>(frame.height),
             static_cast<unsigned>(frame.rectCount), static_cast<unsigned>(frame.byteCount));
}

void onFrameDiscard(FrameDiscardReason reason) {
    ESP_LOGI(kTag, "frame discard reason=%d", static_cast<int>(reason));
}

// ---- Transport 事件 → ProtocolEndpoint ----
#if CONFIG_ESPVIEW_APP_LVGL
void updateFlushWaitFromTransport();  // 定义在下方（onTransportState 提前引用）
#endif
void onTransportData(const uint8_t* data, size_t len) { g_endpoint.onTransportData(data, len); }

void onTransportState(espview::transport::ITransport::State s) {
    g_transportState.store(static_cast<uint8_t>(s));  // M6-E §22：供 statsLoop 诊断
    ESP_LOGI(kTag, "transport state -> %d", static_cast<int>(s));
    if (s == espview::transport::ITransport::State::kConnected) {
        g_endpoint.onTransportConnected();
#if CONFIG_ESPVIEW_APP_LVGL
        updateFlushWaitFromTransport();  // M6-C：UART/TCP 切换后调整 flush 预算
#endif
    } else if (s == espview::transport::ITransport::State::kDisconnected ||
               s == espview::transport::ITransport::State::kError) {
        g_endpoint.onTransportDisconnected();
    }
}

// M6-C §十二：按当前 Transport capabilities 换算 LVGL flush 等待预算。
//   paced（UART 115200）：保持 menuconfig 基线（CONFIG_ESPVIEW_LVGL_FLUSH_WAIT_MS）；
//   unpaced（TCP）：LAN 大帧远快于 250ms，超时 = 整帧丢弃 + FULL resync。
#if CONFIG_ESPVIEW_APP_LVGL
void updateFlushWaitFromTransport() {
    if (!g_lvgl) {
        return;
    }
    const espview::transport::TransportCapabilities caps = g_mgr.capabilities();
    // M6-D 修正：TCP FULL 实测 ~235-254ms，250ms 预算过紧（flush_cb 排队等待超时
    // -> dropPendingFrame -> FULL resync 被 ABORTED -> 静态 UI 不再重绘 -> 30s 看门狗）；
    // FULL 预算放大到 2s（含重同步余量），PARTIAL 保持 250ms（丢弃即 FULL resync）。
    const uint64_t fullWait = caps.paced ? static_cast<uint64_t>(CONFIG_ESPVIEW_LVGL_FLUSH_WAIT_MS)
                                       : 2000u;
    const uint64_t partialWait = caps.paced ? static_cast<uint64_t>(CONFIG_ESPVIEW_LVGL_FLUSH_WAIT_MS)
                                         : 250u;
    g_lvgl->setFlushWaitMs(fullWait, partialWait);
}
#endif

// ---- 会话驱动：每 200ms tick（心跳 2s / 对端超时 5s / ACK 重试 500ms）----
// 注意：本任务绝不做阻塞式发送。流式大帧（153608B FRAME_RECT）会持有
// sendMutex_ 十余秒，任何阻塞式 sendMessage 都会让 tick() 饿死（心跳停摆、
// 对端超时无法发现 → M1-3B reconnect 回归问题）。统计上报已移到独立任务。
// M6-D test-only：F12 调试钩子执行体（sessionLoop 上下文；§二十/§二十一）。
// switchTo 内部按 Disconnected → Connected 顺序重放状态回调：上层完成会话重置
//（decoder/frame/ACK/seq/InputManager 清零）+ HELLO + FULL resync（§五/§六）。
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
void debugTransportSwitch() {
    g_switchEntryMs.store(monotonicMs());
    g_switchActive.store(true);
    // M6-D 修正：切换前先作废当前会话（endpoint 置 Disconnected → alive 检查立即
    // 失败）——TX pump 的在途流式消息（UART FULL 最长持有 sendMutex_ ~13s）马上
    // 中止，旧消息残包不会跨到新 Transport；新 FULL resync 也不会因 pump 被旧流
    // 占用而排队超时被 flush_cb 丢弃（UART→TCP 切换失败根因，M6-D）。
    g_endpoint.onTransportDisconnected();
    const espview::transport::TransportType next =
        g_mgr.current() == espview::transport::TransportType::kUart
            ? espview::transport::TransportType::kTcp
            : espview::transport::TransportType::kUart;
    const bool ok = g_mgr.switchTo(next);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "trx sw=%d ok=%d n=%u",
                  static_cast<int>(next), ok ? 1 : 0,
                  static_cast<unsigned>(g_mgr.switchCount()));
    g_switchExitMs.store(monotonicMs());
    g_switchActive.store(false);
    const auto m = makeError(ErrorCode::kNone, buf);
    if (m.has_value()) {
        g_endpoint.sendMessage(*m);
    }
}
#endif  // CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH

// ---- 会话驱动：每 200ms tick（心跳 2s / 对端超时 5s / ACK 重试 500ms）----
// 注意：本任务绝不做阻塞式发送。流式大帧（153608B FRAME_RECT）会持有
// sendMutex_ 十余秒，任何阻塞式 sendMessage 都会让 tick() 饿死（心跳停摆、
// 对端超时无法发现 → M1-3B reconnect 回归问题）。统计上报已移到独立任务。
void sessionLoop(void*) {
    g_sessionTask = xTaskGetCurrentTaskHandle();
    while (true) {
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
        if (g_debugSwitchPending.exchange(false)) {
            debugTransportSwitch();
        }
#endif
        g_endpoint.tick();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---- M3 输入统计上报（独立低优先级任务）----
// 统计走阻塞式 sendMessage：与流式大帧竞争 sendMutex_ 时会等待（最长一帧
// 传输时间），但只阻塞本 debug 任务，不影响会话状态机。
void statsLoop(void*) {
    g_statsTask = xTaskGetCurrentTaskHandle();
    uint64_t lastStatsMs = 0;
    while (true) {
        const uint64_t now = monotonicMs();
        if (now - lastStatsMs >= 3000) {
            lastStatsMs = now;
            reportInputStats();  // M3：输入统计定期上报（ERROR 消息，fire-and-forget）
            // M6-E §22：Transport 诊断行（ERROR 文本通道，非 wire 格式）：
            //   trx tr=<0|1> st=<0..3> sw=<switchCount> rc=<reconnectCount>
            //   tx=<txBytes> rx=<rxBytes> rssi=<rssi|-128> ch=<ch|0>
            // 计数可见地 clamp 到 4 位（≤9999）：运行时 ≤64B（makeError 限制）。
            {
                char trxBuf[96];
                auto* t = g_mgr.transport();
                const uint64_t rcV = t != nullptr ? t->reconnectCount() : 0;
                const uint64_t txV = t != nullptr ? t->txBytes() : 0;
                const uint64_t rxV = t != nullptr ? t->rxBytes() : 0;
                const uint32_t swC = g_mgr.switchCount() < 9999u ? g_mgr.switchCount() : 9999u;
                const uint32_t rcC = rcV < 9999u ? static_cast<uint32_t>(rcV) : 9999u;
                const uint32_t txC = txV < 9999u ? static_cast<uint32_t>(txV) : 9999u;
                const uint32_t rxC = rxV < 9999u ? static_cast<uint32_t>(rxV) : 9999u;
                int8_t rssi = -128;
                uint8_t ch = 0;
                const bool apOk = t != nullptr && t->wifiApInfo(&rssi, &ch);
                std::snprintf(trxBuf, sizeof(trxBuf),
                              "trx tr=%d st=%u sw=%u rc=%u tx=%u rx=%u rssi=%d ch=%u",
                              g_mgr.current() == espview::transport::TransportType::kTcp ? 1 : 0,
                              static_cast<unsigned>(g_transportState.load()),
                              static_cast<unsigned>(swC),
                              static_cast<unsigned>(rcC),
                              static_cast<unsigned>(txC),
                              static_cast<unsigned>(rxC),
                              apOk ? static_cast<int>(rssi) : -128,
                              apOk ? static_cast<unsigned>(ch) : 0u);
                const auto mTrx = makeError(ErrorCode::kNone, trxBuf);
                if (mTrx.has_value()) {
                    g_endpoint.sendMessage(*mTrx);
                }
            }
#if CONFIG_ESPVIEW_APP_LVGL
            if (g_lvgl) {
                g_lvgl->reportStats();  // M5-A：显示/堆统计（disp/disp2 行）
            }
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---- Transport 发送适配（M1-3B：paced sink）----
// UART TX ring buffer（8KB）远小于 153KB 级帧：直接返回背压会导致整帧丢弃。
// TestPattern 是 test-only 应用（无 UI 线程），允许按 UART 排空速率逐包重试
// （Transport 语义不变：仍返回 ESP_ERR_TIMEOUT 表示缓冲满；重试是应用层策略）。
// 会话 DISCONNECTED（对端断线/超时）时立即放弃，不向虚空继续发送。
// M6-C：shared/transport SendStatus → proto::SendStatus 显式映射
// （两端接口同构但类型不同；禁止依赖数值相等）。
SendStatus mapTransportSend(espview::transport::SendStatus r) {
    switch (r) {
        case espview::transport::SendStatus::kOk: return SendStatus::kOk;
        case espview::transport::SendStatus::kBackpressure: return SendStatus::kBackpressure;
        case espview::transport::SendStatus::kError: return SendStatus::kError;
        case espview::transport::SendStatus::kNotConnected: return SendStatus::kNotConnected;
    }
    return SendStatus::kError;
}

SendStatus transportSink(const uint8_t* data, size_t len) {
    g_sinkEntryMs.store(monotonicMs());
    g_sinkActive.store(true);
    const SendStatus r = mapTransportSend(g_sink.send(data, len));
    g_sinkActive.store(false);
    g_sinkExitMs.store(monotonicMs());
    return r;
}

// ---- M4：非阻塞控制发送（PONG/ACK 回复、心跳 PING、ACK 重试专用）----
// 单次尝试：TX 缓冲满立即返回背压（放弃本次 best-effort 回复），绝不进入
// transportSink 的重试循环。原因：tryTransmit 由 RX 任务/会话 tick 调用，
// 若在这里等待 TX ring 排空，会阻塞 UART RX 读取（大帧期间输入丢包）并
// 长时间持有 sendMutex_（帧流停滞 → 对端 5s 超时误判断开）。
SendStatus trySink(const uint8_t* data, size_t len) {
    // M6-C：单次尽力（PONG/ACK/PING/ACK 重试专用）：门忙/缓冲满立即返回背压，
    // 绝不进入 UART 式重试循环（阻塞 RX 线程/会话 tick）。
    g_sinkEntryMs.store(monotonicMs());
    g_sinkActive.store(true);
    const SendStatus r = mapTransportSend(g_sink.trySend(data, len));
    g_sinkActive.store(false);
    g_sinkExitMs.store(monotonicMs());
    return r;
}

// ---- Endpoint 全局配置（一次构造，不可重新赋值）----
const EndpointConfig kEndpointCfg = [] {
    EndpointConfig c;
    c.protocol_version = kProtocolVersion;
    c.device_class = 0;
    c.width = 320;
    c.height = 240;
    c.pixel_format = PixelFormat::kRgb565;
    c.mode_mask = 0b111;  // WINDOW | DEVICE | MIRROR
    c.device_name = "espview-esp32";
    return c;
}();

uint64_t monotonicMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

ProtocolEndpoint::Callbacks kEndpointCallbacks = [] {
    ProtocolEndpoint::Callbacks c;
    c.onSessionState = onSessionState;
    c.onProtocolError = onProtocolError;
    c.onFrameCommit = onFrameCommit;
    c.onFrameDiscard = onFrameDiscard;
    c.onHello = onHello;
    c.onAckRequest = onSetModeRequest;
    c.onAck = onAck;
    c.onError = onError;
    c.onOtherMessage = onOtherMessage;  // M3：INPUT_* 输入通道
    return c;
}();

ProtocolEndpoint g_endpoint(kEndpointCfg, transportSink, trySink, kEndpointCallbacks, monotonicMs);

#if CONFIG_ESPVIEW_APP_LVGL
// M5-A LVGL Port：display driver + flush_cb + TX 任务 + 统计。
std::unique_ptr<espview::LvglPort> g_lvgl;
#else
// M1-3B TestPattern：CONNECTED 后自动发送确定性帧序列（test-only）。
std::unique_ptr<TestPattern> g_testPattern;
#endif

}  // namespace

extern "C" void app_main() {
#if CONFIG_ESPVIEW_TRANSPORT_TCP
    ESP_LOGI(kTag, "ESPView: initial transport=TCP server=%s:%d (runtime switch via TransportManager)",
             CONFIG_ESPVIEW_TCP_SERVER_IP, CONFIG_ESPVIEW_TCP_SERVER_PORT);
#else
    ESP_LOGI(kTag, "ESPView: initial transport=UART port=%d baud=%d (runtime switch via TransportManager)",
             CONFIG_ESPVIEW_UART_PORT, CONFIG_ESPVIEW_UART_BAUD);
#endif

#if CONFIG_ESPVIEW_APP_LVGL
    // M5-A：LVGL 应用（真实 UI → dirty rect → RemoteDisplay → 协议 → PC 窗口）。
    // 发送回调同时持有普通 Message 与 Streaming 两条路径（与 TestPattern 同构）。
    g_lvgl = std::make_unique<espview::LvglPort>(
        [](const Message& msg) { return g_endpoint.sendMessage(msg); },
        [](const MessageHeader& header, IMessagePayloadSource& source) {
            return g_endpoint.sendMessageStreaming(header, source);
        });
    g_lvgl->start();
#else
    // M1-3C：TestPattern 同时持有普通 Message 发送与 Streaming 发送两条路径；
    // 大 RECT（153608B）走 sendMessageStreaming，其余小消息走 sendMessage。
    g_testPattern = std::make_unique<TestPattern>(
        [](const Message& msg) { return g_endpoint.sendMessage(msg); },
        [](const MessageHeader& header, IMessagePayloadSource& source) {
            return g_endpoint.sendMessageStreaming(header, source);
        });
    g_testPattern->start();
#endif

    // M3：注册输入监听。LVGL 应用：InputManager → LvglInputAdapter（RX 任务只写
    // adapter 状态，LVGL 任务 read_cb 轮询消费，§7）；TestPattern 应用保留
    // 安全 debug channel（console 禁用时无输出，统计经 ERROR 消息上报）。
#if CONFIG_ESPVIEW_APP_LVGL
    g_inputManager.registerListener(g_lvgl->inputListener());
#else
    static TestInputListener s_inputListener;
    g_inputManager.registerListener(&s_inputListener);
#endif

    // M6-C：状态/数据回调接 TransportManager（切换时自动转发 + 状态重放）。
    g_mgr.setDataCallback(onTransportData);
    g_mgr.setStateCallback(onTransportState);

    // 打开初始 Transport（编译期选择；运行时可用 g_mgr.switchTo() 切换）。
    if (!g_mgr.open()) {
        ESP_LOGE(kTag, "transport open failed: %s", g_mgr.lastError());
        return;
    }
    ESP_LOGI(kTag, "transport open OK: type=%d mtu=%zu",
             static_cast<int>(g_mgr.current()),
             g_mgr.transport() != nullptr ? g_mgr.transport()->mtu() : 0);
#if CONFIG_ESPVIEW_APP_LVGL
    updateFlushWaitFromTransport();
#endif

    xTaskCreate(sessionLoop, "espview_sess", 4096, nullptr, 5, nullptr);
    xTaskCreate(statsLoop, "espview_stat", 4096, nullptr, 3, nullptr);

    // 主循环：周期打印会话统计（console 禁用时无输出，仅保留结构）。
    uint32_t loop = 0;
    while (true) {
        const SessionStats& st = g_endpoint.stats();
        if ((loop % 25) == 0) {
            ESP_LOGI(kTag,
                     "state=%d mode=%d txHello=%llu rxHello=%llu txPing=%llu rxPing=%llu "
                     "txPong=%llu rxPong=%llu ackSent=%llu ackRx=%llu errors=%llu "
                     "decErr=%llu rtt=%ums",
                     static_cast<int>(g_endpoint.state()), static_cast<int>(g_currentMode),
                     static_cast<unsigned long long>(st.txHello),
                     static_cast<unsigned long long>(st.rxHello),
                     static_cast<unsigned long long>(st.txPing),
                     static_cast<unsigned long long>(st.rxPing),
                     static_cast<unsigned long long>(st.txPong),
                     static_cast<unsigned long long>(st.rxPong),
                     static_cast<unsigned long long>(st.ackSent),
                     static_cast<unsigned long long>(st.ackReceived),
                     static_cast<unsigned long long>(st.errors),
                     static_cast<unsigned long long>(st.decoderErrors),
                     static_cast<unsigned>(st.rtt.lastMs.value_or(0)));
        }
        ++loop;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
