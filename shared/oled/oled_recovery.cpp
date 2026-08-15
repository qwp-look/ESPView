// ESPView M7-B — ReinitPolicy 实现（复刻 M7-A taskLoop !busReady 恢复逻辑）。
#include "oled_recovery.h"

#include <algorithm>

namespace espview {
namespace oled {

ReinitPolicy::ReinitPolicy(const Params& p) : params_(p) {
    backoffMs_ = params_.backoffBaseMs;
    nextAttemptMs_ = 0;
    cooldownUntilMs_ = 0;
    cycles_ = 0;
}

ReinitPolicy::ReinitPolicy() : ReinitPolicy(Params{}) {}

bool ReinitPolicy::tryBegin(uint64_t nowMs) {
    if (nowMs < cooldownUntilMs_ || nowMs < nextAttemptMs_) {
        return false;
    }
    nextAttemptMs_ = nowMs + backoffMs_;
    return true;
}

void ReinitPolicy::onSuccess(uint64_t nowMs) {
    cycles_ = 0;
    backoffMs_ = params_.backoffBaseMs;
    nextAttemptMs_ = nowMs + params_.backoffBaseMs;
}

void ReinitPolicy::onFailure(uint64_t nowMs) {
    ++cycles_;
    if (cycles_ >= params_.maxCycles) {
        cooldownUntilMs_ = nowMs + params_.cooldownMs;
        cycles_ = 0;
        backoffMs_ = params_.backoffBaseMs;
    } else {
        backoffMs_ = std::min(backoffMs_ * 2, params_.backoffMaxMs);
    }
}

uint64_t ReinitPolicy::nextAttemptMs() const {
    return nextAttemptMs_;
}

uint64_t ReinitPolicy::cooldownUntilMs() const {
    return cooldownUntilMs_;
}

uint32_t ReinitPolicy::cycles() const {
    return cycles_;
}

uint64_t ReinitPolicy::backoffMs() const {
    return backoffMs_;
}

}  // namespace oled
}  // namespace espview
