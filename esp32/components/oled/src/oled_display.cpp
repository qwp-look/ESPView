// ESPView M7-A/M7-B — OledDisplay 实现（低优先级任务 + 有界错误恢复）。
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
#include "oled/oled_display.hpp"

#include <algorithm>
#include <cstdint>
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
    : cfg_(cfg), provider_(std::move(provider)) {}

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
    s.state = static_cast<OledState>(state_.load(std::memory_order_relaxed));
    return s;
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
                    } else {
                        policy.onFailure(nowMs);
                        recordError(nowMs, 0);
                    }
                }
            } else {
                const StatusSnapshot snap = provider_();
                renderStatus(fb_, snap);
                if (uploadFrame(fb_)) {
                    ok_.store(true, std::memory_order_relaxed);
                    state_.store(static_cast<uint8_t>(OledState::kReady),
                                 std::memory_order_relaxed);
                    consecutiveErrors_.store(0, std::memory_order_relaxed);
                    flushCount_.fetch_add(1, std::memory_order_relaxed);
                    lastFlushMs_.store(nowMs, std::memory_order_relaxed);
                } else {
                    recordError(nowMs, 0);
                    const uint32_t cons = consecutiveErrors_.fetch_add(
                                              1, std::memory_order_relaxed) +
                                          1;
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
    // M7-B：停止谓词 —— 每段 transmit 前检查 running_，为 false 立即放弃（不再
    // 发送下一段）；stop() 通知后任务快速退出，I2C 资源留在退出路径释放。
    const std::function<bool()> stopPred = [this]() {
        return !running_.load(std::memory_order_acquire);
    };
    return executeInitSequence(*i2c_, ctrl, kMaxTransmitBytes, stopPred, firstError);
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
        // M7-B：每段 transmit 前检查停止请求 —— stop() 后不再发送下一段，
        // 任务快速退出，I2C 资源由退出路径释放。
        if (!running_.load(std::memory_order_acquire)) {
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