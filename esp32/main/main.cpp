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
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"  // heap_caps_get_largest_free_block / MALLOC_CAP_DEFAULT
#include "esp_system.h"     // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#include "esp_rom_sys.h"      // M7-F：esp_rom_get_cpu_ticks_per_us（v6.0.2 esp_clk_cpu_freq 已移入 esp_private）
#include "esp_rom_spiflash.h" // M7-F：g_rom_flashchip.chip_size（v6.0.2 已移除 esp_get_flash_size）
#if CONFIG_ESPVIEW_OLED_ENABLE
#include "esp_netif.h"
#include "oled/oled_display.hpp"
#include "oled/physical_display_sink.hpp"  // M7-C2：PhysicalDisplaySink（IDisplaySink）
#endif

#include "display_router.h"              // M7-C2：DisplayRouter / PhysicalScene（shared/display）
#include "espview/tcp_transport.hpp"
#include "espview/uart_transport.hpp"
#include "espview/wifi_provisioning.hpp"
#include "transport_manager.h"           // shared/transport：TransportManager
#include "transport_sink.h"              // shared/transport：TransportSink（paced/unpaced 统一收口）
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
// M7-F：boot 电学观测（软件可见侧；最小侵入，无协议改动）。console=none 生产
// profile 无输出为预期（oled_diag 等 console profile 可见）。绝不打印任何
// Wi-Fi 凭据（SSID/密码/server IP 属敏感信息）。
const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN: return "UNKNOWN";
        case ESP_RST_POWERON: return "POWERON_RESET";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        case ESP_RST_EFUSE: return "EFUSE";
        case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
        default: return "UNKNOWN";
    }
}

// 单阶段 boot 诊断行：uptime（esp_timer）+ free heap。
void logBootStage(const char* stage) {
    ESP_LOGI(kTag, "BOOT stage=%s uptime_ms=%llu free_heap=%u",
             stage,
             static_cast<unsigned long long>(esp_timer_get_time() / 1000),
             static_cast<unsigned>(esp_get_free_heap_size()));
}

// ---- M7-G：TCP handoff 目标/编排状态（UART bootstrap → TCP client）----
// 目标（serverIp/serverPort）来自 WIFI_CONFIG provisioning 凭据（AF.2），
// 不是 menuconfig 常量；所有读写均发生在会话任务（sessionLoop）上下文，
// g_mgr 工厂 lambda 仅在 switchTo 时（同一上下文）读取，无跨任务竞争。
// 绝不打印凭据/目标地址。
constexpr uint64_t kHandoffConnectTimeoutMs = 15000;  // 切换后首次 TCP 连接+会话窗口
constexpr uint64_t kHandoffDropTimeoutMs = 30000;     // 已连后掉线重连窗口
enum class HandoffPhase : uint8_t { kIdle = 0, kConnecting = 1, kConnected = 2 };
bool g_handoffActive = false;       // 工厂 lambda 分支开关（仅会话上下文读写）
HandoffPhase g_handoff = HandoffPhase::kIdle;
char g_handoffServerIp[16] = {};    // 点分 IPv4（工厂读取）
uint16_t g_handoffServerPort = 0;
uint64_t g_handoffStageStartMs = 0;  // 当前阶段起点（monotonicMs）
bool g_handoffDisabled = false;      // ServerUnreachable 后锁存；新 CONFIG/CLEAR 解锁

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
            return std::make_shared<UartTransport>(cfg);
        }
        ::espview::TcpTransportConfig cfg;
        if (g_handoffActive) {
            // M7-G：provisioning 驱动的 TCP handoff —— 目标来自 WIFI_CONFIG 凭据
            // （AF.2 serverIp/serverPort；非 menuconfig 常量），Wi-Fi 驱动由
            // provisioning 持有（adopt：不重复 init，直接复用已连接驱动）。
            cfg.server_ip = g_handoffServerIp;
            cfg.server_port = g_handoffServerPort;
            cfg.adopt_existing_wifi = true;
            if (cfg.server_ip[0] == '\0' || cfg.server_port == 0) {
                ESP_LOGE(kTag, "handoff target missing; TCP unavailable");
                return nullptr;
            }
            return std::make_shared<TcpTransport>(cfg);
        }
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
        if (cfg.wifi.ssid[0] == '\0') {
            ESP_LOGE(kTag, "Wi-Fi SSID not configured; TCP unavailable (menuconfig / local sdkconfig)");
            return nullptr;
        }
        return std::make_shared<TcpTransport>(cfg);
#else
        // 非 handoff 且非 TCP 构建：TCP 不可用（与旧行为一致：switchTo 失败）。
        ESP_LOGE(kTag, "TCP transport not available (CONFIG_ESPVIEW_TRANSPORT_TCP=n)");
        return nullptr;
#endif
    },
#if CONFIG_ESPVIEW_TRANSPORT_TCP
    espview::transport::TransportType::kTcp
#else
    espview::transport::TransportType::kUart
#endif
);
display::DisplayRouteMode g_currentMode = display::DisplayRouteMode::kVirtualOnly;
// M6-E §22：最近一次 Transport 状态快照（onTransportState 更新；供 statsLoop 诊断行）。
std::atomic<uint8_t> g_transportState{0};  // ITransport::State::kDisconnected == 0
// M8-A5（SINK-01）：会话状态原子快照（onSessionState 更新）。g_sink 的 alive
// 检查只读该 atomic，不触碰 g_endpoint —— 静态析构序/锁竞争零风险。
// SessionState::kDisconnected == 0（protocol_endpoint.h）。
std::atomic<uint8_t> g_sessionState{0};
// M3：ESP32 侧输入汇聚（RX 任务 feed / 会话任务 resetState，内部互斥）。
espview::input::InputManager g_inputManager(display::kVirtualDisplayGeometry.width,
                           display::kVirtualDisplayGeometry.height);

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
#endif
// M7-C2 test-only：F11（HID 0x44）按下 → 运行时循环 DisplayRouteMode 0..3
// （0→1→2→3→0）的验收钩子（CONFIG_ESPVIEW_TEST_MODE_SWITCH，生产固件置 n）。
// 请求由 sessionLoop 消费执行（避免在 RX/输入处理栈内切换 DisplayRouter 模式）。
#if CONFIG_ESPVIEW_TEST_MODE_SWITCH
std::atomic<bool> g_modeSwitchPending{false};
#endif
// M6-D 诊断：session/stats 任务句柄（供 TEST_TRANSPORT_SWITCH 调试命令报告任务状态；
// 任务循环无条件记录，故无条件声明；生产固件 =n 时仅赋值、不被读取）。
TaskHandle_t g_sessionTask = nullptr;
TaskHandle_t g_statsTask = nullptr;

// g_endpoint / g_testPattern / g_lvgl / monotonicMs 定义在文件下方；回调与 sink 提前引用需要前向声明。
extern ProtocolEndpoint g_endpoint;
// M7-D3：Wi-Fi Provisioning 全局（定义在 g_endpoint 之后；ACK 回调提前引用）。
extern WifiProvisioning g_wifiProv;
#if CONFIG_ESPVIEW_APP_LVGL
extern std::unique_ptr<espview::LvglPort> g_lvgl;
#else
extern std::unique_ptr<TestPattern> g_testPattern;
#endif
uint64_t monotonicMs();
#include "esp_heap_caps.h"  // heap_caps_get_largest_free_block / MALLOC_CAP_DEFAULT
#include "esp_system.h"     // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#if CONFIG_ESPVIEW_OLED_ENABLE
extern std::unique_ptr<espview::oled::OledDisplay> g_oled;  // 定义在文件下方
// M7-C2：PhysicalDisplaySink（定义在文件下方；SET_MODE/调试钩子提前引用）。
extern std::shared_ptr<espview::oled::PhysicalDisplaySink> g_physicalSink;
#endif
// M7-C2：DisplayRouter（定义在文件下方；SET_MODE/调试钩子提前引用）。
extern std::shared_ptr<display::DisplayRouter> g_router;

#include "esp_heap_caps.h"  // heap_caps_get_largest_free_block / MALLOC_CAP_DEFAULT
#include "esp_system.h"     // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#if CONFIG_ESPVIEW_OLED_ENABLE
// M7-A：OLED 状态快照 provider（main 侧填充；OLED 组件不理解字段含义）。
// 读取线程安全：g_endpoint.stats()/g_endpoint.state()/g_transportState 与
// statsLoop 同一约定（普通量/原子）；OLED 任务绝不触碰 protocol sendMutex /
// Transport。安全：绝不显示/打印 SSID/密码。
espview::oled::StatusSnapshot oledStatusSnapshot() {
    espview::oled::StatusSnapshot snap;
    const SessionStats& st = g_endpoint.stats();
    snap.sessionState = static_cast<uint8_t>(g_endpoint.state());
    snap.frameCount = g_endpoint.frameStats().commits();
    snap.sessionErrors = st.errors;
    snap.uptimeMs = monotonicMs();
    snap.freeHeap = esp_get_free_heap_size();
    snap.minFreeHeap = esp_get_minimum_free_heap_size();
    snap.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    // M7-B：统一走 diagSnapshot()（值语义，stateMutex_ 内读，switchTo 并发安全）。
    const espview::transport::TransportDiagSnapshot diag = g_mgr.diagSnapshot();
    const bool isTcp = diag.type == espview::transport::TransportType::kTcp;
    snap.transportType = isTcp ? 1u : 0u;
    snap.transportConnected = diag.connected;
    if (isTcp) {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ipInfo;
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ipInfo) == ESP_OK) {
            std::snprintf(snap.ip, sizeof(snap.ip), IPSTR, IP2STR(&ipInfo.ip));
        } else {
            std::snprintf(snap.ip, sizeof(snap.ip), "--");
        }
        snap.apInfoValid = diag.rssi != -128;
        snap.rssi = diag.rssi;
        snap.channel = diag.channel;
    } else {
        std::snprintf(snap.ip, sizeof(snap.ip), "--");
    }
    return snap;
}
#endif

// M6-C：paced/unpaced 发送统一收口（§八–§十）。所有发送经 TransportManager
// 发送门串行化；paced（UART）背压时按 wire 速率重试，unpaced（TCP）单次尝试
// （send() 内部 sendAll 已按 socket 缓冲/超时背压）。alive：会话已死则放弃。
espview::transport::TransportSink g_sink(
    g_mgr,
    []() { return g_sessionState.load(std::memory_order_acquire) != 0; },  // M8-A5（SINK-01）：只读原子快照
    monotonicMs,
    [](uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); return true; });  // M8-A5：Sleep 返回 bool

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
// M7-D1：CONNECTED 后每会话发送一次 CAPABILITIES（AD.2/AD.3；fire-and-forget，
// 不带 ACK_REQ）。数据源 = DisplayCapabilities sink 事实（display_capabilities.h）
// + OledDisplay 运行时事实（实际 I2C 地址/控制器）+ OledFb 常量（128x64 几何已在
// PhysicalDisplaySink::init 由 OledFb::kWidth/kHeight 落定）；modeMask 硬件派生
// （物理 sink 存在 → 0..3 全模式；否则仅 WINDOW），禁止编译期 0b1111 常量冒充。
void sendCapabilitiesMessage() {
    espview::proto::CapabilitiesInfo caps;
    caps.version = espview::proto::kCapabilitiesPayloadVersion;

    const auto vSink = g_router ? g_router->virtualSink() : nullptr;
    if (vSink) {
        const display::DisplayCapabilities& v = vSink->capabilities();
        caps.virtualPresent = true;
        caps.width = static_cast<uint16_t>(v.width);
        caps.height = static_cast<uint16_t>(v.height);
        caps.pixelFormat = v.format;
        caps.colorDepth = v.color;
        caps.virtualMono = v.mono;
        caps.virtualCanReadback = v.canReadback;
    }

#if CONFIG_ESPVIEW_OLED_ENABLE
    if (g_physicalSink) {
        const display::DisplayCapabilities& p = g_physicalSink->capabilities();
        caps.physicalPresent = true;
        caps.physWidth = static_cast<uint16_t>(p.width);
        caps.physHeight = static_cast<uint16_t>(p.height);
        // 物理输出为 1-bit 单色：physPixelFormat 由 mono 事实派生（AD.2 OLED=1）。
        caps.physPixelFormat = p.mono
                                   ? espview::proto::PhysicalPixelFormat::kMono1
                                   : espview::proto::PhysicalPixelFormat::kRgb565;
        caps.physColorDepth = p.color;
        caps.physMono = p.mono;
        caps.physCanReadback = p.canReadback;
        // 运行时事实（OledDisplay status：实际探测地址/控制器；未就绪时 kAuto/0）。
        if (g_oled) {
            const espview::oled::OledStatus os = g_oled->status();
            caps.physController =
                static_cast<espview::proto::CapabilitiesController>(os.controller);
            caps.physI2cAddress = os.address;
        }
        caps.sceneSupport = espview::proto::kSceneSupportApplication |
                            espview::proto::kSceneSupportDiagnostics;  // 0b11
    }
#endif
    // modeMask 硬件派生：物理 sink 存在 → WINDOW|DEVICE|MIRROR|SPLIT；否则仅 WINDOW。
    caps.modeMask = caps.physicalPresent ? 0x0Fu : 0x01u;

    const SendResult r = g_endpoint.sendCapabilities(caps);
    ESP_LOGI(kTag, "CAPABILITIES sent: %s (phys=%d %ux%u ctrl=%u addr=0x%02X mask=0x%02X)",
             r == SendResult::kOk ? "ok" : "err",
             caps.physicalPresent ? 1 : 0, static_cast<unsigned>(caps.physWidth),
             static_cast<unsigned>(caps.physHeight),
             static_cast<unsigned>(caps.physController),
             static_cast<unsigned>(caps.physI2cAddress),
             static_cast<unsigned>(caps.modeMask));
}

// ---- M7-D2：Physical Preview 上行（AE.3）----
// 默认 2Hz（500ms 节拍）；仅 CONNECTED + OLED 存在时发送。用 endpoint 的
// tryTransmit 路径（sendPhysicalPreview 内部）：锁忙/背压整帧丢弃，绝不阻塞
// 会话任务（M6-D：sessionLoop 不得阻塞在流式大帧的 sendMutex_ 之后）。
// 槽内容由 OLED 任务在内容确定点 store()（应用帧/诊断页快照）；帧去重/过期
// 由 PC 侧按 frameId 判定（AE.3）。transport 相关频率（115200→1Hz、
// 921600→5Hz、TCP→5-10Hz）为后续精调项，v0.1 固定默认 2Hz。
#if CONFIG_ESPVIEW_OLED_ENABLE
inline constexpr uint64_t kPreviewIntervalMs = 500;  // 默认 2Hz
void sendPhysicalPreviewMessage() {
    if (!g_oled || g_endpoint.state() != SessionState::kConnected) {
        return;
    }
    // M7-E：Wi-Fi 扫描挂起期间跳过（I2C/应用帧均静止；previewSlot 保持最后
    // 一帧，恢复后只发最新帧——AE.3 frameId 语义，无需 backlog 队列）。
    if (g_oled->isSuspendedForWifiScan()) {
        return;
    }
    // M8-A4 单编码路径：槽只提供稳定像素快照 + frameId（不重复编码 AE.2），
    // info 由本层直接组装，经 protocol makePhysicalPreview 编码一次发送
    // （消除 encode→parse→re-encode 往返与双编码器漂移，Agent E/L）。
    uint8_t previewPixels[espview::oled::OledPreviewSlot::kSizeBytes];
    uint16_t previewFrameId = 0;
    if (!g_oled->previewSlot().snapshot(previewPixels, previewFrameId)) {
        return;  // 槽无效（未开始刷新/已 reset）
    }
    espview::proto::PhysicalPreviewInfo info;
    info.frameId = previewFrameId;
    info.width = espview::oled::OledPreviewSlot::kWidth;
    info.height = espview::oled::OledPreviewSlot::kHeight;
    info.pixelFormat = espview::proto::PhysicalPixelFormat::kMono1;
    info.flags = 0;
    const auto r = g_endpoint.sendPhysicalPreview(info, previewPixels);
    ESP_LOGD(kTag, "PREVIEW sent r=%d id=%u", static_cast<int>(r),
             static_cast<unsigned>(info.frameId));
}
#endif  // CONFIG_ESPVIEW_OLED_ENABLE

void onSessionState(SessionState s) {
    g_sessionState.store(static_cast<uint8_t>(s), std::memory_order_release);  // M8-A5
    ESP_LOGI(kTag, "session state -> %d", static_cast<int>(s));
    if (s == SessionState::kDisconnected) {
        // M3 spec §18：断线/会话重置 → 本地安全恢复（pressed keys/buttons 全部
        // 补发 release，清空状态；绝不回发 PC）。
        g_inputManager.resetState();
        // M7-E：会话断开 → 中止进行中的扫描事务并恢复 OLED（无则 no-op）。
        g_wifiProv.notifySessionDisconnected();
    }
    // M7-D1：每会话 CONNECTED 后发送一次 CAPABILITIES（重连重发，AD.3）。
    if (s == SessionState::kConnected) {
        sendCapabilitiesMessage();
        // M7-G：会话（重）连接后重发当前 WIFI_STATUS —— 保证 TCP handoff
        // 失败回退 UART 后 kError/kServerUnreachable 必达向导（首发的背压/
        // 会话未就绪丢弃由本次补发覆盖）。
        g_wifiProv.republishStatus();
#if CONFIG_ESPVIEW_OLED_ENABLE
        // M7-D2（AE.3）：握手重置预览槽 frameId/清槽（重连后 PC 首帧无条件接受）。
        if (g_oled) {
            g_oled->previewSlot().reset();
        }
#endif
    }
#if CONFIG_ESPVIEW_OLED_ENABLE
    if (s == SessionState::kDisconnected && g_oled) {
        // M7-D2（AE.3）：断线清槽（下一会话握手再 reset；PC 侧断线清空预览位图）。
        g_oled->previewSlot().reset();
    }
#endif
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

// ---- M7-C2：DisplayRouteMode 映射与模式应用（SET_MODE 与 test-only 钩子共用）----
// wire byte → DisplayRouteMode 显式映射统一走 display::fromWireMode/toWireMode
//（M8-A4 唯一转换点；禁止在本文件散落 switch int / static_cast）。

// 模式 → PhysicalScene（M7-C2 定稿映射）：
//   Mirror/PhysicalOnly → Application（OLED 显示 LVGL 应用缩略帧）；
//   Split → Diagnostics；VirtualOnly → Diagnostics（应用帧禁用，诊断页继续）。
// 仅 OLED_ENABLE 时使用（applyDisplayMode 内 PhysicalScene 仅存在于物理 sink 路径）。
#if CONFIG_ESPVIEW_OLED_ENABLE
display::PhysicalScene sceneOf(display::DisplayRouteMode rm) {
    return (rm == display::DisplayRouteMode::kPhysicalOnly ||
            rm == display::DisplayRouteMode::kMirror)
               ? display::PhysicalScene::kApplication
               : display::PhysicalScene::kDiagnostics;
}
#endif

// 应用模式：白名单已由调用方校验（0..3）；Router 缺 physical sink 时
// setMode 返回 kInvalidParam 且不崩溃（C1 语义；OLED=n 时 1/2/3 走此降级）。
void applyDisplayMode(uint8_t mode) {
    const auto rm = display::fromWireMode(mode);
    if (!rm) {
        ESP_LOGW(kTag, "applyDisplayMode invalid mode=%u", static_cast<unsigned>(mode));
        return;
    }
    g_currentMode = *rm;
    if (g_router) {
        const display::DisplayStatus rs = g_router->setMode(*rm);
        ESP_LOGI(kTag, "applyDisplayMode mode=%u router=%s state=%d", static_cast<unsigned>(mode),
                 rs == display::DisplayStatus::kOk ? "ok" : "err",
                 static_cast<int>(g_router->state()));
    } else {
        ESP_LOGI(kTag, "applyDisplayMode mode=%u (router not assembled)", static_cast<unsigned>(mode));
    }
#if CONFIG_ESPVIEW_OLED_ENABLE
    if (g_physicalSink) {
        g_physicalSink->setScene(sceneOf(*rm));
    }
#endif
}

// SET_MODE 请求（ACK_REQ）：白名单 0..3（M7-C2 起 kSplit 接受）；模式经
// DisplayRouter 实际路由（缺 physical sink 时对应模式仍 ACK OK，但仅虚拟侧生效）。
void onSetModeRequest(uint8_t type, const std::vector<uint8_t>& payload, uint16_t ackSeq) {
    if (type != static_cast<uint8_t>(MessageType::kSetMode) || payload.size() != 1) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        return;
    }
    const uint8_t mode = payload[0];
    if (mode > static_cast<uint8_t>(display::DisplayRouteMode::kSplit)) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        ESP_LOGW(kTag, "SET_MODE invalid mode=%u -> ACK ERR", static_cast<unsigned>(mode));
        return;
    }
    applyDisplayMode(mode);
    ESP_LOGI(kTag, "SET_MODE OK mode=%u -> ACK", static_cast<unsigned>(mode));
    g_endpoint.acknowledge(ackSeq, 0, ErrorCode::kNone);
}

// ---- M7-D3：Wi-Fi Provisioning 请求处理（AF.2/AF.3）----
// WIFI_SCAN_REQ：ACK OK = 已接受并排队扫描（结果经 WIFI_SCAN_RESULT 异步到达）；
// ACK ERR = 非法参数（探针场景下老固件同样回 ERR，PC 据此降级，不发真实凭据）。
void onWifiScanReqRequest(uint8_t type, const std::vector<uint8_t>& payload, uint16_t ackSeq) {
    if (type != static_cast<uint8_t>(MessageType::kWifiScanReq) || payload.size() != 2) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        return;
    }
    const uint8_t flags = payload[0];
    const uint8_t maxEntries = payload[1];
    if (flags != 0 || maxEntries > 64) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        return;
    }
    g_wifiProv.requestScan(maxEntries);
    g_endpoint.acknowledge(ackSeq, 0, ErrorCode::kNone);
}

// WIFI_CONFIG（103B，含密码明文）。AF.3：ACK ERR → 不上报成功、不落存储。
// AF.4 安全：payload 中的凭据经 parseWifiConfig 拷贝到 info 后，本函数内所有
// 本地副本（info.password）在处理后立即清零；请求副本由 WifiProvisioning
// 复制到 RAM 并在应用后清零（消息缓冲由 Decoder 覆写，本处不可写）。
void onWifiConfigRequest(uint8_t type, const std::vector<uint8_t>& payload, uint16_t ackSeq) {
    if (type != static_cast<uint8_t>(MessageType::kWifiConfig) ||
        payload.size() < kWifiConfigPayloadSize) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        return;
    }
    WifiConfigInfo info;
    if (!parseWifiConfig(BytesView(payload.data(), payload.size()), info)) {
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        return;
    }
    if ((info.flags & kWifiConfigFlagClear) != 0) {
        // M7-F：CLEAR 分支同样清零本地密码副本（正常报文密码段全零，防御）。
        if (!info.password.empty()) {
            std::memset(info.password.data(), 0, info.password.size());
        }
        g_wifiProv.requestClear();
        g_endpoint.acknowledge(ackSeq, 0, ErrorCode::kNone);
        return;
    }
    // 校验与 makeWifiConfig 同规则（ssid 1..32；password 0 或 8..63；serverIp 非 0；
    // serverPort 1..65535）。
    const bool ok = !info.ssid.empty() && info.ssid.size() <= kWifiSsidMaxBytes &&
                    (info.password.empty() ||
                     (info.password.size() >= 8 && info.password.size() <= 63)) &&
                    info.serverIp != 0 && info.serverPort != 0;
    if (!ok) {
        // 清零后 ACK ERR（不留凭据副本）。
        if (!info.password.empty()) {
            std::memset(info.password.data(), 0, info.password.size());
        }
        g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
        return;
    }
    g_wifiProv.requestConfig(info.ssid.data(), info.ssid.size(), info.password.data(),
                             info.password.size(), info.serverIp, info.serverPort);
    // 安全：立即清零本地密码副本（AF.4）。
    if (!info.password.empty()) {
        std::memset(info.password.data(), 0, info.password.size());
        info.password.clear();
    }
    g_endpoint.acknowledge(ackSeq, 0, ErrorCode::kNone);
}

// ACK_REQ 控制消息按 type 分派（v0.1：SET_MODE / WIFI_SCAN_REQ / WIFI_CONFIG）。
// 未知类型 → 确定性 ACK ERR kInvalidParam（AF.3：PC 探针据此识别不支持）。
void onAckRequestDispatcher(uint8_t type, const std::vector<uint8_t>& payload,
                            uint16_t ackSeq) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::kSetMode:
            onSetModeRequest(type, payload, ackSeq);
            return;
        case MessageType::kWifiScanReq:
            onWifiScanReqRequest(type, payload, ackSeq);
            return;
        case MessageType::kWifiConfig:
            onWifiConfigRequest(type, payload, ackSeq);
            return;
        default:
            g_endpoint.acknowledge(ackSeq, 1, ErrorCode::kInvalidParam);
            return;
    }
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
#if CONFIG_ESPVIEW_TEST_MODE_SWITCH
        // M7-C2 test-only：F11 按下 → 请求运行时循环 DisplayRouteMode 0..3
        // （验收钩子；生产固件 CONFIG_ESPVIEW_TEST_MODE_SWITCH=n 时该键走
        // 正常输入链路）。调试键不进入输入链路。
        if (e.type == espview::input::InputType::kKeyDown && e.keycode == 0x44u) {
            g_modeSwitchPending.store(true);
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

#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
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
#endif  // CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
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

// ---- M7-G：TCP handoff 编排（UART bootstrap → TCP client）----
// DESIGN.md AF/AG 序列：GOT_IP(5) → kTcpConnecting(6)（switchTo 前经 UART
// 上报）→ switchTo(kTcp)（目标来自 provisioning 凭据，adopt 复用 Wi-Fi 驱动）
// → kTcpConnected(7)（TCP 会话 CONNECTED 后经 TCP 上报）；连接超时/失败 →
// 回退 UART bootstrap + kError/kServerUnreachable（凭据保留，向导可重试/
// CLEAR）。全部在会话任务上下文执行（非阻塞；switchTo 的 close-join 最坏
// 有界：Kconfig 上限下约 122s（connect_timeout + reconnect_delay + 余量，
// M8-A3 B M3），仅发生在故障回退路径）。
void abortTcpHandoff();  // 前向声明（beginTcpHandoff 的失败回退路径引用）
void beginTcpHandoff(const WifiProvStatus& st, uint64_t nowMs) {
    if (st.serverIp == 0 || st.serverPort == 0) {
        ESP_LOGW(kTag, "TCP handoff: no server target");
        g_handoffDisabled = true;
        g_wifiProv.setServerUnreachable();
        return;
    }
    // 网络序 uint32（值语义，AF.2 大端写线；首字节 = 地址第一段）→ 点分
    // IPv4。必须按网络序移位取段：小端机器上
    // reinterpret_cast<const uint8_t*>(&st.serverIp) 会得到逆序段
    // （64 01 A8 C0 → "100.1.168.192"），TCP connect 将打到错误地址。
    std::snprintf(g_handoffServerIp, sizeof(g_handoffServerIp), "%u.%u.%u.%u",
                  static_cast<unsigned>((st.serverIp >> 24) & 0xFFu),
                  static_cast<unsigned>((st.serverIp >> 16) & 0xFFu),
                  static_cast<unsigned>((st.serverIp >> 8) & 0xFFu),
                  static_cast<unsigned>(st.serverIp & 0xFFu));
    g_handoffServerPort = st.serverPort;
    g_handoffActive = true;  // 工厂 lambda 在 switchTo 时读取（同一上下文）
    g_handoffStageStartMs = nowMs;
    // 先报 kTcpConnecting（仍走 UART bootstrap），再切换。
    g_wifiProv.setTcpPhase(static_cast<uint8_t>(WifiStatusPhase::kTcpConnecting));
    const bool ok = g_mgr.switchTo(espview::transport::TransportType::kTcp);
    if (!ok) {
        ESP_LOGE(kTag, "TCP handoff: switchTo(kTcp) failed");
        abortTcpHandoff();
        return;
    }
    g_handoff = HandoffPhase::kConnecting;
    ESP_LOGI(kTag, "TCP handoff: switched to TCP (target from provisioning)");
}

void abortTcpHandoff() {
    ESP_LOGW(kTag, "TCP handoff: server unreachable; falling back to UART bootstrap");
    g_handoff = HandoffPhase::kIdle;
    g_handoffActive = false;
    g_handoffDisabled = true;  // 锁存：直到用户重新 CONFIG/CLEAR（相位 < kGotIp）
    // 回退 UART bootstrap（向导/用户仍在 UART 侧）。endpoint 重新握手后
    // WIFI_STATUS kError/kServerUnreachable 经 UART 上行（onSessionState
    // republish + 本分支的保持断言）。
    g_mgr.switchTo(espview::transport::TransportType::kUart);
    g_wifiProv.setServerUnreachable();
}

// 每 200ms 由 sessionLoop 调用；驱动 GOT_IP → switchTo → 相位 6/7 → 回退。
void tickTcpHandoff(uint64_t nowMs) {
    const WifiProvStatus st = g_wifiProv.statusSnapshot();
    const espview::transport::ITransport::State ts =
        static_cast<espview::transport::ITransport::State>(g_transportState.load());
    switch (g_handoff) {
        case HandoffPhase::kIdle: {
            // 新 CONFIG（phase=2..4 < kGotIp）或 CLEAR（kCleared=9）解锁
            // ServerUnreachable 锁存：CLEAR 后相位回到 kCleared，不得继续强推
            // kServerUnreachable（数值上 kCleared=9 > kGotIp=5，需显式判定）。
            if (st.phase < static_cast<uint8_t>(WifiStatusPhase::kGotIp) ||
                st.phase == static_cast<uint8_t>(WifiStatusPhase::kCleared)) {
                g_handoffDisabled = false;
            } else if (g_handoffDisabled &&
                       g_endpoint.state() == SessionState::kConnected &&
                       st.phase != static_cast<uint8_t>(WifiStatusPhase::kError)) {
                // ServerUnreachable 后 Wi-Fi 事件可能覆盖相位：会话内保持
                // kError/kServerUnreachable 可见（直到用户重新 CONFIG/CLEAR）。
                g_wifiProv.setServerUnreachable();
            }
            // 触发：GOT_IP + 当前不在 TCP + 未被锁存（provisioning 驱动）。
            if (st.phase == static_cast<uint8_t>(WifiStatusPhase::kGotIp) &&
                g_mgr.current() != espview::transport::TransportType::kTcp &&
                !g_handoffDisabled) {
                beginTcpHandoff(st, nowMs);
            }
            break;
        }
        case HandoffPhase::kConnecting: {
            if (ts == espview::transport::ITransport::State::kConnected) {
                // TCP 链路已建立；等 endpoint 会话 CONNECTED（PC HELLO）后才
                // 上报 kTcpConnected（sendWifiStatus 经 tryTransmit CONNECTED 门控）。
                if (g_endpoint.state() == SessionState::kConnected) {
                    g_wifiProv.setTcpPhase(static_cast<uint8_t>(WifiStatusPhase::kTcpConnected));
                    g_handoff = HandoffPhase::kConnected;
                    g_handoffStageStartMs = nowMs;
                    ESP_LOGI(kTag, "TCP handoff: session CONNECTED");
                } else if (nowMs - g_handoffStageStartMs >= kHandoffConnectTimeoutMs) {
                    abortTcpHandoff();
                }
            } else if (nowMs - g_handoffStageStartMs >= kHandoffConnectTimeoutMs) {
                abortTcpHandoff();  // 首次连接窗口超时 → ServerUnreachable
            }
            break;
        }
        case HandoffPhase::kConnected: {
            // 仅当 provisioning 相位仍在 TCP 窗口（5/6/7）时本编排才覆写相位；
            // Wi-Fi 侧错误（kError）不被 TCP 观察态覆盖，向导保持可见。
            const uint8_t p = st.phase;
            const bool inTcpWindow =
                p == static_cast<uint8_t>(WifiStatusPhase::kGotIp) ||
                p == static_cast<uint8_t>(WifiStatusPhase::kTcpConnecting) ||
                p == static_cast<uint8_t>(WifiStatusPhase::kTcpConnected);
            if (ts == espview::transport::ITransport::State::kConnected) {
                g_handoffStageStartMs = nowMs;  // 保持已连
                if (inTcpWindow && g_endpoint.state() == SessionState::kConnected &&
                    st.phase != static_cast<uint8_t>(WifiStatusPhase::kTcpConnected)) {
                    g_wifiProv.setTcpPhase(static_cast<uint8_t>(WifiStatusPhase::kTcpConnected));
                }
            } else if (ts == espview::transport::ITransport::State::kConnecting) {
                // 掉线重连中：相位回 kTcpConnecting（观察态；AG.2 不单独推进）。
                if (inTcpWindow) {
                    g_wifiProv.setTcpPhase(static_cast<uint8_t>(WifiStatusPhase::kTcpConnecting));
                }
            } else {
                // kDisconnected/kError：同样进入重连窗口。
                if (inTcpWindow) {
                    g_wifiProv.setTcpPhase(static_cast<uint8_t>(WifiStatusPhase::kTcpConnecting));
                }
            }
            if (nowMs - g_handoffStageStartMs >= kHandoffDropTimeoutMs) {
                abortTcpHandoff();  // 已连后掉线重连超时 → 回退 UART
            }
            break;
        }
    }
}

// ---- 会话驱动：每 200ms tick（心跳 2s / 对端超时 5s / ACK 重试 500ms）----
// 注意：本任务绝不做阻塞式发送。流式大帧（153608B FRAME_RECT）会持有
// sendMutex_ 十余秒，任何阻塞式 sendMessage 都会让 tick() 饿死（心跳停摆、
// 对端超时无法发现 → M1-3B reconnect 回归问题）。统计上报已移到独立任务。
// M6-D test-only：F12 调试钩子执行体（sessionLoop 上下文；§二十/§二十一）。
// switchTo 内部按 Disconnected → Connected 顺序重放状态回调：上层完成会话重置
//（decoder/frame/ACK/seq/InputManager 清零）+ HELLO + FULL resync（§五/§六）。
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
void debugTransportSwitch() {
    // M7-G：handoff 活动期间禁止 F12 手动切换（避免与 provisioning 编排冲突）。
    if (g_handoffActive) {
        ESP_LOGW(kTag, "debug transport switch ignored (TCP handoff active)");
        return;
    }
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

#if CONFIG_ESPVIEW_TEST_MODE_SWITCH
// M7-C2 test-only：F11 执行体（sessionLoop 上下文）：循环 DisplayRouteMode
// 0..3（0→1→2→3→0），与 SET_MODE 同一 applyDisplayMode 路径（白名单 + Router
// setMode + PhysicalScene 映射）。状态经 ERROR 文本通道上报（非 wire 格式）。
void debugModeSwitch() {
    const uint8_t next =
        static_cast<uint8_t>((display::toWireMode(g_currentMode) + 1u) % 4u);
    applyDisplayMode(next);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "mod sw=%u st=%d scene=%d",
                  static_cast<unsigned>(next),
                  g_router ? static_cast<int>(g_router->state()) : -1,
#if CONFIG_ESPVIEW_OLED_ENABLE
                  g_physicalSink ? static_cast<int>(g_physicalSink->scene()) : -1
#else
                  -1
#endif
    );
    const auto m = makeError(ErrorCode::kNone, buf);
    if (m.has_value()) {
        g_endpoint.sendMessage(*m);
    }
}
#endif  // CONFIG_ESPVIEW_TEST_MODE_SWITCH

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
#if CONFIG_ESPVIEW_TEST_MODE_SWITCH
        if (g_modeSwitchPending.exchange(false)) {
            debugModeSwitch();
        }
#endif
        g_endpoint.tick();
        // M7-D3：Wi-Fi Provisioning 相位机（命令/扫描结果/状态去重；非阻塞回调）。
        g_wifiProv.tick(monotonicMs());
        // M7-G：TCP handoff 编排（GOT_IP → switchTo → 相位 6/7 → 超时回退）。
        tickTcpHandoff(monotonicMs());
#if CONFIG_ESPVIEW_OLED_ENABLE
        // M7-D2（AE.3）：2Hz 节拍发送 Physical Preview（内部 tryTransmit，非阻塞）。
        if (g_oled && g_endpoint.state() == SessionState::kConnected) {
            static uint64_t lastPreviewMs = 0;
            const uint64_t now = monotonicMs();
            if (lastPreviewMs == 0 || now - lastPreviewMs >= kPreviewIntervalMs) {
                lastPreviewMs = now;
                sendPhysicalPreviewMessage();
            }
        }
#endif
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
                // M7-B：改用 diagSnapshot 值语义快照（锁内拷贝）：原 transport()
                // 裸指针在锁外解引用，switchTo 并发时存在 use-after-free。
                const auto diag = g_mgr.diagSnapshot();
                const uint32_t swC = g_mgr.switchCount() < 9999u ? g_mgr.switchCount() : 9999u;
                const uint32_t rcC = diag.reconnectCount < 9999u
                                          ? static_cast<uint32_t>(diag.reconnectCount)
                                          : 9999u;
                const uint32_t txC = diag.txBytes < 9999u
                                          ? static_cast<uint32_t>(diag.txBytes)
                                          : 9999u;
                const uint32_t rxC = diag.rxBytes < 9999u
                                          ? static_cast<uint32_t>(diag.rxBytes)
                                          : 9999u;
                std::snprintf(trxBuf, sizeof(trxBuf),
                              "trx tr=%d st=%u sw=%u rc=%u tx=%u rx=%u rssi=%d ch=%u",
                              diag.type == espview::transport::TransportType::kTcp ? 1 : 0,
                              static_cast<unsigned>(g_transportState.load()),
                              static_cast<unsigned>(swC),
                              static_cast<unsigned>(rcC),
                              static_cast<unsigned>(txC),
                              static_cast<unsigned>(rxC),
                              static_cast<int>(diag.rssi),
                              static_cast<unsigned>(diag.channel));
                const auto mTrx = makeError(ErrorCode::kNone, trxBuf);
                if (mTrx.has_value()) {
                    g_endpoint.sendMessage(*mTrx);
                }
            }
            // M7-B：内存诊断行（ERROR 文本通道，非 wire 格式）：
            //   mem h=<freeHeap> lg=<largestBlock> mn=<minFreeHeap>
            // 计数 clamp 到 8 位（heap ≤ 99,999,999B）；行 ≤64B（makeError 限制）。
            {
                char memBuf[96];
                const uint64_t freeH = static_cast<uint64_t>(esp_get_free_heap_size());
                const uint64_t minH = static_cast<uint64_t>(esp_get_minimum_free_heap_size());
                const uint64_t lgH = static_cast<uint64_t>(
                    heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
                const auto clamp8 = [](uint64_t v) { return v < 100000000u ? v : 99999999u; };
                std::snprintf(memBuf, sizeof(memBuf),
                              "mem h=%llu lg=%llu mn=%llu",
                              static_cast<unsigned long long>(clamp8(freeH)),
                              static_cast<unsigned long long>(clamp8(lgH)),
                              static_cast<unsigned long long>(clamp8(minH)));
                const auto mMem = makeError(ErrorCode::kNone, memBuf);
                if (mMem.has_value()) {
                    g_endpoint.sendMessage(*mMem);
                }
            }
#include "esp_heap_caps.h"  // heap_caps_get_largest_free_block / MALLOC_CAP_DEFAULT
#include "esp_system.h"     // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#if CONFIG_ESPVIEW_OLED_ENABLE
            //   oled a=<addr> c=<ctrl> err=<errCount> ok=<0|1>
            // 计数 clamp 到 5 位（≤99999）；行 ≤64B（makeError 限制）。
            if (g_oled) {
                const espview::oled::OledStatus os = g_oled->status();
                char oledBuf[96];
                const uint64_t oledErr = os.errorCount < 99999u
                                             ? os.errorCount
                                             : 99999u;
                std::snprintf(oledBuf, sizeof(oledBuf),
                              "oled a=0x%02X c=%s err=%llu ok=%d",
                              static_cast<unsigned>(os.address),
                              espview::oled::controllerName(os.controller),
                              static_cast<unsigned long long>(oledErr),
                              os.ok ? 1 : 0);
                const auto mOled = makeError(ErrorCode::kNone, oledBuf);
                if (mOled.has_value()) {
                    g_endpoint.sendMessage(*mOled);
                }
            }
#endif
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
// （Transport 语义不变：kBackpressure = would-block；重试是应用层策略）。
// 会话 DISCONNECTED（对端断线/超时）时立即放弃，不向虚空继续发送。
// M8-A3：proto::SendStatus 与 transport::SendStatus 已是同一类型（using 引用），
// 不再需要显式映射（原 mapTransportSend 已删除）。
SendStatus transportSink(const uint8_t* data, size_t len) {
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
    g_sinkEntryMs.store(monotonicMs());
    g_sinkActive.store(true);
#endif
    const SendStatus r = g_sink.send(data, len);
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
    g_sinkActive.store(false);
    g_sinkExitMs.store(monotonicMs());
#endif
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
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
    g_sinkEntryMs.store(monotonicMs());
    g_sinkActive.store(true);
#endif
    const SendStatus r = g_sink.trySend(data, len);
#if CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH
    g_sinkActive.store(false);
    g_sinkExitMs.store(monotonicMs());
#endif
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
    c.mode_mask = 0b1111;  // M7-C2：WINDOW | DEVICE | MIRROR | SPLIT（SET_MODE 白名单 0..3）
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
    c.onAckRequest = onAckRequestDispatcher;  // M7-D3：SET_MODE / WIFI_SCAN_REQ / WIFI_CONFIG
    c.onAck = onAck;
    c.onError = onError;
    c.onOtherMessage = onOtherMessage;  // M3：INPUT_* 输入通道
    return c;
}();

ProtocolEndpoint g_endpoint(kEndpointCfg, transportSink, trySink, kEndpointCallbacks, monotonicMs);

// ---- M7-D3：Wi-Fi Provisioning（UART bootstrap）----
// 状态/扫描结果回调在会话任务 tick 上下文执行（非阻塞 tryTransmit 发送：
// sendWifiStatus/sendWifiScanResult 内部背压整帧丢弃，绝不阻塞 sessionLoop）。
WifiProvisioning g_wifiProv(WifiProvisioning::Callbacks{
    [](const WifiProvStatus& st) {
        WifiStatusInfo info;
        info.phase = st.phase;
        info.errorCode = st.errorCode;
        info.flags = st.flags;
        info.rssi = st.rssi;
        info.channel = st.channel;
        info.ip = st.ip;
        info.serverIp = st.serverIp;
        info.serverPort = st.serverPort;
        info.ssidLen = st.ssidLen;
        if (st.ssidLen > 0) {
            info.ssid.assign(st.ssid, st.ssid + st.ssidLen);
        }
        g_endpoint.sendWifiStatus(info);
    },
    [](uint8_t scanSeq, bool truncated, uint16_t total,
       const WifiProvScanRecord* records, size_t count) {
        WifiScanResultInfo result;
        result.scanSeq = scanSeq;
        result.total = total;
        if (truncated) {
            result.flags |= kWifiScanResultFlagTruncated;
        }
        result.records.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            WifiScanRecordInfo rec;
            const size_t n = std::strlen(records[i].ssid);
            rec.ssid.assign(records[i].ssid, records[i].ssid + n);
            std::memcpy(rec.bssid.data(), records[i].bssid, 6);
            rec.rssi = records[i].rssi;
            rec.channel = records[i].channel;
            rec.authmode = records[i].authmode;
            result.records.push_back(std::move(rec));
        }
        result.count = static_cast<uint8_t>(result.records.size());
        g_endpoint.sendWifiScanResult(result);
    },
#if CONFIG_ESPVIEW_OLED_ENABLE
    // M7-E：扫描期间 OLED 暂停/恢复挂钩（事务终态路径保证配对）。
    // OLED 未启动（g_oled 空）时直接成功 no-op；CONFIG_ESPVIEW_SCAN_SUSPEND_OLED=n
    // 时由 WifiProvisioning 内部忽略本挂钩（等同 M7 前行为，供 A/B 对比）。
    []() -> bool {
        if (!g_oled) {
            return true;
        }
        return g_oled->pauseForWifiScan();
    },
    []() {
        if (g_oled) {
            g_oled->resumeAfterWifiScan();
        }
    },
#else
    // OLED 禁用（CONFIG_ESPVIEW_OLED_ENABLE=n）：空回调 —— WifiProvisioning 侧
    // 对空回调按"直接成功/no-op"处理，等同 M7 前行为（供 A/B 对比）。
    {},
    {},
#endif
});

#include "esp_heap_caps.h"  // heap_caps_get_largest_free_block / MALLOC_CAP_DEFAULT
#include "esp_system.h"     // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#if CONFIG_ESPVIEW_OLED_ENABLE
// M7-A：独立 OLED 状态显示（组件协议无关；任务优先级 2 < stats=3/session=5）。
std::unique_ptr<espview::oled::OledDisplay> g_oled;
// M7-C2：物理显示 sink（IDisplaySink；app_main 组装：init + attachPhysical）。
std::shared_ptr<espview::oled::PhysicalDisplaySink> g_physicalSink;
#endif
// M7-C2：DisplayRouter 全局（app_main 组装：lvgl_port attach virtual、本文件
// attach physical + setMode；SET_MODE 请求与 test-only 钩子共用）。
std::shared_ptr<display::DisplayRouter> g_router;

#if CONFIG_ESPVIEW_APP_LVGL
// M5-A LVGL Port：display driver + flush_cb + TX 任务 + 统计。
std::unique_ptr<espview::LvglPort> g_lvgl;
#else
// M1-3B TestPattern：CONNECTED 后自动发送确定性帧序列（test-only）。
std::unique_ptr<TestPattern> g_testPattern;
#endif

}  // namespace

extern "C" void app_main() {
    // M7-F：boot 摘要（reset reason / CPU 频率 / flash 大小 / heap / uptime）。
    // v6.0.2 API 可用性：esp_get_flash_size() 已移除 → g_rom_flashchip.chip_size；
    // esp_clk_cpu_freq() 已移入 esp_private → esp_rom_get_cpu_ticks_per_us()（等价 MHz）。
    const esp_reset_reason_t bootReason = esp_reset_reason();
    ESP_LOGI(kTag,
             "BOOT: reset_reason=%d(%s) cpu_mhz=%u flash_kb=%u uptime_ms=%llu free_heap=%u",
             static_cast<int>(bootReason), resetReasonName(bootReason),
             static_cast<unsigned>(esp_rom_get_cpu_ticks_per_us()),
             static_cast<unsigned>(g_rom_flashchip.chip_size / 1024u),
             static_cast<unsigned long long>(esp_timer_get_time() / 1000),
             static_cast<unsigned>(esp_get_free_heap_size()));

#if CONFIG_ESPVIEW_TRANSPORT_TCP
    ESP_LOGI(kTag, "ESPView: initial transport=TCP server=%s:%d (runtime switch via TransportManager)",
             CONFIG_ESPVIEW_TCP_SERVER_IP, CONFIG_ESPVIEW_TCP_SERVER_PORT);
#else
    ESP_LOGI(kTag, "ESPView: initial transport=UART port=%d baud=%d (runtime switch via TransportManager)",
             CONFIG_ESPVIEW_UART_PORT, CONFIG_ESPVIEW_UART_BAUD);
#endif

#if CONFIG_ESPVIEW_APP_LVGL
    // M7-C2：DisplayRouter 组装（lvgl_port 构造时 attach VirtualSink；本文件随后
    // attach PhysicalDisplaySink + setMode；SET_MODE 白名单 0..3 映射到 Router）。
    g_router = std::make_shared<display::DisplayRouter>();
    // M5-A：LVGL 应用（真实 UI → dirty rect → DisplayRouter → 虚拟/物理 sink）。
    // 发送回调同时持有普通 Message 与 Streaming 两条路径（与 TestPattern 同构）。
    g_lvgl = std::make_unique<espview::LvglPort>(
        [](const Message& msg) { return g_endpoint.sendMessage(msg); },
        [](const MessageHeader& header, IMessagePayloadSource& source) {
            return g_endpoint.sendMessageStreaming(header, source);
        },
        g_router);
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
    logBootStage("transport_open_before");

    // 打开初始 Transport（编译期选择；运行时可用 g_mgr.switchTo() 切换）。
    if (!g_mgr.open()) {
        ESP_LOGE(kTag, "transport open failed: %s", g_mgr.lastError());
        return;
    }
    ESP_LOGI(kTag, "transport open OK: type=%d mtu=%zu",
             static_cast<int>(g_mgr.current()),
             g_mgr.capabilities().mtu);  // M8-A3：mtu 经 capabilities() 读取（原 transport()->mtu() 已删除）
    logBootStage("transport_open_after");
#if CONFIG_ESPVIEW_APP_LVGL
    updateFlushWaitFromTransport();
#endif

#include "esp_heap_caps.h"  // heap_caps_get_largest_free_block / MALLOC_CAP_DEFAULT
#include "esp_system.h"     // esp_get_free_heap_size / esp_get_minimum_free_heap_size
#if CONFIG_ESPVIEW_OLED_ENABLE
    // M7-A：独立 OLED 状态显示（低优先级任务；OLED 组件协议无关，状态由
    // oledStatusSnapshot 注入；绝不触碰 protocol sendMutex / Transport）。
    {
        espview::oled::OledConfig oledCfg;
        oledCfg.sdaGpio = CONFIG_ESPVIEW_OLED_SDA_GPIO;
        oledCfg.sclGpio = CONFIG_ESPVIEW_OLED_SCL_GPIO;
        oledCfg.clkHz = CONFIG_ESPVIEW_OLED_I2C_CLK_HZ;
#if CONFIG_ESPVIEW_OLED_ADDR_AUTO
        oledCfg.addrAuto = true;
#else
        oledCfg.addrAuto = false;
        oledCfg.address = static_cast<uint8_t>(CONFIG_ESPVIEW_OLED_ADDR);
#endif
        oledCfg.refreshMs = CONFIG_ESPVIEW_OLED_REFRESH_MS;
        oledCfg.taskStack = CONFIG_ESPVIEW_OLED_TASK_STACK;
        oledCfg.taskPriority = CONFIG_ESPVIEW_OLED_TASK_PRIORITY;
        oledCfg.i2cTimeoutMs = CONFIG_ESPVIEW_OLED_I2C_TIMEOUT_MS;
        oledCfg.maxReinit = CONFIG_ESPVIEW_OLED_MAX_REINIT;
#if CONFIG_ESPVIEW_OLED_CONTROLLER_SSD1306
        oledCfg.controller = espview::oled::ControllerType::kSsd1306;
#elif CONFIG_ESPVIEW_OLED_CONTROLLER_SH1106
        oledCfg.controller = espview::oled::ControllerType::kSh1106;
#endif
        g_oled = std::make_unique<espview::oled::OledDisplay>(oledCfg,
                                                              oledStatusSnapshot);
        if (g_oled->start()) {
            ESP_LOGI(kTag, "OLED display started");
            logBootStage("oled_start_ok");
        } else {
            ESP_LOGE(kTag, "OLED display start failed");
            logBootStage("oled_start_fail");
            g_oled.reset();
        }
        // M7-C2：PhysicalDisplaySink 组装（init 校验生产者能力并落定自身 128x64
        // mono；attach 后由 Router 路由；OLED start 失败时 sink 仍 attach，
        // isAvailable()==false → Router 收敛 kDegraded，Virtual 侧不受影响）。
        if (g_oled) {
            // 非拥有引用：g_physicalSink 与 g_oled 同为全局，销毁序 sink 先于 display。
            g_physicalSink = std::make_shared<espview::oled::PhysicalDisplaySink>(g_oled.get());
            display::DisplayCapabilities prodCaps;
            prodCaps.width = 320;    // LVGL/TestPattern 源帧分辨率（LVGL 320x240）
            prodCaps.height = 240;
            prodCaps.format = proto::PixelFormat::kRgb565;
            prodCaps.color = 16;
            prodCaps.mono = false;
            prodCaps.canReadback = true;
            prodCaps.sinkKind = display::DisplaySinkKind::kVirtual;  // 生产者侧能力描述
            if (g_physicalSink->init(prodCaps) == display::DisplayStatus::kOk && g_router) {
                g_router->attachPhysical(g_physicalSink);
                ESP_LOGI(kTag, "PhysicalDisplaySink attached (src %ux%u -> 128x64 mono)",
                         static_cast<unsigned>(prodCaps.width),
                         static_cast<unsigned>(prodCaps.height));
            } else {
                ESP_LOGE(kTag, "PhysicalDisplaySink init failed; physical routing disabled");
                g_physicalSink.reset();
            }
        }
    }
#endif

    // M7-C2：Router 钩子 —— 模式切换成功后请求下一帧 FULL resync（LVGL 全屏置脏，
    // 所有启用 sink 收新帧；stale-clear 由 RemoteDisplay 自身 FULL 语义覆盖，本层
    // 不额外清屏）。钩子内不得调用 Router 锁方法（display_router.h 约定）。
    if (g_router) {
        g_router->setFullResyncCallback([] {
#if CONFIG_ESPVIEW_APP_LVGL
            if (g_lvgl) {
                g_lvgl->requestFullInvalidate();
            }
#endif
        });
        // 初始模式（CONFIG_ESPVIEW_DEFAULT_MODE：0=VirtualOnly 1=PhysicalOnly
        // 2=Mirror 3=Split；硬件验证 Physical/Mirror 时改此值；与 SET_MODE 同路径）。
        uint8_t initialMode = static_cast<uint8_t>(CONFIG_ESPVIEW_DEFAULT_MODE);
        if (initialMode > static_cast<uint8_t>(display::DisplayRouteMode::kSplit)) {
            initialMode = static_cast<uint8_t>(display::DisplayRouteMode::kVirtualOnly);
        }
        applyDisplayMode(initialMode);
    }

    xTaskCreate(sessionLoop, "espview_sess", 4096, nullptr, 5, nullptr);
    xTaskCreate(statsLoop, "espview_stat", 4096, nullptr, 3, nullptr);
    logBootStage("session_stats_tasks_created");

    // 主循环：周期打印会话统计（console 禁用时无输出，仅保留结构）。
    uint32_t loop = 0;
    while (true) {
        if (loop == 0) {
            logBootStage("main_loop_first_round");
        }
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
