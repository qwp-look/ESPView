// ESPView M4 — runtime_stats 实现（纯 C++17）。

#include "runtime_stats.h"

#include <algorithm>

namespace espview {
namespace proto {

const char* toString(Severity s) {
    switch (s) {
        case Severity::kInfo:
            return "INFO";
        case Severity::kWarning:
            return "WARNING";
        case Severity::kError:
            return "ERROR";
        case Severity::kCritical:
            return "CRITICAL";
    }
    return "?";
}

DiagnosticsRing::DiagnosticsRing(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void DiagnosticsRing::push(uint64_t timestampMs, Severity severity, std::string source,
                           std::string message) {
    push(DiagnosticEntry{timestampMs, severity, std::move(source), std::move(message)});
}

void DiagnosticsRing::push(const DiagnosticEntry& e) {
    std::lock_guard<std::mutex> lk(mutex_);
    items_.push_back(e);
    while (items_.size() > capacity_) {
        items_.pop_front();
    }
}

void DiagnosticsRing::clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    items_.clear();
}

size_t DiagnosticsRing::size() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return items_.size();
}

std::vector<DiagnosticEntry> DiagnosticsRing::items() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return std::vector<DiagnosticEntry>(items_.begin(), items_.end());
}

const DiagnosticEntry* DiagnosticsRing::last() const {
    // 并发 push 会使返回指针失效；生产代码不并发调用（仅测试/调试）。
    std::lock_guard<std::mutex> lk(mutex_);
    return items_.empty() ? nullptr : &items_.back();
}

void RttAggregate::record(uint32_t rttMs) {
    lastMs = rttMs;
    ++samples;
    sumMs += rttMs;
    if (samples == 1) {
        minMs = rttMs;
        maxMs = rttMs;
    } else {
        minMs = std::min(minMs, rttMs);
        maxMs = std::max(maxMs, rttMs);
    }
    avgMs = static_cast<uint32_t>(sumMs / samples);
}

void RttAggregate::reset() {
    lastMs.reset();
    samples = 0;
    minMs = 0;
    maxMs = 0;
    avgMs = 0;
    sumMs = 0;
}

}  // namespace proto
}  // namespace espview
