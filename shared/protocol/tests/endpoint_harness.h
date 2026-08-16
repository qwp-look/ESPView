// ESPView M8-A2 — CoordinatedHarness：确定性并发测试编排器（纯 C++17）。
//
// 两个真实 ProtocolEndpoint（A=PC、B=ESP32）+ 内存字节队列（sink→对端 RX）+
// 受控时钟（原子；确定性模式 conductor 单写，自动模式 ticker 线程单写）+
// blockSink 原子开关 + 角色线程（RX feeder / ticker / app sender / stats reader）。
// 主线程（测试函数）为 conductor：负责 connect/disconnect/reconnect 与断言。
//
// 确定性 interleaving 机制：EndpointConfig::testHooks（test-only，锁外信号）
//   把角色线程钉在精确位置（onFeedEnter 等），conductor 用 Gate 放行。
// 本文件仅测试代码使用；生产代码不包含。
//
// 用法：
//   CoordinatedHarness h;
//   h.cfgA.testHooks.onFeedEnter = [&]{ h.feedGateA.wait(); };  // 可选项
//   h.init();
//   h.connectBoth(); h.pumpBoth();         // 确定性握手
//   ... conductor 驱动 / 启动角色线程 ...

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "encoder.h"
#include "message.h"
#include "protocol_endpoint.h"
#include "test_sync.h"
#include "test_util.h"

namespace espview {
namespace proto {
namespace test {

// 受控时钟：原子单调毫秒。写者约束：确定性模式 = conductor；自动模式 = ticker。
struct CoordinatedClock {
    std::atomic<uint64_t> now{0};
    uint64_t operator()() { return now.load(std::memory_order_relaxed); }
    void set(uint64_t t) { now.store(t, std::memory_order_relaxed); }
    void advance(uint64_t delta) { now.fetch_add(delta, std::memory_order_relaxed); }
};

class CoordinatedHarness {
public:
    using ByteQueue = LockedQueue<std::vector<uint8_t>>;

    // 配置与回调：init() 前设置（含 testHooks）。
    EndpointConfig cfgA;
    EndpointConfig cfgB;
    ProtocolEndpoint::Callbacks cbA;
    ProtocolEndpoint::Callbacks cbB;
    CoordinatedClock clock;

    // 预置闸门（test 把 testHooks 接到这些门上；不用则保持关闭/忽略）。
    Gate feedGateA;          // A.onTransportData 取锁前
    Gate feedGateB;
    Gate connectGateA;       // A.onTransportConnected 取锁前
    Gate disconnectGateA;    // A 断开清理完成后
    Gate tickGateA;          // A.tick 状态快照后
    Gate failGateA;          // A.tick 将 failSession 前

    // sink 阻塞开关（原子）：置 true 后该端 sink 返回 kBackpressure 且不投递。
    std::atomic<bool> blockSinkA{false};
    std::atomic<bool> blockSinkB{false};
    // sink "仅失败一次" 开关（原子）：下一次 sink 调用返回 kBackpressure 并自动
    // 解除（M8-A2 HIGH-1 确定性测试：让 drain 恰好失败一次、其余发送成功）。
    std::atomic<bool> failNextSinkA{false};
    std::atomic<bool> failNextSinkB{false};
    // sink 保持闸门：关闭时 sink 阻塞等待（模拟"发送在途"；stopAll 会放行）。
    Gate sinkHoldA;
    Gate sinkHoldB;
    // sink 进入计数（test 判断"已进入发送段"；原子）。
    std::atomic<int> sinkCallsA{0};
    std::atomic<int> sinkCallsB{0};

    // 方向队列：A 的 sink → a2b_（B 的 RX）；B 的 sink → b2a_（A 的 RX）。
    ByteQueue a2b_;
    ByteQueue b2a_;

    CoordinatedHarness() = default;
    ~CoordinatedHarness() { stopAll(); }

    void init() {
        // 包装回调：内置 onAck/onAckTimeout 计数（test 自己的回调照常调用）。
        const auto userAckA = cbA.onAck;
        cbA.onAck = [this, userAckA](uint16_t s, uint8_t st, espview::proto::ErrorCode c) {
            onAckA_.fetch_add(1, std::memory_order_relaxed);
            if (userAckA) {
                userAckA(s, st, c);
            }
        };
        const auto userAckTimeoutA = cbA.onAckTimeout;
        cbA.onAckTimeout = [this, userAckTimeoutA](uint16_t s) {
            ackTimeoutA_.fetch_add(1, std::memory_order_relaxed);
            if (userAckTimeoutA) {
                userAckTimeoutA(s);
            }
        };
        const auto userAckB = cbB.onAck;
        cbB.onAck = [this, userAckB](uint16_t s, uint8_t st, espview::proto::ErrorCode c) {
            onAckB_.fetch_add(1, std::memory_order_relaxed);
            if (userAckB) {
                userAckB(s, st, c);
            }
        };
        const auto userAckTimeoutB = cbB.onAckTimeout;
        cbB.onAckTimeout = [this, userAckTimeoutB](uint16_t s) {
            ackTimeoutB_.fetch_add(1, std::memory_order_relaxed);
            if (userAckTimeoutB) {
                userAckTimeoutB(s);
            }
        };
        a_ = std::make_unique<ProtocolEndpoint>(
            cfgA, makeSink(blockSinkA, a2b_, sinkHoldA, sinkCallsA, failNextSinkA), cbA,
            [this]() { return clock(); });
        b_ = std::make_unique<ProtocolEndpoint>(
            cfgB, makeSink(blockSinkB, b2a_, sinkHoldB, sinkCallsB, failNextSinkB), cbB,
            [this]() { return clock(); });
    }

    // A 侧计数访问器（conductor 读取；原子）。
    int onAckCount() const { return onAckA_.load(std::memory_order_relaxed); }
    int ackTimeoutCount() const { return ackTimeoutA_.load(std::memory_order_relaxed); }
    int onAckCountB() const { return onAckB_.load(std::memory_order_relaxed); }
    int ackTimeoutCountB() const { return ackTimeoutB_.load(std::memory_order_relaxed); }

    ProtocolEndpoint& a() { return *a_; }
    ProtocolEndpoint& b() { return *b_; }

    // ---- 确定性驱动（conductor 单线程；不启动任何角色线程时使用）----
    void connectBoth() {
        a_->onTransportConnected();
        b_->onTransportConnected();
    }
    void pumpA() { drain(b2a_, *a_); }
    void pumpB() { drain(a2b_, *b_); }
    void pumpBoth() {
        pumpA();
        pumpB();
    }
    void tickA() { a_->tick(); }
    void tickB() { b_->tick(); }
    void tickBoth() {
        tickA();
        tickB();
    }

    // 被动握手（确定性）：双方 connect → 互相交换 HELLO → 双方 CONNECTED。
    bool connectAndHandshake() {
        connectBoth();
        pumpBoth();
        return a_->state() == SessionState::kConnected &&
               b_->state() == SessionState::kConnected;
    }

    // ---- 角色线程（自动模式；stop_ 置位后退出）----
    void startFeederA() {
        threads_.emplace_back([this] { feedLoop(b2a_, *a_); });
    }
    void startFeederB() {
        threads_.emplace_back([this] { feedLoop(a2b_, *b_); });
    }
    // ticker：唯一写时钟的线程；每 stepMs 推进并 tick 双方。
    void startTicker(uint64_t stepMs) {
        threads_.emplace_back([this, stepMs] {
            while (!stop_.load(std::memory_order_acquire)) {
                clock.advance(stepMs);
                a_->tick();
                b_->tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    // app sender：循环发送 factory() 产生的消息（count 次）。
    void startSender(bool sideA, const std::function<Message()>& factory, int count,
                     const Gate& startGate) {
        threads_.emplace_back([this, sideA, factory, count, &startGate] {
            startGate.wait();
            auto* ep = sideA ? a_.get() : b_.get();
            for (int i = 0; i < count; ++i) {
                ep->sendMessage(factory());
            }
        });
    }
    // stats reader：循环快照直到 stop。
    void startStatsReader() {
        threads_.emplace_back([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                (void)a_->stats();
                (void)b_->stats();
                std::this_thread::yield();
            }
        });
    }

    void stopAll() {
        if (stopping_) {
            return;
        }
        stopping_ = true;
        stop_.store(true, std::memory_order_release);
        a2b_.stop();
        b2a_.stop();
        feedGateA.open();
        feedGateB.open();
        connectGateA.open();
        disconnectGateA.open();
        tickGateA.open();
        failGateA.open();
        sinkHoldA.open();
        sinkHoldB.open();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        threads_.clear();
    }

private:
    static std::function<SendStatus(const uint8_t*, size_t)> makeSink(
        const std::atomic<bool>& block, ByteQueue& out, Gate& hold,
        std::atomic<int>& calls, std::atomic<bool>& failNext) {
        // 返回按引用捕获的 lambda（harness 生命周期长于 endpoints）。
        return [&block, &out, &hold, &calls, &failNext](const uint8_t* d, size_t n) -> SendStatus {
            calls.fetch_add(1, std::memory_order_acq_rel);
            // M8-A2 HIGH-1：failNext 一次性失败（exchange 原子取走标志，只命中一次）。
            if (failNext.exchange(false, std::memory_order_acq_rel)) {
                return SendStatus::kBackpressure;
            }
            if (block.load(std::memory_order_acquire)) {
                return SendStatus::kBackpressure;
            }
            if (!hold.isOpen()) {
                // 发送在途：阻塞到闸门放行（test-only；看门狗超时则放弃）。
                if (!hold.wait(30000)) {
                    return SendStatus::kError;
                }
            }
            out.push(std::vector<uint8_t>(d, d + n));
            return SendStatus::kOk;
        };
    }
    static void drain(ByteQueue& q, ProtocolEndpoint& ep) {
        std::vector<uint8_t> chunk;
        while (q.tryPop(chunk)) {
            ep.onTransportData(chunk.data(), chunk.size());
        }
    }
    // M8-A2 HIGH-2：feeder 只在 stop 信号下退出——队列空闲绝不退出。旧实现
    // pop(1000ms) 超时即永久退出：负载/仪器化下生产者被饿死 >1s 时消费者早退
    // → C8/L1/S1/S3 精确计数 flake（本机 ~40% 失败率）。pop 改短超时（200ms）
    // 周期性唤醒检查 stop 标志；数据到达时 notify_one 立即返回，无额外延迟。
    void feedLoop(ByteQueue& q, ProtocolEndpoint& ep) {
        std::vector<uint8_t> chunk;
        while (!stop_.load(std::memory_order_acquire)) {
            if (q.pop(chunk, 200)) {
                ep.onTransportData(chunk.data(), chunk.size());
            }
        }
    }

    std::unique_ptr<ProtocolEndpoint> a_;
    std::unique_ptr<ProtocolEndpoint> b_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::atomic<int> onAckA_{0};
    std::atomic<int> ackTimeoutA_{0};
    std::atomic<int> onAckB_{0};
    std::atomic<int> ackTimeoutB_{0};
    bool stopping_ = false;
};

}  // namespace test
}  // namespace proto
}  // namespace espview
