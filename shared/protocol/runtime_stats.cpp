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
    items_.push_back(e);
    while (items_.size() > capacity_) {
        items_.pop_front();
    }
}

void DiagnosticsRing::clear() {
    items_.clear();
}

std::vector<DiagnosticEntry> DiagnosticsRing::items() const {
    return std::vector<DiagnosticEntry>(items_.begin(), items_.end());
}

const DiagnosticEntry* DiagnosticsRing::last() const {
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
