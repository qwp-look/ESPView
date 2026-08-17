// ESPView M8-A3 — BoundedQueue / BoundedByteBuffer（shared/transport，纯 C++17）。
//
// 用途：PC SerialWorker 的 4 类跨线程队列（§三十五.9 queue ownership）——
//   rxBuf_（BoundedByteBuffer，drop-newest）、inputQueue_（drop-newest）、
//   displayModeQueue_（latest-wins）、wifiQueue_（latest-wins + 密码副本清理）。
// 语义：
//   - 容量固定（构造时指定）；溢出按策略丢最旧（latest-wins）或丢新（drop-newest），
//     并累计丢弃计数（诊断，不回绕）。
//   - 非线程安全：调用方（SerialWorker）在自己的 mutex 下使用。
//   - 错误路径不使用异常。
//
// 禁止在协议/transport 热路径引入本头（那是 per-packet 路径）；仅用于跨线程
// 队列的容量治理（GUI→Worker / Transport RX→Worker）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace espview {
namespace transport {

// 有界 FIFO 队列。push 溢出策略：
//   - dropOldest=true  → latest-wins：丢弃队头，新元素入队（返回 true）；
//   - dropOldest=false → drop-newest：拒绝新元素（返回 false）。
// 溢出即累计 dropped()（新元素被拒或旧元素被丢都计 1）。
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

    // 入队（latest-wins / drop-newest）。onDrop 在丢最旧元素前调用（默认空操作）：
    // 调用方可在元素销毁前清理资源（如 Wi-Fi 密码副本，AF.4）。
    template <typename OnDrop>
    bool push(T&& v, bool dropOldest, OnDrop&& onDrop) {
        if (capacity_ == 0) {
            // M8-A3（F/K）：容量 0 = 永不入队（拒绝一切 push，避免空 deque
            // pop_front UB）；拒绝即计 1 次丢弃。
            ++dropped_;
            return false;
        }
        if (buf_.size() >= capacity_) {
            ++dropped_;
            if (!dropOldest) {
                return false;
            }
            onDrop(buf_.front());
            buf_.pop_front();
        }
        buf_.push_back(std::move(v));
        return true;
    }

    // 拷贝入队（T 须可拷贝；与右值版本同语义）。
    template <typename OnDrop>
    bool push(const T& v, bool dropOldest, OnDrop&& onDrop) {
        if (capacity_ == 0) {
            ++dropped_;
            return false;
        }
        if (buf_.size() >= capacity_) {
            ++dropped_;
            if (!dropOldest) {
                return false;
            }
            onDrop(buf_.front());
            buf_.pop_front();
        }
        buf_.push_back(v);
        return true;
    }

    bool push(T&& v, bool dropOldest) {
        return push(std::move(v), dropOldest, [](T&) {});
    }

    // 拷贝入队（T 须可拷贝；与右值版本同语义）。
    bool push(const T& v, bool dropOldest) {
        return push(v, dropOldest, [](T&) {});
    }

    // 弹出队头；空队列返回 false。
    bool pop(T& out) {
        if (buf_.empty()) {
            return false;
        }
        out = std::move(buf_.front());
        buf_.pop_front();
        return true;
    }

    // 清空（不计数；调用方负责清理元素内资源，如 Wi-Fi 密码副本）。
    void clear() { buf_.clear(); }

    // 取出全部元素（FIFO 序）；调用方负责元素内资源清理。
    std::vector<T> takeAll() {
        std::vector<T> out;
        out.reserve(buf_.size());
        while (!buf_.empty()) {
            out.push_back(std::move(buf_.front()));
            buf_.pop_front();
        }
        return out;
    }

    // 取出并清零丢弃计数（调用方须持锁）。
    uint64_t takeDropped() {
        const uint64_t d = dropped_;
        dropped_ = 0;
        return d;
    }

    size_t size() const { return buf_.size(); }
    bool empty() const { return buf_.empty(); }
    size_t capacity() const { return capacity_; }
    uint64_t dropped() const { return dropped_; }

private:
    size_t capacity_;
    std::deque<T> buf_;
    uint64_t dropped_ = 0;
};

// 有界字节缓冲（rxBuf_：Transport RX 线程 append，Worker 线程 takeAll）。
// 溢出策略固定 drop-newest：满时保留已缓冲的旧字节，丢弃本次 append 的
// 尾部新字节（超出容量部分），计数累加。
class BoundedByteBuffer {
public:
    explicit BoundedByteBuffer(size_t capacity) : capacity_(capacity) {}

    void append(const uint8_t* data, size_t len) {
        if (data == nullptr || len == 0 || capacity_ == 0) {
            if (len > 0 && capacity_ == 0) {
                dropped_ += len;
            }
            return;
        }
        if (buf_.size() + len > capacity_) {
            // 保留已缓冲的旧字节，丢弃本次新字节的溢出部分（drop-newest）。
            dropped_ += buf_.size() + len - capacity_;
            const size_t keep = capacity_ > buf_.size() ? capacity_ - buf_.size() : 0;
            if (keep > 0) {
                buf_.insert(buf_.end(), data, data + keep);
            }
            return;
        }
        buf_.insert(buf_.end(), data, data + len);
    }

    // 取出全部缓冲（移动语义）；调用后缓冲为空。
    std::vector<uint8_t> takeAll() {
        std::vector<uint8_t> out;
        out.swap(buf_);
        return out;
    }

    void clear() { buf_.clear(); }
    // 取出并清零丢弃计数（调用方须持锁）。
    uint64_t takeDropped() {
        const uint64_t d = dropped_;
        dropped_ = 0;
        return d;
    }
    size_t size() const { return buf_.size(); }
    bool empty() const { return buf_.empty(); }
    size_t capacity() const { return capacity_; }
    uint64_t dropped() const { return dropped_; }

private:
    size_t capacity_;
    std::vector<uint8_t> buf_;
    uint64_t dropped_ = 0;
};

}  // namespace transport
}  // namespace espview
