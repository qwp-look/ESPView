// ESPView M8-A2 — 确定性并发测试同步原语（纯 C++17，零平台依赖）。
//
// 背景：C++17 无 std::barrier/std::latch；测试需要 Gate（放行/单次交接）、
//   TestBarrier（N 方回合）、LockedQueue（线程安全字节/消息队列）来编排
//   RX feeder / ticker / app sender / stats reader 的精确 interleaving。
// 全部原语仅用于测试代码；生产代码不使用本文件。

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace espview {
namespace proto {
namespace test {

// ---- 一次性闸门（open() 放行全部等待者；可 reset 复用）----
class Gate {
public:
    void open() {
        // M8-A2 修复：lost-wakeup —— 必须在持锁下更新 open_，否则与 wait()
        // 的"持锁检查谓词→进入等待"竞态：open 置位+notify 可能在等待者进入
        // 等待队列之前发出，唤醒丢失 → 等待者直到超时才返回（C8/L1 flake：
        // sender 在 go.wait() 上丢失唤醒，30s 超时后才发送，feeder 已随
        // stopAll 停止 → 接收端精确计数失败）。持锁更新 + 锁外 notify 是
        // condition_variable 标准用法（谓词检查与状态更新同锁互斥）。
        {
            std::lock_guard<std::mutex> lock(m_);
            open_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    }
    void close() {
        // 与 open() 同理：状态更新与 wait() 谓词检查同锁互斥。
        std::lock_guard<std::mutex> lock(m_);
        open_.store(false, std::memory_order_release);
    }
    bool isOpen() const {
        return open_.load(std::memory_order_acquire);
    }
    // 阻塞直到 open()；超时返回 false（看门狗用）。
    bool wait(int timeoutMs = 30000) const {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this] { return open_.load(std::memory_order_acquire); });
    }

private:
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::atomic<bool> open_{false};
};

// 注：CountGate / TestBarrier 当前未被任何测试引用（M8-A2 全部确定性编排只用
// Gate + LockedQueue + testHooks）；保留作为后续确定性并发测试的同步原语，
// 避免重复实现（最小改动：不删除）。
// ---- 计数闸门：等待恰有 n 个到达者（屏障的"先到先等"版本）----
class CountGate {
public:
    explicit CountGate(size_t n) : n_(n) {}
    void arrive() {
        std::lock_guard<std::mutex> lock(m_);
        ++arrived_;
        cv_.notify_all();
    }
    // 阻塞直到 arrived >= n。
    bool wait(int timeoutMs = 30000) const {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this] { return arrived_ >= n_; });
    }
    size_t arrived() const {
        std::lock_guard<std::mutex> lock(m_);
        return arrived_;
    }

private:
    size_t n_;
    size_t arrived_ = 0;
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
};

// ---- N 方回合屏障（每轮所有参与者到齐后一起放行；可复用多轮）----
class TestBarrier {
public:
    explicit TestBarrier(size_t n) : n_(n) {}
    bool wait(int timeoutMs = 30000) {
        std::unique_lock<std::mutex> lock(m_);
        const size_t gen = generation_;
        ++arrived_;
        if (arrived_ >= n_) {
            arrived_ = 0;
            ++generation_;
            cv_.notify_all();
            return true;
        }
        return cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this, gen] { return gen != generation_; });
    }

private:
    size_t n_;
    size_t arrived_ = 0;
    size_t generation_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
};

// ---- 线程安全字节/消息队列（互斥 + condvar；conductor ↔ 角色线程）----
template <typename T>
class LockedQueue {
public:
    void push(T v) {
        {
            std::lock_guard<std::mutex> lock(m_);
            q_.push_back(std::move(v));
        }
        cv_.notify_one();
    }
    // 阻塞取一个；超时返回 false（stop 信号时也返回 false）。
    bool pop(T& out, int timeoutMs = 30000) {
        std::unique_lock<std::mutex> lock(m_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                          [this] { return !q_.empty() || stop_; })) {
            return false;
        }
        if (q_.empty()) {
            return false;  // stop_ 置位且队列空
        }
        out = std::move(q_.front());
        q_.erase(q_.begin());
        return true;
    }
    // 非阻塞尝试；返回 false 表示空。
    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(m_);
        if (q_.empty()) {
            return false;
        }
        out = std::move(q_.front());
        q_.erase(q_.begin());
        return true;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_);
        return q_.size();
    }
    bool empty() const { return size() == 0; }
    void clear() {
        std::lock_guard<std::mutex> lock(m_);
        q_.clear();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
    }
    void reset() {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = false;
        q_.clear();
    }

private:
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    std::vector<T> q_;
    bool stop_ = false;
};

}  // namespace test
}  // namespace proto
}  // namespace espview
