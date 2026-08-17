// ESPView M6-C — TransportSink 实现（见 transport_sink.h）。

#include "transport_sink.h"

namespace espview {
namespace transport {

SendStatus TransportSink::send(const uint8_t* data, size_t len) {
    if (!alive_()) {
        return SendStatus::kNotConnected;  // 会话已死：放弃本次发送
    }
    // M8-A5（TM-05）：RAII 发送门 —— 忘记 unlock 即全链路死锁，结构上禁止。
    TransportLockGuard guard = lockTransport(mgr_);
    ITransport* t = guard.get();
    if (t == nullptr) {
        return SendStatus::kError;  // Transport 未 open/切换失败
    }
    const TxPolicy pol = txPolicyFor(t->capabilities());
    SendStatus r = SendStatus::kError;
    if (pol.retryOnBackpressure) {
        // paced（UART）：背压 → 按 wire 速率重试至预算上限。
        const uint64_t deadline = now_() + pol.maxWaitMs;
        while (true) {
            r = t->send(data, len);
            if (r != SendStatus::kBackpressure) {
                break;
            }
            if (now_() >= deadline || !alive_()) {
                r = SendStatus::kBackpressure;  // 超时兜底：上层整帧丢弃
                break;
            }
            if (sleep_ != nullptr && !sleep_(pol.retryIntervalMs)) {
                r = SendStatus::kBackpressure;  // M8-A5（SINK-04）：睡眠被中断，立即放弃
                break;
            }
        }
    } else {
        // unpaced（TCP）：单次调用；Transport 内部 sendAll 已按 socket
        // send buffer / SO_SNDTIMEO 背压，不做 UART 式 sleep。
        r = t->send(data, len);
    }
    return r;  // guard 析构自动 unlock
}

SendStatus TransportSink::trySend(const uint8_t* data, size_t len) {
    if (!alive_()) {
        return SendStatus::kNotConnected;  // 会话已死：放弃本次回复
    }
    // M8-A5：真正关闭（非切换窗口）才报 NotConnected；切换窗口按背压处理，
    // 上层可安全重试而非误判会话终止（TM-02 配套语义）。
    if (!mgr_.isOpen() && !mgr_.isSwitching()) {
        return SendStatus::kNotConnected;
    }
    TransportLockGuard guard = tryLockTransport(mgr_);
    ITransport* t = guard.get();
    if (t == nullptr) {
        return SendStatus::kBackpressure;  // 门忙（切换中/大帧发送中）：放弃本次回复
    }
    const SendStatus r = t->send(data, len);
    return r;  // guard 析构自动 unlock
}

}  // namespace transport
}  // namespace espview
