// ESPView M6-C — TransportSink 实现（见 transport_sink.h）。

#include "transport_sink.h"

namespace espview {
namespace transport {

SendStatus TransportSink::send(const uint8_t* data, size_t len) {
    if (!alive_()) {
        return SendStatus::kNotConnected;  // 会话已死：放弃本次发送
    }
    ITransport* t = mgr_.lockTransport();
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
            if (sleep_ != nullptr) {
                sleep_(pol.retryIntervalMs);
            }
        }
    } else {
        // unpaced（TCP）：单次调用；Transport 内部 sendAll 已按 socket
        // send buffer / SO_SNDTIMEO 背压，不做 UART 式 sleep。
        r = t->send(data, len);
    }
    mgr_.unlockTransport();
    return r;
}

SendStatus TransportSink::trySend(const uint8_t* data, size_t len) {
    if (!alive_()) {
        return SendStatus::kNotConnected;  // 会话已死：放弃本次回复
    }
    if (!mgr_.isOpen()) {
        return SendStatus::kNotConnected;
    }
    ITransport* t = mgr_.tryLockTransport();
    if (t == nullptr) {
        return SendStatus::kBackpressure;  // 门忙（切换中/大帧发送中）：放弃本次回复
    }
    const SendStatus r = t->send(data, len);
    mgr_.unlockTransport();
    return r;
}

}  // namespace transport
}  // namespace espview
