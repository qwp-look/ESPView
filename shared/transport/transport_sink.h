// ESPView M6-C — TransportSink（shared/transport，纯 C++17）。
//
// 规范来源：M6-C 任务书 §八（UART pacing）、§九/§十（TCP pacing + bounded queue）、
//   §十三（Transport 报告 txPolicy，上层只消费抽象结果）、§十四（control fairness）。
//
// 职责：把"上层消息 → 当前 Transport"的发送路径统一收口，按 Transport 报告的
//   capabilities（→ TxPolicy）决定 pacing 行为：
//   - paced（UART）：send() 背压时按 retryIntervalMs 重试至 maxWaitMs（= 按 wire
//     速率节流）；trySend() 单次尽力（PONG/ACK/PING 等控制回复不阻塞 RX 线程）。
//   - unpaced（TCP）：send() 单次调用（Transport 内部 sendAll 已阻塞到 socket
//     超时 = transport backpressure）；trySend() 单次尽力。
// 所有发送经 TransportManager 的发送门串行化（lockTransport / tryLockTransport），
// 运行时切换（switchTo）与发送互斥，杜绝 use-after-free。
// 错误路径不使用异常。

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

#include "transport.h"
#include "transport_manager.h"

namespace espview {
namespace transport {

class TransportSink {
public:
    // 会话存活检查：DISCONNECTED 时放弃发送（与 M1-3B paced sink 语义一致）。
    using AliveCheck = std::function<bool()>;
    // 单调毫秒时钟（统计/超时）。
    using Clock = std::function<uint64_t()>;
    // 背压重试间隔 sleep（UART pacing；host 测试注入计数/短眠）。
    using Sleep = std::function<void(uint32_t ms)>;

    TransportSink(TransportManager& mgr, AliveCheck alive, Clock now, Sleep sleep)
        : mgr_(mgr), alive_(std::move(alive)), now_(std::move(now)), sleep_(std::move(sleep)) {}

    // 阻塞式发送（按当前 Transport 的 TxPolicy）。整条消息 1..N 包由上层
    // （ProtocolEndpoint transmit）在 sendMutex_ 下串行调用。
    SendStatus send(const uint8_t* data, size_t len);
    // 单次尽力发送（控制回复 PONG/ACK、心跳 PING、ACK 重试专用）：
    // 门忙/缓冲满立即返回 kBackpressure，绝不长时间阻塞。
    SendStatus trySend(const uint8_t* data, size_t len);

private:
    TransportManager& mgr_;
    AliveCheck alive_;
    Clock now_;
    Sleep sleep_;
};

}  // namespace transport
}  // namespace espview
