// ESPView M7-A/M7-B — 独立 OLED 状态显示（ESP32 组件公共接口）。
//
// 协议无关：本组件不依赖 protocol/display/lvgl/transport；状态由 main 经
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

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oled_config.h"   // shared/oled：OledConfig / validateOledConfig
#include "oled_cmd.h"      // shared/oled：ControllerType（espview::oled 命名空间）
#include "oled_status.h"   // shared/oled：StatusSnapshot / renderStatus

namespace espview {
namespace oled {

// M7-B：OLED 生命周期状态（status().state；错误恢复期间保持 kDegraded）。
enum class OledState : uint8_t {
    kDisabled = 0,     // 任务未运行 / 已退出
    kInitializing = 1, // start() 成功，初始化中/待初始化
    kReady = 2,        // 初始化成功，正常刷新
    kDegraded = 3,     // 上传/初始化失败，恢复中
    kStopping = 4,     // stop() 已请求，等待任务退出
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

    OledDisplay(const OledConfig& cfg, StatusProvider provider);
    ~OledDisplay();

    OledDisplay(const OledDisplay&) = delete;
    OledDisplay& operator=(const OledDisplay&) = delete;

    // 创建低优先级任务并启动刷新循环；配置非法返回 false（幂等）。
    bool start();
    // 停止任务并等待退出（有界）；I2C 资源由任务退出路径释放；幂等。
    void stop();
    OledStatus status() const;

private:
    static void taskEntry(void* arg);
    void taskLoop();
    bool initOnce();                 // bus 创建/探测/addDevice + 初始化序列 + 清屏
    bool runInitSequence(esp_err_t* firstError = nullptr);  // init 命令 + 清屏上传
    bool uploadFrame(const OledFb& fb);
    void recordError(uint64_t nowMs, int32_t errorCode);
    void recordRecover();

    OledConfig cfg_;
    StatusProvider provider_;
    std::unique_ptr<OledI2c> i2c_;
    OledFb fb_;                      // 1KB 页式 framebuffer（BSS 一次性分配）

    std::atomic<bool> running_{false};
    std::atomic<bool> taskExited_{false};
    TaskHandle_t task_ = nullptr;
    std::atomic<uint8_t> state_{static_cast<uint8_t>(OledState::kDisabled)};  // M7-B：生命周期状态

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