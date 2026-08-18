// ESPView M7-C3 — DisplayUiState：Display Mode UI 纯模型（纯 C++17，零 Qt / 零平台依赖）。
//
// 规范来源：M7-C 任务书 §二十二（1-10）+ docs/DESIGN.md AA 节（四种模式语义 /
// 降级 / FULL resync）。本文件只做 UI 状态机，不接触协议 wire、不持有传输。
//
// 职责：
//   - 持有 UI 可展示/可编辑的显示模式状态（选择 / 已应用 / 路由状态 /
//     capability 门控 / 会话 / 切换 / FULL resync / Apply 可用性 / 错误）；
//   - 提供确定性转移方法（applyRequested / onSwitchStart / onAck /
//     onConnected / onDisconnected / onFullCommit / onPhysicalAvailable /
//     onPhysicalDegraded），供 GUI 与主控接线调用；
//   - 与 shared/display/display_router.h 的 DisplayRouteMode / RouterState
//     语义对齐（只读 include，见下）。
//
// 冻结语义（M7-C1/C2，禁止推翻）：
//   - DisplayRouteMode：0=VirtualOnly / 1=PhysicalOnly / 2=Mirror / 3=Split；
//   - wire SET_MODE payload 仍为 [0] mode（1 byte），发送经
//     proto::makeSetMode(DisplayMode)，本模型不生成 wire。
//
// 状态转移规则（任务书 §二十二）：
//   - 断开时允许改变选择；此时 Apply 进入 "Waiting for connection" 标记，
//     不进入切换、不改 appliedMode、不假装成功；
//   - Apply 防重复：switchingInProgress=true 期间 applyEnabled=false，
//     applyRequested() 直接拒绝；
//   - ACK ok → appliedMode=本次实际发送的模式 + fullResyncPending=true
//     （设备已应用，PC 侧等新 FULL resync 帧）；
//   - ACK fail → 回退选择到 appliedMode，保留错误文本，恢复 Apply；
//   - 新会话（onConnected）→ fullResyncPending=true；FULL 帧提交
//     （onFullCommit）→ 清 pending，状态收敛到 kConnected / kDegraded；
//   - capability 门控：physicalAvailable=false → PhysicalOnly/Mirror/Split
//     不可选（setSelectedMode 拒绝 + 显示 Unavailable），选择回退 VirtualOnly。

#pragma once

#include <cstdint>
#include <string>

#include "display_router.h"  // DisplayRouteMode / RouterState（只读头，纯 C++17）

namespace espview {
namespace display {

// UI 层路由状态视图：前 4 个值与 DisplayRouter::RouterState 数值一致
// （kIdle=0 / kSwitching=1 / kConnected=2 / kDegraded=3），追加
// kUnavailable=4（capability 缺失或会话断开时的 UI 展示状态）。
enum class UiRouterState : uint8_t {
    kIdle = 0,
    kSwitching = 1,
    kConnected = 2,
    kDegraded = 3,
    kUnavailable = 4,
};

// RouterState → UiRouterState 1:1 映射（前 4 值相等，见上）。
inline UiRouterState toUiRouterState(RouterState s) {
    return static_cast<UiRouterState>(static_cast<uint8_t>(s));
}

// 该模式是否需要物理 sink（PhysicalOnly / Mirror / Split）。
inline bool modeRequiresPhysical(DisplayRouteMode m) {
    return m == DisplayRouteMode::kPhysicalOnly || m == DisplayRouteMode::kMirror ||
           m == DisplayRouteMode::kSplit;
}

// M8-B（B3）：该模式是否需要 FULL resync 帧收敛。PhysicalOnly 下 virtual sink
// 已禁用（ESP32 不再发 Application 帧），收敛不依赖 FULL（capability 门控由
// onPhysicalAvailable 保证）；其余模式在 ACK 后等待新 FULL 帧。
inline bool modeExpectsFullResync(DisplayRouteMode m) {
    return m != DisplayRouteMode::kPhysicalOnly;
}
// M7-C3 — Display Mode UI 状态模型（GUI 线程独占；纯值对象 + 转移方法）。
class DisplayUiState {
public:
    DisplayUiState() { refreshActive(); }

    // ---- 状态字段（public：GUI 只读展示 / 测试断言；转移请走方法）----
    DisplayRouteMode selectedMode = DisplayRouteMode::kVirtualOnly;  // 用户选择（0..3）
    DisplayRouteMode appliedMode = DisplayRouteMode::kVirtualOnly;   // 设备已确认的模式
    UiRouterState routerState = UiRouterState::kIdle;                // UI 视图状态
    bool physicalAvailable = false;   // capability 门控（canPhysical）
    bool virtualActive = true;        // 按 selectedMode 派生的激活标志
    bool physicalActive = false;      // 按 selectedMode 派生的激活标志
    bool sessionConnected = false;    // 会话（HELLO 握手）是否已建立
    bool switchingInProgress = false; // SET_MODE 已发、ACK 未回
    bool fullResyncPending = false;   // 重连/切换后等待新 FULL 帧
    bool applyEnabled = true;         // switching 期间 false
    bool waitingForConnection = false;// 断开时点了 Apply → "Waiting for connection"
    bool pendingInterruptedApply = false;// 在飞 Apply 被断线打断 → 重连后补发（P1-1）
    std::string lastError;            // 最近一次错误（空 = 无错误）

    // ---- M8-B（B2）：分辨率三态（reported / applied / rendered）----
    // reported：对端 HELLO/CAPABILITIES 报告的源分辨率（尚未提交帧）；
    // rendered：最近一次 FULL 帧提交的实际分辨率（画面真实状态）；
    // resolutionChangedPending：已有 rendered 基准且 reported 变化、尚无新
    //   FULL commit（"收到 metadata ≠ 画面已更新"，UI 据此显示 waiting FULL）。
    uint16_t reportedWidth = 0;
    uint16_t reportedHeight = 0;
    uint8_t reportedPixelFormat = 0;
    uint16_t renderedWidth = 0;
    uint16_t renderedHeight = 0;
    uint8_t renderedPixelFormat = 0;
    bool resolutionChangedPending = false;
    // ---- 转移方法 ----
    // 选择模式（0..3；capability 不足时拒绝并保留 lastError）。断开/切换中均可改。
    bool setSelectedMode(DisplayRouteMode mode);
    // Apply：校验（applyEnabled / 会话 / capability）后进入切换。
    // 返回 true = 调用方应发送 wire SET_MODE；false = 未发送
    // （断开 → waitingForConnection；切换中/能力不足 → lastError）。
    bool applyRequested();
    // wire 发送已开始（幂等；外部接线在真实 dispatch 后可调用）。
    void onSwitchStart();
    // SET_MODE ACK 结果（ok=true 设备已应用 → appliedMode + FULL resync pending）。
    void onAck(bool ok);
    // 会话建立（HELLO done）→ 需要 FULL resync。
    void onConnected();
    // 会话断开 → kUnavailable；Apply 允许点击（进入 waiting）。
    void onDisconnected();
    // FULL 帧提交 → 重同步完成，状态收敛（kConnected / kDegraded）。
    void onFullCommit();
    // capability 门控变化：false → 物理相关模式不可选/Unavailable。
    void onPhysicalAvailable(bool available);
    // 运行时物理 sink 不可用（OLED 未 kReady 等）→ 相关模式 kDegraded。
    void onPhysicalDegraded(bool degraded);
    void clearError() { lastError.clear(); }

    // ---- M8-B（B2/B3）：分辨率与 FULL 超时 ----
    // 对端报告分辨率（HELLO/CAPABILITIES/stats 层；校验 1..4096）。
    // reported 变化且已有 rendered 基准 → resolutionChangedPending=true。
    void onResolutionReported(uint16_t w, uint16_t h, uint8_t fmt);
    // FULL 帧提交的实际分辨率（rendered 更新 + 清 pending）。
    void onFrameResolution(uint16_t w, uint16_t h, uint8_t fmt);
    // FULL 等待超时（ACK 已 ok、模式已应用但新 FULL 未到达）：状态明确、
    // 恢复 Apply（可重试），不改变 appliedMode（设备确实已应用）、不清空
    // capability / 会话。仅 fullResyncPending 时生效（幂等）。
    void onFullTimeout();
private:
    // 按 appliedMode + 会话 + capability + 降级收敛路由状态（不含切换）。
    UiRouterState convergedState() const;
    void refreshActive();  // 按 selectedMode 派生 virtualActive / physicalActive

    bool physicalDegraded_ = false;
    DisplayRouteMode pendingApplyMode_ = DisplayRouteMode::kVirtualOnly;  // 本次实际发送的模式
};

}  // namespace display
}  // namespace espview
