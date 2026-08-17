// ESPView M8-A5 — ReconnectPolicy 实现（见 reconnect_policy.h）。

#include "reconnect_policy.h"

namespace espview {
namespace wifi {

namespace {
uint32_t clampShift(uint32_t baseMs, uint32_t attempts, uint32_t maxShift, uint32_t maxMs) {
    const uint32_t shift = attempts > maxShift ? maxShift : attempts;
    const uint64_t v = static_cast<uint64_t>(baseMs) << shift;
    return v >= maxMs ? maxMs : static_cast<uint32_t>(v);
}
}  // namespace

void ReconnectPolicy::onAttempt(WifiFailureKind kind) {
    lastKind_ = kind;
    ++attempts_;
    if (kind == WifiFailureKind::kTerminal) {
        nextDelayMs_ = clampShift(kTerminalBaseMs, attempts_ - 1, 5u, kTerminalMaxMs);
    } else {
        nextDelayMs_ = clampShift(kTransientBaseMs, attempts_ - 1, 6u, kTransientMaxMs);
    }
}

void ReconnectPolicy::onSuccess() {
    attempts_ = 0;
    nextDelayMs_ = kTransientBaseMs;
    lastKind_ = WifiFailureKind::kTransient;
}

void ReconnectPolicy::stop() {
    stopped_ = true;
}

}  // namespace wifi
}  // namespace espview