// ESPView M7-E — ScanTransaction（Wi-Fi 扫描期间 OLED/I2C 暂停/恢复事务状态机）。
//
// 规范来源：_m7e_contract.md「ScanTransaction（Agent K 实现，Agent C 消费）」。
// 纯 C++17，零平台依赖（PC host 测试与 ESP32 侧共用同一份源码；ESP32 侧由
// esp32/components/espview 复用）。
//
// 设计要点：
//   - 驱动（Agent C 的 wifi_provisioning）按固定顺序调用：
//       begin() -> onScanStarted(true) -> onScanDone(ok)
//     失败/异常可改走 onScanStarted(false) / onTimeout() / onDisconnect()。
//   - 核心不变量：一旦 suspendDisplay() 成功，任何终态路径（成功/失败/超时/断线）
//     恰好调用一次 resumeDisplay()；begin() 幂等（活动相位重复调用为 no-op）；
//     onDisconnect() 后停在 kDisconnected，不自动回 kIdle，需外部显式重新 begin()。
//   - tick(nowMs, timeoutMs) 为防御性看门狗：扫描窗口（kDisplaySuspended /
//     kScanning / kCollecting）内首次 tick 锚定窗口起点，之后按 nowMs 单调差值判定
//     超时；驱动应周期性 tick（如每 100ms）。timeoutMs == 0 表示不启用超时。
//   - kScanning 为契约保留相位：当前 API 的扫描窗口落在 kDisplaySuspended（显示挂起
//     即扫描进行中）；kScanning/kCollecting 仍被超时/断线/恢复逻辑同等对待（防御性，
//     便于驱动后续细分阶段）。
//   - 回调未注入：suspendDisplay 为空视同暂停失败（begin 进入 kError，绝不无保护扫描）；
//     resumeDisplay 为空则恢复为 no-op。

#pragma once

#include <cstdint>
#include <functional>

namespace espview {
namespace wifi {

// 扫描事务相位（数值仅作调试/日志，无协议 wire 语义）。
enum class ScanPhase : int {
    kIdle = 0,             // 空闲；可 begin()
    kPreparing = 1,        // 已请求扫描，正在暂停显示（begin 已同步调用 suspendDisplay）
    kDisplaySuspended = 2, // 显示已挂起，扫描进行中（扫描窗口主相位）
    kScanning = 3,         // 预留：扫描进行中（当前 API 不进入，防御性超时支持）
    kCollecting = 4,       // 扫描完成，结果收集中（onScanDone 内瞬时相位）
    kRestoring = 5,        // 正在恢复显示（onScanDone 内瞬时相位；resumeDisplay 恰一次）
    kError = 6,            // 错误终态（显示已恢复；可重新 begin()）
    kDisconnected = 7,     // 会话断开终态（显示已恢复；不自动回 kIdle，需外部重新 begin()）
};

// 相位调试名（用于日志/诊断；不参与状态机逻辑）。
const char* scanPhaseName(ScanPhase phase);

// 显示暂停/恢复回调（Agent C 注入；OLED 未启动时由其返回 true / no-op）。
struct ScanTransactionCallbacks {
    std::function<bool()> suspendDisplay;  // 暂停显示；返回 false = 暂停失败（不进入挂起）
    std::function<void()> resumeDisplay;   // 恢复显示；本类保证每次会话至多调用一次
};

// Wi-Fi 扫描事务状态机：管理扫描期间的 OLED 暂停/恢复，保证任何终态路径恰好恢复一次。
class ScanTransaction {
public:
    ScanTransaction() = default;
    explicit ScanTransaction(ScanTransactionCallbacks callbacks);

    // 注入/替换回调（通常在 begin() 前注入；会话中途替换亦可，按新回调执行）。
    void setCallbacks(ScanTransactionCallbacks callbacks);

    // kIdle/kError/kDisconnected -> kPreparing；同步调用 suspendDisplay()：
    //   成功 -> 进入挂起（suspended 置位，等待 onScanStarted）；
    //   失败 -> kError（不进入挂起，也不恢复——本会话从未挂起）。
    // 活动相位（Preparing/DisplaySuspended/Scanning/Collecting/Restoring）重复调用
    // 为 no-op（double begin 幂等，不重复 suspend）。
    void begin();

    // kPreparing -> kDisplaySuspended（suspendedOk=true）；suspendedOk=false -> kError
    // （begin 内 suspend 已成功，故仍恢复显示恰一次）。其他相位调用为 no-op。
    void onScanStarted(bool suspendedOk);

    // kDisplaySuspended/kScanning -> kCollecting -> kRestoring（调用 resumeDisplay
    // 恰一次）-> kIdle（ok）或 kError（!ok，显示已恢复）。其他相位调用为 no-op。
    void onScanDone(bool ok);

    // 活动相位（kDisplaySuspended/kScanning/kCollecting）-> kError + 恢复；
    // kPreparing 直接 kError（begin 内 suspend 已成功，按不变量同样恢复）。
    // 空闲/终态调用为 no-op（不重复恢复，double resume 幂等）。
    void onTimeout();

    // 任何相位 -> kDisconnected；若当前挂起则恢复恰一次。此后停在 kDisconnected，
    // 不自动回 kIdle；恢复会话需外部重新调用 begin()。
    void onDisconnect();

    // 防御性看门狗：扫描窗口内首次调用锚定起点，此后 nowMs 差值 >= timeoutMs 即
    // onTimeout()。timeoutMs == 0 或非扫描窗口调用为 no-op。
    void tick(uint64_t nowMs, uint64_t timeoutMs);

    ScanPhase phase() const;
    bool displaySuspended() const;  // true = 本会话已成功 suspend 且尚未恢复

private:
    void restoreDisplayOnce();
    void resetWindowClock();

    ScanPhase phase_ = ScanPhase::kIdle;
    bool suspended_ = false;
    uint64_t scanStartMs_ = 0;  // 0 = 未锚定（tick 首次进入窗口时锚定）
    ScanTransactionCallbacks callbacks_;
};

}  // namespace wifi
}  // namespace espview