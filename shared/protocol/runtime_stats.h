// ESPView M4 — 运行态统计与诊断基础层（纯 C++17，零平台依赖）。
//
// 规范来源：M4 spec §三（分域统计）/ §五-六（Heartbeat 可观察 + RTT 语义）/
//          §十九（错误分级）/ §二十（ring-buffer diagnostics）。
// 原则：
//   - 统计层不改变协议：本文件不定义任何 wire 字段，不触碰 Packet/Message 布局；
//   - 分域小 struct（Connection / Heartbeat / Frame / Packet / Input），
//     不合并成几百字段的巨型 struct；
//   - 计数器不回绕（uint64）；RTT 用 std::optional<uint32_t> 明确区分
//     「无测量」与「实际值」，禁止用 0 表示无测量（M3 遗留的 RTT=0 问题）。
//
// 本文件可被 ESP32 与 PC 共用（不依赖 ESP-IDF / Qt / Windows API）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace espview {
namespace proto {

// ---- 错误/事件分级（M4 spec §十九）----
enum class Severity : uint8_t {
    kInfo = 0,      // 正常状态变化（connected / committed）
    kWarning = 1,   // 可恢复异常（seq gap / CRC 一次 / 丢帧）
    kError = 2,     // 影响当前会话（transport disconnected / handshake timeout）
    kCritical = 3,  // 不可协商（protocol incompatible）
};

const char* toString(Severity s);

// ---- 诊断条目（M4 spec §二十：timestamp / severity / source / message）----
struct DiagnosticEntry {
    uint64_t timestampMs = 0;
    Severity severity = Severity::kInfo;
    std::string source;   // 事件来源（如 "transport" / "session" / "frame" / "input"）
    std::string message;  // 人类可读描述（不刷屏：累计计数 + 最近 N 条）
};

// ---- Ring-buffer 诊断（默认保留最近 50 条；与 Packet parser 解耦）----
class DiagnosticsRing {
public:
    static constexpr size_t kDefaultCapacity = 50;

    explicit DiagnosticsRing(size_t capacity = kDefaultCapacity);
    void push(uint64_t timestampMs, Severity severity, std::string source, std::string message);
    void push(const DiagnosticEntry& e);
    void clear();
    // P1-3：size/capacity/items/last 与 push 并发安全（mutex 保护）。
    size_t size() const;
    size_t capacity() const { return capacity_; }
    // 按时间顺序返回全部条目（最旧在前；副本）。
    std::vector<DiagnosticEntry> items() const;
    // 最近一条（生产代码无并发调用；返回指针在随后 push 后可能失效，
    // 仅限测试/单线程调试使用）。
    const DiagnosticEntry* last() const;

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<DiagnosticEntry> items_;
};

// ---- RTT 聚合（M4 spec §六：区分 no measurement / actual RTT；每会话重置）----
struct RttAggregate {
    std::optional<uint32_t> lastMs;  // 最近一次 PING→PONG 实测；nullopt = 无测量
    uint64_t samples = 0;            // 有效样本数
    uint32_t minMs = 0;
    uint32_t maxMs = 0;
    uint32_t avgMs = 0;              // samples>0 时有效（向下取整）
    uint64_t sumMs = 0;              // 内部累计，不回绕（uint64）

    void record(uint32_t rttMs);     // 追加一次有效测量
    void reset();                    // 会话重置：全部清空（无测量）
};

}  // namespace proto
}  // namespace espview
