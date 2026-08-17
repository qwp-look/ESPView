// ESPView M8-A5 — Wi-Fi 有界重连策略（shared/wifi，纯 C++17，零平台依赖）。
//
// 规范来源：M8-A5 任务书 §十三/§十四 + WifiSta 审计（WIFI-01/02/07）。
// 目标：
//   - 重连收敛为单一 owner（link task）；事件 handler 不得直接 esp_wifi_connect；
//   - 指数退避且有上限（transient/terminal 两套调度）；
//   - restart loop 可停止：stop() 后 mustStop()==true，不再安排下一次 retry；
//   - host 单元可测（ESP32 侧在 tcp_transport linkLoop 中使用）。
// 纯实现层策略：不接触 wire、不调用 ESP-IDF API。

#pragma once

#include <cstdint>

namespace espview {
namespace wifi {

// 失败分类（与 wifi_provisioning 的 reason 分类对齐，见 M8-A5 审计 WIFI-01）。
enum class WifiFailureKind : uint8_t {
    kTransient = 0,  // AP 临时不可达 / 偶发断开：快速退避重试
    kTerminal = 1,   // 认证失败 / 握手超时等终态：慢退避重试（等配置/AP 恢复）
};

// 有界指数退避策略（线程不要求；由单一管理任务调用）。
class ReconnectPolicy {
public:
    // transient：100ms → 200 → 400 → 800 → 1600 → 3200 → cap 5000ms
    static constexpr uint32_t kTransientBaseMs = 100;
    static constexpr uint32_t kTransientMaxMs = 5000;
    // terminal：1s → 2s → 4s → 8s → 16s → cap 30s（慢循环，避免风暴）
    static constexpr uint32_t kTerminalBaseMs = 1000;
    static constexpr uint32_t kTerminalMaxMs = 30000;

    // 记录一次失败并推进退避。kind 决定下次延迟调度。
    void onAttempt(WifiFailureKind kind);
    // 连接成功：重置尝试计数与退避起点。
    void onSuccess();
    // 停止重试（stop/deinit）：此后 mustStop() 恒 true，调用方不得再安排 retry。
    void stop();
    // stop() 后 true；未 stop 恒 false。
    bool mustStop() const { return stopped_; }

    // 下一次重试前应等待的毫秒数（onAttempt 之后读取）。
    uint32_t nextDelayMs() const { return nextDelayMs_; }
    uint32_t attempts() const { return attempts_; }
    WifiFailureKind lastKind() const { return lastKind_; }

private:
    WifiFailureKind lastKind_ = WifiFailureKind::kTransient;
    uint32_t attempts_ = 0;
    uint32_t nextDelayMs_ = kTransientBaseMs;
    bool stopped_ = false;
};

}  // namespace wifi
}  // namespace espview