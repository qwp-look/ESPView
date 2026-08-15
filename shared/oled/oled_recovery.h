// ESPView M7-B — OLED 重新初始化恢复策略状态机（纯 C++17，零平台依赖）。
//
// 语义精确复刻 M7-A esp32/components/oled/src/oled_display.cpp taskLoop
// !busReady 分支的恢复逻辑：有界重试轮数 + 指数退避 + 冷却窗口，防止
// 无限制的重置风暴。host 测试与 ESP32 侧共用同一份源码。
#pragma once

#include <cstdint>

namespace espview {
namespace oled {

// 恢复策略：每次尝试前 tryBegin(now) 判定是否放行，成功 onSuccess 复位，
// 失败 onFailure 累计轮数并翻倍退避；达 maxCycles 进入 cooldown 冷却。
class ReinitPolicy {
public:
    struct Params {
        uint64_t backoffBaseMs = 500;
        uint64_t backoffMaxMs = 30000;
        uint32_t maxCycles = 3;
        uint64_t cooldownMs = 30000;
    };

    ReinitPolicy();
    explicit ReinitPolicy(const Params& p);
    bool tryBegin(uint64_t nowMs);   // 允许则预约下次尝试(now+backoff)并返回 true
    void onSuccess(uint64_t nowMs);  // 复位 backoff/cycles，预约 next=now+base
    void onFailure(uint64_t nowMs);  // cycles++；>=maxCycles 进入冷却并复位，否则 backoff 翻倍

    uint64_t nextAttemptMs() const;
    uint64_t cooldownUntilMs() const;
    uint32_t cycles() const;
    uint64_t backoffMs() const;

private:
    Params params_;
    uint64_t backoffMs_ = 0;      // 初始 = backoffBaseMs
    uint64_t nextAttemptMs_ = 0;  // 初始 0 → 首次 tryBegin(任意 now) 允许
    uint64_t cooldownUntilMs_ = 0;
    uint32_t cycles_ = 0;
};

}  // namespace oled
}  // namespace espview
