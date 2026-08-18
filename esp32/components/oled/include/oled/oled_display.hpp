// ESPView M7-A/M7-B/M7-C2 — 独立 OLED 显示（ESP32 组件公共接口）。
//
// M7-A/M7-B：诊断状态显示（协议无关：不依赖 protocol/transport 与 LVGL；
// M7-C2 起依赖 shared/display 抽象 —— PhysicalDisplaySink 是 IDisplaySink
// 实现，应用帧经 shared/oled PhysicalRenderer 渲染）。状态由 main 经
// StatusProvider（std::function<StatusSnapshot()>）注入，任务内自行读取。
// 线程模型：
//   - start() 创建低优先级任务（默认 2，低于 session=5 / stats=3）；
//   - 任务每 refreshMs 调用 provider()（main 保证线程安全：读原子/普通量）、
//     渲染进 1KB 页式 framebuffer、按段上传（每段 ≤32B，含控制字节）；
//   - OLED 任务绝不触碰 protocol sendMutex / Transport；
//   - stop() 置停止标志 + 通知唤醒，有界等待任务退出；I2C 资源由任务自身
//     在退出路径释放（杜绝跨任务删除 bus 的 use-after-free）。
// 安全：OLED 不显示/不打印 Wi-Fi SSID/密码（状态快照无凭据字段）。
//
// M7-B：StatusSnapshot/OledConfig/renderStatus 已迁移到 shared/oled
//   （oled_status.h / oled_config.h，纯 C++17，host 可测）；本头只保留
//   OledStatus 运行时快照与 OledDisplay 生命周期接口。
// M7-C2：OledDisplay 扩展为 Physical Display Sink 的帧缓冲宿主 ——
//   PhysicalDisplaySink（physical_display_sink.hpp）经 presentScene()
//   用 SceneRenderer（shared/oled，M8-B B2）把共享 LogicalScene 同步
//   渲染进共享 1KB 应用 fb（appFb_，与 OLED 任务互斥共享，绝不持有
//   外部指针；RGB565 缩略路径已废除）；taskLoop 按场景分发：
//   Scene::kApplication + 应用帧启用 → 上传应用 fb（dirty 触发）；
//   否则走既有 renderStatus 诊断路径（系统诊断页独立于 router 持续
//   刷新）。I2C 上传只发生在 OLED 任务内（本类不对外暴露任何 I2C
//   操作；PhysicalDisplaySink 只做同步渲染）。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oled_config.h"   // shared/oled：OledConfig / validateOledConfig
#include "oled_preview.h"  // shared/oled（M7-D2）：OledPreviewSlot 预览槽（seqlock）
#include "oled_cmd.h"      // shared/oled：ControllerType（espview::oled 命名空间）
#include "scene_renderer.h"     // shared/oled（M8-B B2）：SceneRenderer（LogicalScene → 128x64 mono）
#include "oled_status.h"   // shared/oled：StatusSnapshot / renderStatus

namespace espview {
namespace oled {

// M7-B：OLED 生命周期状态（status().state；错误恢复期间保持 kDegraded）。
// M7-E：kSuspendedForWifiScan 为 Wi-Fi 扫描挂起观察态（挂起是正交原子标志，
// 不落 state_；status() 在挂起期间把它报告为 kSuspendedForWifiScan）。0..4
// 既有数值保持不变。
enum class OledState : uint8_t {
    kDisabled = 0,     // 任务未运行 / 已退出
    kInitializing = 1, // start() 成功，初始化中/待初始化
    kReady = 2,        // 初始化成功，正常刷新
    kDegraded = 3,     // 上传/初始化失败，恢复中
    kStopping = 4,     // stop() 已请求，等待任务退出
    kSuspendedForWifiScan = 5,  // M7-E：Wi-Fi 扫描临界区挂起（I2C 流量=0，仅休眠）
};

// 只读状态快照（status() 返回；错误计数只增）。M7-B 增补 oled* 指标：
//   initCount / flushCount / probeCount / recoverCount / lastErrorCode /
//   lastFlushMs / lastFlushDurationMs / state（DESIGN.md Z 节 M7-B 指标清单）。
struct OledStatus {
    bool ok = false;                        // 最近一次刷新/初始化成功
    OledState state = OledState::kDisabled; // 生命周期状态（M7-B）
    uint8_t address = 0;                    // 实际地址（探测结果或显式配置）
    ControllerType controller = ControllerType::kAuto;  // 实际控制器
    uint64_t errorCount = 0;                // I2C 错误累计（只增）
    uint64_t refreshCount = 0;              // 刷新尝试次数（含失败）
    uint64_t initCount = 0;                 // 成功 init（含重建）次数
    uint64_t flushCount = 0;                // 成功帧上传（flush）次数
    uint64_t probeCount = 0;                // I2C 探测（probe）尝试次数
    uint64_t recoverCount = 0;              // 恢复动作（bus reset/重建）次数
    uint32_t lastErrorCode = 0;             // 最近一次错误码（esp_err_t 值，0=无）
    uint64_t lastErrorMs = 0;               // 最近一次错误时刻（esp_timer ms）
    uint64_t lastFlushMs = 0;               // 最近一次成功上传时刻（esp_timer ms）
    uint64_t lastFlushDurationMs = 0;       // 最近一次成功上传耗时（esp_timer ms；M7-B）
};

class OledI2c;  // 内部 I2C 总线封装（src/oled_i2c.h，不进公共头）

class OledDisplay {
public:
    using StatusProvider = std::function<StatusSnapshot()>;

    // M7-C2：物理显示场景（taskLoop 上传分发依据；由 PhysicalDisplaySink
    // 按 SET_MODE 模式映射设置 —— Mirror/PhysicalOnly→Application，
    // Split/VirtualOnly→Diagnostics）。数值与 display::PhysicalScene 对齐
    // （0=Diagnostics, 1=Application），但本头不依赖 shared/display。
    enum class Scene : uint8_t {
        kDiagnostics = 0,  // 系统诊断页（renderStatus；独立于 router 持续刷新）
        kApplication = 1,  // LVGL 应用缩略帧（dirty 标记触发上传）
    };

    OledDisplay(const OledConfig& cfg, StatusProvider provider);
    ~OledDisplay();

    OledDisplay(const OledDisplay&) = delete;
    OledDisplay& operator=(const OledDisplay&) = delete;

    // 创建低优先级任务并启动刷新循环；配置非法返回 false（幂等）。
    bool start();
    // 停止任务并等待退出（有界）；I2C 资源由任务退出路径释放；幂等。
    void stop();
    OledStatus status() const;

    // ---- M7-C2：场景/应用帧控制（跨任务安全；均为原子操作）----
    // 场景切换（PhysicalDisplaySink::setScene 转发；SET_MODE 后调用）。
    void setScene(Scene scene);
    Scene scene() const;
    // 应用帧接收开关（PhysicalDisplaySink::setEnabled 转发；false = 应用帧
    // 禁用，taskLoop 回退 renderStatus 路径）。
    void setAppFramesEnabled(bool enabled);
    bool appFramesEnabled() const;
    // 应用帧呈现（UI/LVGL 任务调用）：用 SceneRenderer 同步渲染共享
    // LogicalScene 进 appFb_ 并置 dirty（绝不持有外部指针；与 OLED 任务
    // 互斥）。场景与启用状态由调用方（PhysicalDisplaySink）保证；渲染为
    // 纯 C++17 亚毫秒级（128x64 全量重绘），不持有任何像素引用。
    void presentScene(const display::LogicalScene& scene);

    // ---- M7-D2：Physical Preview 预览槽 ----
    // 内容确定点（taskLoop 上传 fb_ 后）写入；发送任务经
    // snapshot() 取 1024B 稳定像素快照并递增 frameId（AE.2 编码归 protocol 层，M8-A4）；
    // reset() 由会话侧在握手/断线时调用（AE.3）。OLED 任务只 store，不触碰
    // transport；槽为无锁 seqlock，写者零自旋。
    OledPreviewSlot& previewSlot() { return previewSlot_; }
    const OledPreviewSlot& previewSlot() const { return previewSlot_; }

    // ---- M7-E：Wi-Fi 扫描暂停/恢复（跨任务安全；原子标志）----
    // 扫描临界区期间挂起：taskLoop 跳过一切 I2C/provider/预览槽工作（I2C
    // 流量=0，仅休眠），status().state 报告 kSuspendedForWifiScan。与 stop()/
    // I2C 生命周期正交：挂起中 stop() 仍安全退出（I2C 资源仍由任务退出路径
    // 释放）；恢复后按既有刷新节奏继续，previewSlot 保持最新帧语义。本特性
    // 绝不关闭/重建 I2C bus，绝不 double-release。调用方：Wi-Fi 扫描任务
    //（main/espview 侧）；OLED 任务自身不调用。
    bool pauseForWifiScan();        // 幂等；任务未运行时 no-op（返回 true）；任务运行中置挂起
    bool resumeAfterWifiScan();     // 幂等；清除挂起；返回本次调用前是否处于挂起
    bool isSuspendedForWifiScan() const;  // 挂起查询

private:
    static void taskEntry(void* arg);
    void taskLoop();
    bool initOnce();                 // bus 创建/探测/addDevice + 初始化序列 + 清屏
    bool runInitSequence(esp_err_t* firstError = nullptr);  // init 命令 + 清屏上传
    bool uploadFrame(const OledFb& fb);
    void recordError(uint64_t nowMs, int32_t errorCode);
    void recordRecover();
    void handleUploadResult(uint64_t nowMs, bool uploaded, bool& busReady);

    OledConfig cfg_;
    StatusProvider provider_;
    std::unique_ptr<OledI2c> i2c_;
    OledFb fb_;                      // 1KB 页式 framebuffer（诊断渲染/上传 staging）

    // M7-C2/M8-B B2：应用帧共享缓冲（PhysicalDisplaySink 渲染 / taskLoop 上传）。
    std::mutex appFbMutex_;
    OledFb appFb_;                                     // 1KB 应用帧（SceneRenderer 输出）
    SceneRenderer sceneRenderer_;                      // 128x64 场景渲染器（值成员，无堆分配）
    OledPreviewSlot previewSlot_;                      // M7-D2：1KB 预览槽（OLED 任务写）
    std::atomic<uint8_t> scene_{static_cast<uint8_t>(Scene::kDiagnostics)};
    std::atomic<bool> appFramesEnabled_{false};
    std::atomic<bool> appFrameDirty_{false};

    std::atomic<bool> running_{false};
    std::atomic<bool> taskExited_{false};
    TaskHandle_t task_ = nullptr;
    std::atomic<uint8_t> state_{static_cast<uint8_t>(OledState::kDisabled)};  // M7-B：生命周期状态
    std::atomic<bool> suspendedForWifiScan_{false};  // M7-E：Wi-Fi 扫描挂起标志（正交于生命周期）

    // 状态快照（原子；status() 跨任务安全读取）。
    std::atomic<bool> ok_{false};
    std::atomic<uint8_t> address_{0};
    std::atomic<uint8_t> controller_{static_cast<uint8_t>(ControllerType::kAuto)};
    std::atomic<uint64_t> errorCount_{0};
    std::atomic<uint64_t> refreshCount_{0};
    std::atomic<uint64_t> initCount_{0};
    std::atomic<uint64_t> flushCount_{0};
    std::atomic<uint64_t> probeCount_{0};
    std::atomic<uint64_t> recoverCount_{0};
    std::atomic<uint32_t> lastErrorCode_{0};
    std::atomic<uint64_t> lastErrorMs_{0};
    std::atomic<uint64_t> lastFlushMs_{0};
    std::atomic<uint64_t> lastFlushDurationMs_{0};  // M7-B：最近一次成功上传耗时（esp_timer ms）
    std::atomic<uint32_t> consecutiveErrors_{0};
};

}  // namespace oled
}  // namespace espview