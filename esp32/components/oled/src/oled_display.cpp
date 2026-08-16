// ESPView M7-A/M7-B/M7-E — OledDisplay 实现（低优先级任务 + 有界错误恢复）。
//
// 线程模型：
//   - taskLoop 每 refreshMs：provider() → renderStatus → 分段上传；
//   - 上传失败 → errorCount++（只增）→ 连续失败达 maxReinit 触发恢复：
//       ① bus_reset + 重发 init/清屏（用现有设备句柄）；
//       ② 失败则整体重建（下次刷新经 initOnce：bus→probe→addDevice→init）；
//   - re-init 轮数每故障窗口有界（ReinitPolicy：maxCycles + 指数退避 + 冷却，
//     纯 C++17 策略，host 可测），不允许无限重置循环；degraded 时仍按
//     refresh 周期尝试（refreshCount 照常）；
//   - M7-B：I2C 资源（bus/device）只在任务退出路径释放（taskLoop 结束处），
//     stop() 只置停止标志 + 有界等待任务退出 —— 杜绝跨任务 i2c_del_master_bus
//     与任务内 transmit/probe 并发的 use-after-free（DESIGN.md Z 节）。
//   - M7-E：Wi-Fi 扫描临界区挂起（pauseForWifiScan/resumeAfterWifiScan 原子
//     正交标志）：挂起期间 taskLoop 跳过一切 I2C/provider/预览槽工作（I2C
//     流量=0，仅休眠），绝不关闭/重建 I2C bus；挂起中 stop() 仍安全退出。
#include "oled/oled_display.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"

#include "oled_config.h"     // shared/oled：validateOledConfig
#include "oled_controller.h"
#include "oled_i2c.h"
#include "oled_recovery.h"   // shared/oled：ReinitPolicy
#include "oled_status.h"     // shared/oled：StatusSnapshot / renderStatus

namespace espview {
namespace oled {

namespace {
constexpr char kTag[] = "espview_oled";
// I2C 单次 transmit 上限（含控制字节）；fb 上传按此分段（shared/oled 生成）。
constexpr size_t kMaxTransmitBytes = 32;
// 恢复策略参数（与 M7-A 一致，抽取为可测策略对象）：
constexpr uint64_t kReinitBackoffBaseMs = 500;
constexpr uint64_t kReinitBackoffMaxMs = 30000;
constexpr uint32_t kMaxReinitCycles = 3;    // 每个故障窗口最大 re-init 轮数
constexpr uint64_t kReinitCooldownMs = 30000;  // 放弃后的冷却窗口
// stop() 有界等待：覆盖任务单趟最坏阻塞（全地址探测 1.12s + init 清屏 1.8s）。
constexpr uint32_t kStopJoinTimeoutMs = 5000;
// M7-B：析构路径兜底等待（stop 业务路径 5s 不变；析构放宽到 15s）：任务退出前
// 绝不析构成员；仍不退出则打印 CRITICAL 后返回——宁可泄漏也不 use-after-free。
constexpr uint32_t kDtorJoinTimeoutMs = 15000;

}  // namespace

OledDisplay::OledDisplay(const OledConfig& cfg, StatusProvider provider)
    : cfg_(cfg), provider_(std::move(provider)) {
    // M7-C2：应用帧渲染器（128x64 单色；presentAppFrame 在互斥锁内使用）。
    appRenderer_ = std::make_unique<PhysicalRenderer>(OledFb::kWidth, OledFb::kHeight);
}

OledDisplay::~OledDisplay() {
    stop();
    // M7-B：stop() 仅业务路径（kStopJoinTimeoutMs=5s）有界等待；析构用更长兜底，
    // 保证任务退出后才允许成员析构（杜绝 use-after-free）。若 join 超时任务仍存活：
    // 打印 CRITICAL 后返回——宁可泄漏，也不释放任务仍在访问的资源（i2c_/fb_）。
    if (!taskExited_.load(std::memory_order_acquire)) {
        const uint64_t dtorDeadline =
            esp_timer_get_time() / 1000 + kDtorJoinTimeoutMs;
        while (!taskExited_.load(std::memory_order_acquire)) {
            const uint64_t nowMs = esp_timer_get_time() / 1000;
            if (nowMs >= dtorDeadline) {
                ESP_LOGE(kTag,
                         "~OledDisplay: task still alive after %u ms; "
                         "leaking resources to avoid use-after-free",
                         static_cast<unsigned>(kDtorJoinTimeoutMs));
                // 宁可泄漏也不释放任务仍在访问的资源：release() 让 OledI2c（bus/dev
                // 句柄）继续存活，任务退出路径对其 reset() 变为空操作（无 UAF）。
                i2c_.release();
                return;
            }
            ESP_LOGE(kTag, "~OledDisplay: waiting for task exit (%llu ms left)",
                     static_cast<unsigned long long>(dtorDeadline - nowMs));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

bool OledDisplay::start() {
    if (running_.load(std::memory_order_acquire)) {
        return true;  // 幂等（任务正在运行）
    }
    if (!taskExited_.load(std::memory_order_acquire) && task_ != nullptr) {
        // M7-B：上次 stop() join 超时，旧任务仍在运行（运行状态未确认退出）。
        // 禁止重启：否则会创建第二个任务与旧任务并存（I2C 资源双写/双重释放）。
        ESP_LOGE(kTag,
                 "start: previous task still running (join timeout); restart denied");
        return false;
    }
    if (!validateOledConfig(cfg_, 2u)) {
        ESP_LOGE(kTag, "start: invalid config");
        return false;
    }

    taskExited_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    const BaseType_t r = xTaskCreate(taskEntry, "espview_oled", cfg_.taskStack,
                                     this, static_cast<UBaseType_t>(cfg_.taskPriority),
                                     &task_);
    if (r != pdPASS) {
        running_.store(false, std::memory_order_release);
        task_ = nullptr;
        ESP_LOGE(kTag, "start: xTaskCreate failed");
        return false;
    }
    state_.store(static_cast<uint8_t>(OledState::kInitializing),
                 std::memory_order_relaxed);
    ESP_LOGI(kTag, "task started: prio=%lu stack=%lu refresh=%lums",
             static_cast<unsigned long>(cfg_.taskPriority),
             static_cast<unsigned long>(cfg_.taskStack),
             static_cast<unsigned long>(cfg_.refreshMs));
    return true;
}

void OledDisplay::stop() {
    if (!running_.exchange(false, std::memory_order_release)) {
        return;  // 未运行
    }
    // M7-E：挂起标志随 stop 清除（挂起是运行期正交状态；任务退出后不允许
    // 残留挂起导致下次 start() 直接进入挂起）。I2C 资源仍由任务退出路径释放。
    suspendedForWifiScan_.store(false, std::memory_order_release);
    state_.store(static_cast<uint8_t>(OledState::kStopping),
                 std::memory_order_relaxed);
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);  // 唤醒可能处于有界等待中的任务
        const uint64_t deadline =
            esp_timer_get_time() / 1000 + kStopJoinTimeoutMs;
        while (!taskExited_.load(std::memory_order_acquire)) {
            if (esp_timer_get_time() / 1000 >= deadline) {
                // M7-B：超时不置 task_ = nullptr —— 任务仍在运行，I2C 资源由任务
                // 退出路径释放；保留句柄，后续 start() 检测到任务未退出时拒绝重启
                //（运行状态未确认退出时禁止重启，防止双任务并存）。
                ESP_LOGW(kTag,
                         "stop: join timeout (%u ms); task still running, "
                         "resources released by task exit path",
                         static_cast<unsigned>(kStopJoinTimeoutMs));
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (taskExited_.load(std::memory_order_acquire)) {
            task_ = nullptr;  // 仅任务确认退出后才清句柄
        }
    }
    // M7-B：i2c_ 不在此处释放 —— 由 taskLoop 退出路径释放，避免跨任务
    // use-after-free（bus 删除与任务内 transmit/probe 并发）。
    ESP_LOGI(kTag, "stopped (i2c release deferred to task exit)");
}

OledStatus OledDisplay::status() const {
    OledStatus s;
    s.ok = ok_.load(std::memory_order_relaxed);
    s.address = address_.load(std::memory_order_relaxed);
    s.controller = static_cast<ControllerType>(
        controller_.load(std::memory_order_relaxed));
    s.errorCount = errorCount_.load(std::memory_order_relaxed);
    s.refreshCount = refreshCount_.load(std::memory_order_relaxed);
    s.initCount = initCount_.load(std::memory_order_relaxed);
    s.flushCount = flushCount_.load(std::memory_order_relaxed);
    s.probeCount = probeCount_.load(std::memory_order_relaxed);
    s.recoverCount = recoverCount_.load(std::memory_order_relaxed);
    s.lastErrorCode = lastErrorCode_.load(std::memory_order_relaxed);
    s.lastErrorMs = lastErrorMs_.load(std::memory_order_relaxed);
    s.lastFlushMs = lastFlushMs_.load(std::memory_order_relaxed);
    s.lastFlushDurationMs = lastFlushDurationMs_.load(std::memory_order_relaxed);
    // M7-E：挂起期间 state 报告 kSuspendedForWifiScan；底层生命周期状态保持
    // 不变（正交标志，不落 state_）。kStopping/kDisabled 优先 —— 挂起中 stop()
    // 仍能正确观察停止/退出。
    OledState lifecycle =
        static_cast<OledState>(state_.load(std::memory_order_relaxed));
    if (suspendedForWifiScan_.load(std::memory_order_acquire) &&
        lifecycle != OledState::kStopping &&
        lifecycle != OledState::kDisabled) {
        lifecycle = OledState::kSuspendedForWifiScan;
    }
    s.state = lifecycle;
    return s;
}

// ---- M7-E：Wi-Fi 扫描暂停/恢复（跨任务安全；原子标志）----
// 调用方为 Wi-Fi 扫描任务（main/espview 侧）；与 OLED 任务内的 taskLoop 以
// 原子标志通信。挂起不触碰 i2c_（绝不关闭/重建 bus、绝不 double-release）；
// 恢复后 taskLoop 按既有刷新节奏继续，previewSlot 保持最新帧语义。

bool OledDisplay::pauseForWifiScan() {
    // 幂等：任务运行期间置挂起标志（taskLoop 自此跳过一切 I2C/provider/预览槽
    // 工作，I2C 流量=0）；任务未运行时 no-op 返回 true（显示本就无 I2C 活动，
    // 扫描临界区天然安全，且不会设置会在下次 start() 生效的残留标志）。
    if (!running_.load(std::memory_order_acquire)) {
        return true;
    }
    suspendedForWifiScan_.store(true, std::memory_order_release);
    return true;
}

bool OledDisplay::resumeAfterWifiScan() {
    // 幂等：清除挂起标志；返回本次调用前是否处于挂起（"是否曾被挂起"）。
    return suspendedForWifiScan_.exchange(false, std::memory_order_acq_rel);
}

bool OledDisplay::isSuspendedForWifiScan() const {
    return suspendedForWifiScan_.load(std::memory_order_acquire);
}

// ---- M7-C2：场景/应用帧控制（跨任务安全；原子/互斥）----

void OledDisplay::setScene(Scene scene) {
    scene_.store(static_cast<uint8_t>(scene), std::memory_order_relaxed);
}

OledDisplay::Scene OledDisplay::scene() const {
    return static_cast<Scene>(scene_.load(std::memory_order_relaxed));
}

void OledDisplay::setAppFramesEnabled(bool enabled) {
    appFramesEnabled_.store(enabled, std::memory_order_relaxed);
}

bool OledDisplay::appFramesEnabled() const {
    return appFramesEnabled_.load(std::memory_order_relaxed);
}

// PhysicalDisplaySink::present 委托：用 PhysicalRenderer 同步渲染进共享
// appFb_ 并置 dirty。本方法在 UI/LVGL 任务执行，绝不持有 rgb565 指针
// （renderFrame 同步完成）；appFbMutex_ 与 OLED 任务的上传快照互斥。
void OledDisplay::presentAppFrame(int srcW, int srcH, const RenderRect& rect,
                                  const uint8_t* rgb565) {
    std::lock_guard<std::mutex> lock(appFbMutex_);
    if (appRenderer_ == nullptr || rgb565 == nullptr) {
        return;  // 渲染器未就绪 / 非法指针：忽略（调用方已校验场景与启用态）
    }
    appRenderer_->renderFrame(appFb_, srcW, srcH, rgb565, rect);
    appFrameDirty_.store(true, std::memory_order_release);
}

// M7-C2：上传结果统一收口（保持 M7-B Z.2/Z.4 语义：成功更新 ok/state/
// flushCount/lastFlushMs；失败 recordError + 有界 re-init 恢复；busReady
// 为 taskLoop 局部变量，重建路径经引用回落）。
void OledDisplay::handleUploadResult(uint64_t nowMs, bool uploaded, bool& busReady) {
    if (uploaded) {
        ok_.store(true, std::memory_order_relaxed);
        state_.store(static_cast<uint8_t>(OledState::kReady),
                     std::memory_order_relaxed);
        consecutiveErrors_.store(0, std::memory_order_relaxed);
        flushCount_.fetch_add(1, std::memory_order_relaxed);
        lastFlushMs_.store(nowMs, std::memory_order_relaxed);
        return;
    }
    recordError(nowMs, 0);
    // M7-E：挂起期间不触发 recover 动作（bus reset/reinit/重建均为 I2C 动作）。
    // 错误已记账；恢复动作延后到挂起解除后的失败路径（有界重试语义不变）。
    if (suspendedForWifiScan_.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t cons =
        consecutiveErrors_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cons >= cfg_.maxReinit) {
        consecutiveErrors_.store(0, std::memory_order_relaxed);
        // 恢复路径 ①：bus_reset + 重发 init/清屏。
        bool recovered = false;
        esp_err_t recErr = ESP_OK;
        if (i2c_ != nullptr && i2c_->valid()) {
            const esp_err_t r = i2c_->resetBus();
            if (r == ESP_OK) {
                recovered = runInitSequence(&recErr);
            } else {
                recErr = r;
            }
        }
        if (!recovered && recErr != ESP_OK) {
            lastErrorCode_.store(static_cast<uint32_t>(recErr),
                                 std::memory_order_relaxed);
        }
        if (!recovered) {
            // 恢复路径 ②：整体重建（下次刷新走 initOnce）。
            i2c_.reset();
            busReady = false;
        }
        recordRecover();
    }
}
void OledDisplay::taskEntry(void* arg) {
    static_cast<OledDisplay*>(arg)->taskLoop();
}

void OledDisplay::taskLoop() {
    bool busReady = false;
    ReinitPolicy policy(ReinitPolicy::Params{kReinitBackoffBaseMs,
                                             kReinitBackoffMaxMs,
                                             kMaxReinitCycles,
                                             kReinitCooldownMs});
    uint64_t nextRefreshMs = esp_timer_get_time() / 1000 + cfg_.refreshMs;

    while (running_.load(std::memory_order_acquire)) {
        const uint64_t nowMs = esp_timer_get_time() / 1000;
        if (nowMs >= nextRefreshMs) {
            nextRefreshMs = nowMs + cfg_.refreshMs;

            // M7-E：Wi-Fi 扫描挂起 —— 跳过一切 I2C/provider/预览槽工作（I2C
            // 流量=0，仅休眠）。挂起期间不触发任何 recover 动作（无 I2C 即无新
            // 错误；恢复后继续既有生命周期：kReady 刷新 / !busReady 按策略重试
            // init）。refreshCount 只计实际刷新尝试（挂起周期不计）。
            if (suspendedForWifiScan_.load(std::memory_order_acquire)) {
                continue;
            }
            refreshCount_.fetch_add(1, std::memory_order_relaxed);

            if (!busReady) {
                // 初始化 / 重建：有界轮数 + 指数退避 + 冷却（ReinitPolicy）。
                if (policy.tryBegin(nowMs)) {
                    if (initOnce()) {
                        busReady = true;
                        policy.onSuccess(nowMs);
                        ok_.store(true, std::memory_order_relaxed);
                        state_.store(static_cast<uint8_t>(OledState::kReady),
                                     std::memory_order_relaxed);
                    } else if (!suspendedForWifiScan_.load(std::memory_order_acquire)) {
                        // M7-E：挂起中止的 init 不记为失败（非 I2C 错误，不消耗
                        // 恢复预算；恢复后按既有策略继续重试）。
                        policy.onFailure(nowMs);
                        recordError(nowMs, 0);
                    }
                }
            } else {
                // M7-C2：场景分发（PhysicalDisplaySink 经 setScene/setAppFramesEnabled
                // 设置；模式映射见 main.cpp）。Application + 应用帧启用 → dirty 触发
                // 上传应用 fb；Diagnostics 或应用帧未启用 → 既有 renderStatus 诊断路径
                //（系统诊断页独立于 router 持续刷新）。I2C 上传只在本任务内进行。
                const bool appScene = scene() == Scene::kApplication &&
                                      appFramesEnabled();
                if (appScene) {
                    // M7-E：挂起与任务周期并发的兜底检查 —— 挂起中不消费 dirty
                    //（保留 dirty：恢复后仍上传最新帧），不写预览槽。
                    if (suspendedForWifiScan_.load(std::memory_order_acquire)) {
                        continue;
                    }
                    if (appFrameDirty_.exchange(false)) {
                        // 快照 1KB 应用帧：锁内拷贝到上传 staging（fb_），锁外分段上传，
                        // 避免上传期间阻塞 UI/LVGL 侧的同步渲染。
                        {
                            std::lock_guard<std::mutex> lock(appFbMutex_);
                            std::memcpy(fb_.data(), appFb_.data(), OledFb::kSizeBytes);
                        }
                        // M7-D2：内容确定点 1 —— 应用帧快照写入预览槽（AE.3）。
                        previewSlot_.store(fb_.data());
                        const bool uploaded = uploadFrame(fb_);
                        if (!uploaded &&
                            suspendedForWifiScan_.load(std::memory_order_acquire)) {
                            // M7-E：挂起中止的上传不是 I2C 错误：不记账不触发
                            // recover；恢复 dirty，恢复后仍上传最新帧。
                            appFrameDirty_.store(true, std::memory_order_release);
                            continue;
                        }
                        handleUploadResult(nowMs, uploaded, busReady);
                    }
                } else if (!appScene) {
                    // M7-E：挂起兜底 —— 跳过 provider/渲染/预览槽/上传。
                    if (suspendedForWifiScan_.load(std::memory_order_acquire)) {
                        continue;
                    }
                    const StatusSnapshot snap = provider_();
                    renderStatus(fb_, snap);
                    // M7-D2：内容确定点 2 —— 诊断页快照写入预览槽（AE.3）。
                    previewSlot_.store(fb_.data());
                    const bool uploaded = uploadFrame(fb_);
                    if (!uploaded &&
                        suspendedForWifiScan_.load(std::memory_order_acquire)) {
                        continue;  // M7-E：挂起中止，非 I2C 错误，不记账不恢复
                    }
                    handleUploadResult(nowMs, uploaded, busReady);
                }
                // appScene 且无新帧：跳过本周期上传（保持最后画面；dirty 由 UI 侧置位）。
            }

        }

        // 有界等待（≤50ms 粒度）；stop() 通知立即唤醒退出。
        const uint64_t waitMs =
            nextRefreshMs > nowMs ? nextRefreshMs - nowMs : 0;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<uint64_t>(waitMs, 50)));
    }

    // M7-B：唯一允许释放 I2C 资源的位置（任务自身退出路径）。
    i2c_.reset();
    state_.store(static_cast<uint8_t>(OledState::kDisabled),
                 std::memory_order_relaxed);
    taskExited_.store(true, std::memory_order_release);  // release：与 stop()/析构的 acquire 配对
}

bool OledDisplay::initOnce() {
    if (i2c_ == nullptr) {
        auto i2c = std::make_unique<OledI2c>(cfg_);
        if (i2c->init() != ESP_OK) {
            recordError(esp_timer_get_time() / 1000, 0);
            return false;  // 局部对象析构自动 deinit
        }
        const uint8_t addr = cfg_.addrAuto ? i2c->probe() : cfg_.address;
        if (cfg_.addrAuto) {
            probeCount_.fetch_add(1, std::memory_order_relaxed);
        }
        if (addr == 0) {
            ESP_LOGW(kTag, "init: no display on bus");
            return false;
        }
        if (i2c->addDevice(addr) != ESP_OK) {
            return false;
        }
        i2c_ = std::move(i2c);
        address_.store(addr, std::memory_order_relaxed);
    }
    const ControllerType ctrl = resolveController(cfg_.controller);
    controller_.store(static_cast<uint8_t>(ctrl), std::memory_order_relaxed);
    esp_err_t initErr = ESP_OK;
    if (!runInitSequence(&initErr)) {
        recordError(esp_timer_get_time() / 1000,
                    initErr != ESP_OK ? static_cast<int32_t>(initErr) : 0);
        return false;
    }
    initCount_.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGI(kTag, "display ready: addr=0x%02X ctrl=%s",
             static_cast<unsigned>(address_.load(std::memory_order_relaxed)),
             controllerName(ctrl));
    return true;
}

bool OledDisplay::runInitSequence(esp_err_t* firstError) {
    if (i2c_ == nullptr || !i2c_->valid()) {
        return false;
    }
    const ControllerType ctrl = static_cast<ControllerType>(
        controller_.load(std::memory_order_relaxed));
    // M7-B/M7-E：中止谓词 —— 每段 transmit 前检查 running_ 与挂起标志；停止
    // 请求或挂起置位后立即放弃（不再发送下一段）。停止时任务快速退出、I2C
    // 资源留在退出路径释放；挂起时保证挂起窗口内 I2C 流量≈0（仅段内已提交
    // 的部分可能完成）。
    const std::function<bool()> abortPred = [this]() {
        return !running_.load(std::memory_order_acquire) ||
               suspendedForWifiScan_.load(std::memory_order_acquire);
    };
    return executeInitSequence(*i2c_, ctrl, kMaxTransmitBytes, abortPred, firstError);
}

bool OledDisplay::uploadFrame(const OledFb& fb) {
    if (i2c_ == nullptr || !i2c_->valid()) {
        return false;
    }
    const ControllerType ctrl = static_cast<ControllerType>(
        controller_.load(std::memory_order_relaxed));
    const auto segs = segmentFrameUpload(fb, ctrl, kMaxTransmitBytes);
    // M7-B：上传耗时统计（首段前起表，末段后结表；成功才写入）。
    const int64_t flushStartUs = esp_timer_get_time();
    for (const auto& seg : segs) {
        // M7-B/M7-E：每段 transmit 前检查停止请求/挂起 —— stop() 后不再发送
        // 下一段（任务快速退出，I2C 资源由退出路径释放）；挂起后不再发送下一
        // 段（挂起窗口 I2C 流量≈0；挂起中止由调用侧区分，不记账）。
        if (!running_.load(std::memory_order_acquire) ||
            suspendedForWifiScan_.load(std::memory_order_acquire)) {
            return false;
        }
        const esp_err_t err = i2c_->transmit(seg.data(), seg.size());
        if (err != ESP_OK) {
            lastErrorCode_.store(static_cast<uint32_t>(err),
                                 std::memory_order_relaxed);
            return false;
        }
    }
    lastFlushDurationMs_.store(
        static_cast<uint64_t>((esp_timer_get_time() - flushStartUs) / 1000),
        std::memory_order_relaxed);
    return true;
}

void OledDisplay::recordError(uint64_t nowMs, int32_t errorCode) {
    errorCount_.fetch_add(1, std::memory_order_relaxed);
    if (errorCode != 0) {
        lastErrorCode_.store(static_cast<uint32_t>(errorCode),
                             std::memory_order_relaxed);
    }
    lastErrorMs_.store(nowMs, std::memory_order_relaxed);
    ok_.store(false, std::memory_order_relaxed);
    // M7-B：上传/初始化失败 → kDegraded；错误恢复期间保持 kDegraded，
    // 直到下次初始化/上传成功才回到 kReady。
    state_.store(static_cast<uint8_t>(OledState::kDegraded),
                 std::memory_order_relaxed);
}

void OledDisplay::recordRecover() {
    recoverCount_.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace oled
}  // namespace espview