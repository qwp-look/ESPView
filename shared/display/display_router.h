// ESPView M7-C1 — DisplayRouter：Display Sink 路由状态机（纯 C++17，零平台依赖）。
//
// 规范来源：M7-C 任务书（§33 降级语义 / §34 测试）+ docs/DESIGN.md D.2/F 节
// （DisplayManager / Mirror 扇出语义）。C1 只做接口与状态语义；
// PhysicalDisplay 渲染与 PhysicalScene 的真实内容选择是 C2。
//
// 职责：
//   - 持有 Virtual / Physical 两个 IDisplaySink（shared_ptr，可空）；
//   - setMode(DisplayRouteMode) 安全切换（见下）；
//   - writeRect/flush 生产者路径按模式扇出到启用中的 sink；
//   - Split 模式为 C2 预留 presentScene(PhysicalScene, ...)：应用帧只走
//     virtual，物理侧独立场景帧经 presentScene 显式提交（C1 只实现接口
//     与状态语义，不做内容生成）。
//
// 状态机：
//   kIdle      初始 / 未选择模式；写路径返回 kNotEnabled
//   kSwitching setMode 进行中（sink 已全部 disable）；写路径返回 kNotEnabled
//   kConnected 所有启用中的 sink 均 isAvailable()
//   kDegraded  部分启用中的 sink 不可用（Mirror 物理不可用 → virtual 继续；
//              Split 两 sink 独立统计）
//
// setMode 安全切换序列（持互斥锁，对生产者原子）：
//   1) 校验 mode（0..3；必需 sink 缺失 → kInvalidParam，不进入切换）；
//   2) 置 kSwitching；
//   3) disable 所有 sink（两个都做，幂等）；
//   4) stale clear 钩子（onStaleClear：生产者丢弃半帧 / 清空 PC 基准）；
//   5) 按模式 enable（VirtualOnly → virtual；PhysicalOnly → physical；
//      Mirror / Split → 两者）；
//   6) FULL resync 钩子（onFullResync：生产者置 needFull，下一帧 FULL）；
//   7) 依据 isAvailable() 收敛到 kConnected / kDegraded，返回 kOk。
//
// 降级语义（任务 §33）：
//   - Mirror 时 physical 不可用 → kDegraded，但 virtual 继续收帧（写路径
//     按 isAvailable() 跳过不可用 sink，不阻塞可用 sink）；
//   - Split 两个 sink 独立：virtual 收应用帧（writeRect），physical 只经
//     presentScene 收独立场景帧，各自的可用性独立评估。
//
// 返回聚合规则（与 DESIGN.md F 节「任一后端失败不阻塞 UI」一致）：
//   任一启用且可用的 sink 成功 → kOk；全部不可用 → kNotConnected；
//   切换窗口 / 未选择模式 → kNotEnabled。present() 错误（如背压）只在
//   无任何 sink 成功时向上传播（Mirror 中 virtual 的 kQueueFull 由
//   RemoteDisplay 自己的背压路径处理，本层不放大）。
//
// 线程模型：setMode 与写路径共用同一把互斥锁（切换窗口对生产者原子）；
// 钩子（stale clear / FULL resync）在锁内同步触发（约定为短、非阻塞）；
// 钩子内不得调用 Router 的锁方法（mode()/state()/writeRect 等），避免死锁。
// 错误路径不使用异常。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "display.h"               // DisplayStatus

#include "protocol.h"
#include "display_capabilities.h"  // DisplaySinkKind
#include "display_sink.h"          // IDisplaySink / Rect

namespace espview {
namespace display {

// 路由模式（值与 wire SET_MODE.mode 对齐：0/1/2/3 =
// proto::DisplayMode kWindow/kDevice/kMirror/kSplit）。
enum class DisplayRouteMode : uint8_t {
    kVirtualOnly = 0,
    kPhysicalOnly = 1,
    kMirror = 2,
    kSplit = 3,
};

// ---- wire SET_MODE.mode 显式转换（M8-A4：唯一转换点，禁止散落 static_cast）----
// DisplayRouteMode ↔ wire byte 0..3（proto::DisplayMode 值）。proto::DisplayMode
// 是 wire 侧唯一权威；本组函数是 shared/display 与 wire 的边界转换，PC/ESP32
// 一律经此转换（§八「只保留一个 canonical mode，其他层经 conversion 映射」）。
inline uint8_t toWireMode(DisplayRouteMode mode) {
    switch (mode) {
        case DisplayRouteMode::kVirtualOnly: return 0;
        case DisplayRouteMode::kPhysicalOnly: return 1;
        case DisplayRouteMode::kMirror: return 2;
        case DisplayRouteMode::kSplit: return 3;
    }
    return 0;  // 不可达（enum 全覆盖）
}

// DisplayRouteMode → wire 协议枚举（单一集中转换点；依赖 toWireMode 的映射）。
inline proto::DisplayMode toProtoMode(DisplayRouteMode mode) {
    return static_cast<proto::DisplayMode>(toWireMode(mode));
}

// wire byte → DisplayRouteMode；非法值（>3）返回 nullopt（调用方拒绝）。
inline std::optional<DisplayRouteMode> fromWireMode(uint8_t wire) {
    switch (wire) {
        case 0: return DisplayRouteMode::kVirtualOnly;
        case 1: return DisplayRouteMode::kPhysicalOnly;
        case 2: return DisplayRouteMode::kMirror;
        case 3: return DisplayRouteMode::kSplit;
        default: return std::nullopt;
    }
}

// 物理侧场景（C2 预留）：Split 模式下物理 sink 的独立内容来源。
enum class PhysicalScene : uint8_t {
    kDiagnostics = 0,  // 诊断页（OLED 状态）
    kApplication = 1,  // 应用帧（缩放的 UI 帧）
};

// Router 状态（见文件头状态机）。
enum class RouterState : uint8_t {
    kIdle = 0,
    kSwitching = 1,
    kConnected = 2,
    kDegraded = 3,
};

class DisplayRouter {
public:
    DisplayRouter() = default;
    ~DisplayRouter() = default;

    DisplayRouter(const DisplayRouter&) = delete;
    DisplayRouter& operator=(const DisplayRouter&) = delete;

    // ---- sink 注册（nullptr = 卸载）----
    // 约定：sink 拥有方在 attach 前完成 init(caps)；Router 只查询
    // capabilities()/isAvailable()。attach 不改变当前模式与状态，
    // 之后需重新 setMode() 使新 sink 生效。
    void attachVirtual(std::shared_ptr<IDisplaySink> sink);
    void attachPhysical(std::shared_ptr<IDisplaySink> sink);

    // ---- 模式切换（安全切换序列见文件头）----
    // mode 非法（> kSplit）或必需 sink 未 attach → kInvalidParam；
    // 成功 → kOk，state() 收敛到 kConnected / kDegraded。
    DisplayStatus setMode(DisplayRouteMode mode);
    DisplayRouteMode mode() const;
    RouterState state() const;

    // ---- 钩子 ----
    // stale clear：切换窗口内清空旧画面/旧帧基准（FrameAssembler 复位、
    // PC 清屏等，由应用注入）。FULL resync：切模式成功后请求下一帧 FULL。
    void setStaleClearCallback(std::function<void()> cb);
    void setFullResyncCallback(std::function<void()> cb);

    // ---- 生产者路径（Application / LVGL 侧）----
    DisplayStatus writeRect(const Rect& rect, const uint8_t* pixels);

    // M8-A4：writeRect 的逐路径结果（lvgl flush_cb 判断 Virtual 背压用）。
    // 聚合规则不变（任一启用且可用的 sink 成功 → overall kOk）；extra 字段
    // 只报告「实际被调用」的路径状态：virtualInPath=false 表示 Virtual 不在
    // 应用帧路径（PhysicalOnly）或不可用（未调用 present）。
    struct RouteWriteResult {
        DisplayStatus overall = DisplayStatus::kNotEnabled;
        bool virtualInPath = false;
        std::optional<DisplayStatus> virtualStatus;
        std::optional<DisplayStatus> physicalStatus;
    };
    RouteWriteResult writeRectDetailed(const Rect& rect, const uint8_t* pixels);
    DisplayStatus flush();

    // ---- C2 预留：Split 模式下物理侧独立场景帧 ----
    // 仅 kSplit 模式接受（物理 sink 启用且可用）；其余模式 → kNotSupported。
    // C1 只实现接口与状态语义：按场景路由到物理 sink，不生成内容。
    DisplayStatus presentScene(PhysicalScene scene, const Rect& rect,
                               const uint8_t* pixels);

    // ---- M8-B B2：语义场景投递（Application → Physical OLED）----
    // 把共享 LogicalScene 投递给物理 sink：
    //   PhysicalOnly / Mirror → 物理侧显示 Application，渲染场景；
    //   VirtualOnly / Split   → 物理侧显示 Diagnostics（OLED 任务自绘），
    //                           返回 kOk 空操作（调用方不重试）。
    // 与 writeRect 正交：物理应用内容只经本路径（RGB565 缩略路径已废除）。
    DisplayStatus presentScene(const LogicalScene& scene);

    // ---- 查询 / 主动重评估 ----
    std::shared_ptr<IDisplaySink> virtualSink() const;
    std::shared_ptr<IDisplaySink> physicalSink() const;
    // 应用在 sink 可用性变化事件（transport 断线等）后主动调用。
    void refreshState();

private:
    void disableAllLocked();   // 已持 mutex_：disable 两个 sink（幂等，忽略结果）
    void reconcileStateLocked();  // 已持 mutex_：按 isAvailable() 收敛状态

    mutable std::mutex mutex_;
    std::shared_ptr<IDisplaySink> virtual_;
    std::shared_ptr<IDisplaySink> physical_;
    DisplayRouteMode mode_ = DisplayRouteMode::kVirtualOnly;
    RouterState state_ = RouterState::kIdle;
    std::function<void()> staleClearCb_;
    std::function<void()> fullResyncCb_;
};

}  // namespace display
}  // namespace espview
