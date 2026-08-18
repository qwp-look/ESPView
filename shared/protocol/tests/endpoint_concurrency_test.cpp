// ESPView — ProtocolEndpoint 并发发送测试（M1-3B 前置修复的回归保护）。
//
// 背景：ESP32 上多个任务（心跳任务 + App 帧发送任务 / RX 任务回复）会并发调用
//   sendMessage。若不串行化，两个多包消息的 Packet 会在包粒度交叉，对端 Decoder
//   会按 CHUNKED 违规作废消息（DESIGN.md：控制消息不得插入 CHUNKED 消息内部）。
// 修复：transmit() 以整条消息为粒度持锁（sendMutex_），tryTransmit() 供 RX 任务
//   尽力而为地发送 PONG/ACK（锁忙即放弃，不阻塞 RX 线程）。
//
// 本测试验证：多线程并发 sendMessage 时，每条消息的 Packet 在 sink 日志中保持
//   连续（不交叉），且解码拼接后能恢复原 payload。
// 纯 C++17，零平台依赖。

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#include "encoder.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::EndpointConfig;
using espview::proto::makeHello;
using espview::proto::makeMessage;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::ProtocolEndpoint;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionState;

constexpr size_t kPayloadSize = 10000;  // 3 packets（4096+4096+1808）
constexpr int kMsgsPerThread = 10;

// 线程安全 sink：记录所有发出的包。
struct SinkLog {
    std::mutex m;
    std::vector<std::vector<uint8_t>> packets;
    void add(const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> lock(m);
        packets.emplace_back(d, d + n);
    }
};

uint16_t seqOf(const std::vector<uint8_t>& p) {
    PacketHeader h;
    CHECK_EQ(espview::proto::decodeHeader(p.data(), p.size(), &h),
                                   PacketError::kNone);
    return h.seq;
}

// 构造一条多包消息：payload 前 4 字节 = 消息 id（LE），其余按确定性 pattern 填充。
Message makeTaggedMessage(uint32_t msgId) {
    std::vector<uint8_t> payload(kPayloadSize);
    payload[0] = static_cast<uint8_t>(msgId & 0xFFu);
    payload[1] = static_cast<uint8_t>((msgId >> 8) & 0xFFu);
    payload[2] = static_cast<uint8_t>((msgId >> 16) & 0xFFu);
    payload[3] = static_cast<uint8_t>((msgId >> 24) & 0xFFu);
    for (size_t i = 4; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i * 13u + msgId);
    }
    return makeMessage(static_cast<uint8_t>(espview::proto::MessageType::kInputMouse), 0,
                       std::move(payload));
}

}  // namespace

void concurrent_send_serializes_messages() {
    SinkLog log;
    ProtocolEndpoint::Callbacks cb;
    std::vector<SessionState> states;
    cb.onSessionState = [&states](SessionState s) { states.push_back(s); };

    auto sink = [&log](const uint8_t* d, size_t n) {
        log.add(d, n);
        return SendStatus::kOk;
    };

    ProtocolEndpoint ep(EndpointConfig{}, sink, cb);

    // 经被动握手进入 CONNECTED（无需真实对端）。
    ep.onTransportConnected();
    const auto hello = makeHello(1, 0, 320, 240, espview::proto::PixelFormat::kRgb565, 0b111,
                                 "concurrency-test");
    CHECK(hello.has_value());
    SequenceCounter localSeq;
    MessageEncoder enc(localSeq);
    std::vector<std::vector<uint8_t>> helloPkts;
    CHECK_EQ(enc.encode(*hello, helloPkts), PacketError::kNone);
    for (const auto& p : helloPkts) {
        ep.onTransportData(p.data(), p.size());
    }
    CHECK_EQ(ep.state(), SessionState::kConnected);
    CHECK_EQ(log.packets.size(), 1u);  // 只有本端 HELLO

    // 两个线程各发 kMsgsPerThread 条多包消息。
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&, t]() {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kMsgsPerThread; ++i) {
                const uint32_t msgId = static_cast<uint32_t>(t * 1000 + i);
                const SendResult r = ep.sendMessage(makeTaggedMessage(msgId));
                CHECK_EQ(r, SendResult::kOk);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : threads) {
        th.join();
    }

    // 校验：sink 日志中每条消息的包连续且不交叉。
    // 注意：日志第 0 个包是本端 HELLO（握手时发出），测试消息从 i=1 开始。
    const size_t expectedTotal = static_cast<size_t>(2 * kMsgsPerThread);
    CHECK_EQ(log.packets.size(), 1u + expectedTotal * 3u);

    size_t msgCount = 0;
    size_t i = 1;  // 跳过 HELLO
    std::vector<uint8_t> groupPayload;
    uint32_t lastMsgId = 0;
    bool any = false;
    while (i < log.packets.size()) {
        // 收集一个消息组的全部包（CHUNKED=0 收尾）。
        groupPayload.clear();
        size_t groupPackets = 0;
        uint16_t prevSeq = 0;
        bool first = true;
        while (i < log.packets.size()) {
            const auto& p = log.packets[i];
            const uint16_t s = seqOf(p);
            if (!first) {
                CHECK_EQ(s, static_cast<uint16_t>(prevSeq + 1));  // 组内 seq 连续
            }
            prevSeq = s;
            first = false;

            PacketHeader h;
            CHECK_EQ(espview::proto::decodeHeader(p.data(), p.size(), &h), PacketError::kNone);
            groupPayload.insert(groupPayload.end(), p.begin() + 20, p.end());
            ++groupPackets;
            const bool lastChunk = (h.flags & espview::proto::kFlagChunked) == 0;
            ++i;
            if (lastChunk) {
                break;
            }
        }
        CHECK_EQ(groupPackets, 3u);
        CHECK_EQ(groupPayload.size(), kPayloadSize);

        // 校验 payload 可恢复（decode 拼接 == 原消息）。
        const uint32_t msgId = static_cast<uint32_t>(groupPayload[0]) |
                               (static_cast<uint32_t>(groupPayload[1]) << 8) |
                               (static_cast<uint32_t>(groupPayload[2]) << 16) |
                               (static_cast<uint32_t>(groupPayload[3]) << 24);
        for (size_t k = 4; k < groupPayload.size(); ++k) {
            const uint8_t expected = static_cast<uint8_t>(k * 13u + msgId);
            CHECK_EQ(groupPayload[k], expected);
        }
        if (any) {
            CHECK_MSG(msgId != lastMsgId, "two distinct messages must not share a group");
        }
        lastMsgId = msgId;
        any = true;
        ++msgCount;
    }
    CHECK_EQ(msgCount, expectedTotal);
    // 组内 seq 连续性 + 全局 seq 单调隐含"不交叉"：每条消息独占一段连续 seq。
    CHECK(!states.empty());
}

// encodeStream 与 encode() 逐位一致（长消息低内存路径的回归保护）。
void encode_stream_matches_encode() {
    SequenceCounter seqA, seqB;
    MessageEncoder encA(seqA), encB(seqB);

    const std::vector<size_t> sizes = {0, 1, 4095, 4096, 4097, 10000, 153608};
    for (const size_t n : sizes) {
        std::vector<uint8_t> payload(n);
        for (size_t i = 0; i < n; ++i) {
            payload[i] = static_cast<uint8_t>(i * 31u + n);
        }
        const Message msg = makeMessage(
            static_cast<uint8_t>(espview::proto::MessageType::kFrameRect), 0, payload);

        std::vector<std::vector<uint8_t>> batch;
        CHECK_EQ(encA.encode(msg, batch), PacketError::kNone);

        std::vector<std::vector<uint8_t>> streamed;
        const PacketError err = encB.encodeStream(msg, [&streamed](const uint8_t* d, size_t len) {
            streamed.emplace_back(d, d + len);
            return true;
        });
        CHECK_EQ(err, PacketError::kNone);
        CHECK_EQ(batch.size(), streamed.size());
        for (size_t k = 0; k < batch.size() && k < streamed.size(); ++k) {
            CHECK_EQ(batch[k].size(), streamed[k].size());
            CHECK(batch[k] == streamed[k]);
        }
    }

    // sink 提前终止：返回 kSinkAborted，且 seq 已消耗（与 encode 失败语义一致）。
    {
        std::vector<uint8_t> payload(10000, 0x42);
        const Message msg = makeMessage(static_cast<uint8_t>(espview::proto::MessageType::kFrameRect),
                                        0, payload);
        int calls = 0;
        const PacketError err = encB.encodeStream(msg, [&calls](const uint8_t*, size_t) {
            ++calls;
            return calls < 2;  // 第二个包中止
        });
        CHECK_EQ(err, PacketError::kSinkAborted);
        CHECK_EQ(calls, 2);
    }
}

// M1-3C 回归：长流式消息占用 sendMutex_ 期间，会话 tick() 不得阻塞。
// 背景：经典 ESP32 单条 153608B FRAME_RECT 流式发送耗时 ~13.3s（115200），
//   全程持有 sendMutex_；若 tick() 内 PING 用阻塞式 sendMessage，心跳任务
//   会在第一次 PING 时卡死，peer 超时检测被饿死，断线永远无法被发现
//   （M1-3B reconnect 回归在 M1-3C 大 RECT 下失败）。
// 修复：tick() 内 PING/ACK 重试改用 tryTransmit（锁忙即跳过，不阻塞会话 tick）。
void heartbeat_does_not_block_during_streaming_transmit() {
    uint64_t nowMs = 0;
    std::atomic<bool> sinkBlock{false};  // 握手期间不阻塞；长发送开始前才启用
    std::atomic<int> sinkCalls{0};
    std::vector<SessionState> states;
    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&states](SessionState s) { states.push_back(s); };

    auto sink = [&sinkBlock, &sinkCalls](const uint8_t*, size_t) -> SendStatus {
        sinkCalls.fetch_add(1, std::memory_order_acq_rel);
        while (sinkBlock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return SendStatus::kOk;
    };
    ProtocolEndpoint ep(EndpointConfig{}, sink, cb, [&nowMs]() { return nowMs; });

    // 被动握手 → CONNECTED（无真实对端）。
    ep.onTransportConnected();
    const auto hello = makeHello(1, 0, 320, 240, espview::proto::PixelFormat::kRgb565, 0b111,
                                 "hb-test");
    CHECK(hello.has_value());
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> helloPkts;
    CHECK_EQ(enc.encode(*hello, helloPkts), PacketError::kNone);
    for (const auto& p : helloPkts) {
        ep.onTransportData(p.data(), p.size());
    }
    CHECK_EQ(ep.state(), SessionState::kConnected);

    // 长发送开始前启用 sink 阻塞（此后每个包都卡住 → sendMutex_ 被长期持有）。
    sinkBlock.store(true, std::memory_order_release);

    // 后台线程：长流式发送。
    struct BlockingSource : espview::proto::IMessagePayloadSource {
        std::vector<uint8_t> data;
        size_t off = 0;
        explicit BlockingSource(size_t n) : data(n, 0xAB) {}
        size_t read(uint8_t* dst, size_t maxBytes) override {
            const size_t n = std::min(maxBytes, data.size() - off);
            if (n > 0) {
                std::memcpy(dst, data.data() + off, n);
                off += n;
            }
            return n;
        }
    };
    std::atomic<bool> started{false};
    std::thread tx([&]() {
        started.store(true, std::memory_order_release);
        espview::proto::MessageHeader h;
        h.type = static_cast<uint8_t>(espview::proto::MessageType::kFrameRect);
        h.flags = 0;
        BlockingSource src(10000);
        ep.sendMessageStreaming(h, src);  // 阻塞在 sink，直到 sinkBlock 释放
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    while (sinkCalls.load(std::memory_order_acquire) == 0) {
        std::this_thread::yield();  // 确认 sendMutex_ 已持有
    }

    // Phase A：PING 到期但未超时；tick() 必须立即返回（PING 被跳过）。
    nowMs = 3000;
    auto futA = std::async(std::launch::async, [&ep]() { ep.tick(); });
    CHECK_MSG(futA.wait_for(std::chrono::milliseconds(1500)) == std::future_status::ready,
              "tick() blocked on TX mutex while streaming transmit holds it");
    CHECK_EQ(ep.state(), SessionState::kConnected);
    CHECK_EQ(ep.stats().txPing, 0u);  // PING 未发出（锁忙跳过）
    CHECK_EQ(ep.stats().pingTimeouts, 0u);

    // Phase B：peer 超时已过；tick() 必须仍能发现断线（不被流式发送饿死）。
    nowMs = 6000;
    auto futB = std::async(std::launch::async, [&ep]() { ep.tick(); });
    CHECK_MSG(futB.wait_for(std::chrono::milliseconds(1500)) == std::future_status::ready,
              "tick() blocked before peer-timeout detection");
    CHECK_EQ(ep.state(), SessionState::kDisconnected);
    CHECK_EQ(ep.stats().pingTimeouts, 1u);

    // 释放 sink，让后台发送线程退出（会话已断，其返回值不影响断言）。
    sinkBlock.store(false, std::memory_order_release);
    tx.join();
    CHECK_EQ(ep.stats().txPing, 0u);  // 全程无 PING 发出
}


// M8-B（B5）：长流式发送 + 对端心跳 —— 不得误判 peer dead（Problem D）。
// 语义：peer liveness = 最近收到对端有效消息（lastPeerRxMs_），与 TX 是否忙碌
//   无关。ESP32 长 FULL 帧流（115200 下 ~13.5s）持有 sendMutex_ 期间本端 PING
//   发不出，但只要 PC 的 PING 持续到达（RX 任务独立），会话必须保持 CONNECTED；
//   反之对端真正静默 5s，即使本端仍在流式发送也必须 failSession（不因 TX 忙碌
//   掩盖断线，M1-3B/M1-3C 回归）。
void long_stream_live_peer_no_peer_timeout() {
    uint64_t nowMs = 0;
    std::atomic<bool> sinkBlock{false};
    std::atomic<int> sinkCalls{0};
    std::vector<SessionState> states;
    std::vector<uint16_t> protoErrors;
    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&states](SessionState s) { states.push_back(s); };
    cb.onProtocolError = [&protoErrors](espview::proto::SessionError e, std::string_view) {
        protoErrors.push_back(static_cast<uint16_t>(e));
    };
    auto sink = [&sinkBlock, &sinkCalls](const uint8_t*, size_t) -> SendStatus {
        sinkCalls.fetch_add(1, std::memory_order_acq_rel);
        while (sinkBlock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return SendStatus::kOk;
    };
    ProtocolEndpoint ep(EndpointConfig{}, sink, cb, [&nowMs]() { return nowMs; });

    // 被动握手 → CONNECTED。
    ep.onTransportConnected();
    {
        SequenceCounter seq(0);
        MessageEncoder enc(seq);
        const auto hello = espview::proto::makeHello(
            1, 0, 320, 240, espview::proto::PixelFormat::kRgb565, 0b1111, "b5-live");
        CHECK(hello.has_value());
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(*hello, pkts), PacketError::kNone);
        for (const auto& p : pkts) {
            ep.onTransportData(p.data(), p.size());
        }
    }
    CHECK_EQ(ep.state(), SessionState::kConnected);

    // 预编码对端 PING（握手完成后 expectedSeq 基线 = 0，与 M0 基线语义一致）。
    const auto pingMsg = espview::proto::makePing(0x1234);
    SequenceCounter pingSeq(0);
    MessageEncoder pingEnc(pingSeq);
    std::vector<std::vector<uint8_t>> pingPkts;
    CHECK_EQ(pingEnc.encode(pingMsg, pingPkts), PacketError::kNone);
    CHECK_EQ(pingPkts.size(), 1u);
    const auto& pingBytes = pingPkts[0];

    // 长流式发送：sink 阻塞 → sendMutex_ 被长期持有（模拟 ~13.5s FULL 流）。
    sinkBlock.store(true, std::memory_order_release);
    struct BlockingSource : espview::proto::IMessagePayloadSource {
        std::vector<uint8_t> data;
        size_t off = 0;
        explicit BlockingSource(size_t n) : data(n, 0x5A) {}
        size_t read(uint8_t* dst, size_t maxBytes) override {
            const size_t n = std::min(maxBytes, data.size() - off);
            if (n > 0) {
                std::memcpy(dst, data.data() + off, n);
                off += n;
            }
            return n;
        }
    };
    std::atomic<bool> started{false};
    const int sinkBaseline = sinkCalls.load(std::memory_order_acquire);
    std::thread tx([&]() {
        started.store(true, std::memory_order_release);
        espview::proto::MessageHeader h;
        h.type = static_cast<uint8_t>(espview::proto::MessageType::kFrameRect);
        h.flags = 0;
        BlockingSource src(10000);
        ep.sendMessageStreaming(h, src);
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    while (sinkCalls.load(std::memory_order_acquire) == sinkBaseline) {
        std::this_thread::yield();
    }

    // 对端 PING 每 2s 到达（RX 任务不受 sendMutex_ 影响）→ 跨越 >5s 窗口。
    for (const uint64_t t : {2000u, 4000u, 6000u}) {
        nowMs = t;
        ep.onTransportData(pingBytes.data(), pingBytes.size());
        nowMs = t + 1000;
        ep.tick();  // 本端 PING 锁忙跳过；但不得 failSession
        CHECK_EQ(ep.state(), SessionState::kConnected);
        CHECK_EQ(ep.stats().pingTimeouts, 0u);
    }
    CHECK_EQ(protoErrors.size(), 0u);

    // 对端停止心跳：再 +5s → 即使本端仍在流式发送，也必须 failSession。
    nowMs = 11000;
    ep.tick();
    CHECK_EQ(ep.state(), SessionState::kDisconnected);
    CHECK_EQ(ep.stats().pingTimeouts, 1u);
    CHECK(protoErrors.size() == 1u &&
          protoErrors[0] ==
              static_cast<uint16_t>(espview::proto::SessionError::kPeerTimeout));

    // 释放 sink，让后台发送线程退出（会话已断，其返回值不影响断言）。
    sinkBlock.store(false, std::memory_order_release);
    tx.join();
}

void rx_tick_hello_connected_no_seq_gap();
void rx_tick_ping_auto_pong_and_rtt();
void rx_tick_set_mode_ack_exactly_once();
void rx_tick_disconnect_clears_pending_ack();
void rx_tick_frame_flow_no_gaps();
void rx_tick_passive_hello_try_transmit_backpressure();
void rx_tick_capabilities_backpressure_drained();

void runEndpointConcurrencyTests() {
    std::printf("  concurrent_send_serializes_messages\n");
    concurrent_send_serializes_messages();
    std::printf("  encode_stream_matches_encode\n");
    encode_stream_matches_encode();
    std::printf("  heartbeat_does_not_block_during_streaming_transmit\n");
    heartbeat_does_not_block_during_streaming_transmit();
    std::printf("  long_stream_live_peer_no_peer_timeout\n");
    long_stream_live_peer_no_peer_timeout();
    std::printf("  rx_tick_hello_connected_no_seq_gap\n");
    rx_tick_hello_connected_no_seq_gap();
    std::printf("  rx_tick_ping_auto_pong_and_rtt\n");
    rx_tick_ping_auto_pong_and_rtt();
    std::printf("  rx_tick_set_mode_ack_exactly_once\n");
    rx_tick_set_mode_ack_exactly_once();
    std::printf("  rx_tick_disconnect_clears_pending_ack\n");
    rx_tick_disconnect_clears_pending_ack();
    std::printf("  rx_tick_frame_flow_no_gaps\n");
    rx_tick_frame_flow_no_gaps();
    std::printf("  rx_tick_passive_hello_try_transmit_backpressure\n");
    rx_tick_passive_hello_try_transmit_backpressure();
    std::printf("  rx_tick_capabilities_backpressure_drained\n");
    rx_tick_capabilities_backpressure_drained();
}
// ---- M8-A1：确定性 RX + tick 编排（单线程，假时钟 + 注入屏障）----

struct RxFakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct RxHarness {
    RxFakeClock clock;
    std::vector<uint8_t> rx;
    std::vector<std::vector<uint8_t>> txPackets;
    std::vector<SessionState> states;
    std::vector<std::pair<uint8_t, uint16_t>> ackRequests;
    std::vector<uint16_t> ackTimeouts;
    std::vector<uint16_t> protoErrorCodes;
    std::vector<espview::proto::CommittedFrame> commits;
    std::unique_ptr<ProtocolEndpoint> ep;
    RxHarness* peer = nullptr;

    void init(RxHarness* peerSide) {
        peer = peerSide;
        ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](SessionState s) { states.push_back(s); };
        cb.onProtocolError = [this](espview::proto::SessionError e, std::string_view) {
            protoErrorCodes.push_back(static_cast<uint16_t>(e));
        };
        cb.onAckRequest = [this](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
            ackRequests.emplace_back(t, s);
        };
        cb.onAckTimeout = [this](uint16_t s) { ackTimeouts.push_back(s); };
        cb.onFrameBegin = [](const espview::proto::FrameBeginInfo&) {};
        cb.onFrameRect = [](const espview::proto::RectInfo&, const uint8_t*, size_t) {};
        cb.onFrameCommit = [this](const espview::proto::CommittedFrame& f) {
            commits.push_back(f);
        };
        cb.onFrameDiscard = [](espview::proto::FrameDiscardReason) {};
        auto sink = [this](const uint8_t* d, size_t n) {
            if (peer != nullptr) {
                peer->rx.insert(peer->rx.end(), d, d + n);
            }
            txPackets.emplace_back(d, d + n);
            return SendStatus::kOk;
        };
        ep = std::make_unique<ProtocolEndpoint>(EndpointConfig{}, sink, cb,
                                                [this]() { return clock.now; });
    }
    void pump() {
        std::vector<uint8_t> data = std::move(rx);
        rx.clear();
        if (!data.empty()) {
            ep->onTransportData(data.data(), data.size());
        }
    }
};

void rxConnectPair(RxHarness& a, RxHarness& b) {
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    a.pump();
    b.pump();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

// HELLO 握手 → CONNECTED，握手过程零 decoder 错误（无 kSequenceGap）。
void rx_tick_hello_connected_no_seq_gap() {
    RxHarness a, b;
    a.init(&b);
    b.init(&a);
    rxConnectPair(a, b);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(a.ep->stats().decoderErrors, 0u);
    CHECK_EQ(b.ep->stats().decoderErrors, 0u);
    // 握手后控制面立即可用：tick 到 PING 周期 → 一次往返。
    a.clock.now = 2000;
    a.ep->tick();
    b.pump();
    a.pump();
    CHECK_EQ(b.ep->stats().rxPing, 1u);
    CHECK_EQ(a.ep->stats().rxPong, 1u);
    CHECK_EQ(a.protoErrorCodes.size(), 0u);
    CHECK_EQ(b.protoErrorCodes.size(), 0u);
}

// tick 驱动 PING → 对端恰好一个自动 PONG → PONG 更新 RTT。
void rx_tick_ping_auto_pong_and_rtt() {
    RxHarness a, b;
    a.init(&b);
    b.init(&a);
    rxConnectPair(a, b);

    a.clock.now = 2000;
    a.ep->tick();
    CHECK_EQ(a.ep->stats().txPing, 1u);
    b.pump();  // b 处理 PING → 自动 PONG
    CHECK_EQ(b.ep->stats().rxPing, 1u);
    CHECK_EQ(b.ep->stats().txPong, 1u);  // 恰好一个自动 PONG
    a.clock.now = 2010;  // RTT 需要 now > lastPingSentAtMs_（严格递增）
    a.pump();  // a 处理 PONG → RTT
    CHECK_EQ(a.ep->stats().rxPong, 1u);
    CHECK_EQ(a.ep->stats().txPong, 0u);  // a 不是接收方，无 PONG 回复
    CHECK_EQ(a.ep->stats().rtt.samples, 1u);
    if (a.ep->stats().rtt.lastMs.has_value()) {
        CHECK_EQ(*a.ep->stats().rtt.lastMs, 10u);  // 2010 - 2000
    }
    // interval 未到 → 后续 tick 不重复 PING。
    a.clock.now = 2100;
    a.ep->tick();
    CHECK_EQ(a.ep->stats().txPing, 1u);
    CHECK_EQ(a.protoErrorCodes.size(), 0u);
    CHECK_EQ(b.protoErrorCodes.size(), 0u);
}

// SET_MODE（ACK_REQ）→ onAckRequest 恰好一次；acknowledge → 恰好一个 ACK；
// ACK 已收 → 后续 tick 不重试、无 onAckTimeout。
void rx_tick_set_mode_ack_exactly_once() {
    RxHarness a, b;
    a.init(&b);
    b.init(&a);
    rxConnectPair(a, b);

    CHECK_EQ(a.ep->sendMessage(
                 espview::proto::makeSetMode(espview::proto::DisplayMode::kWindow)),
             SendResult::kOk);
    b.pump();
    CHECK_EQ(b.ackRequests.size(), 1u);
    CHECK_EQ(b.ackRequests[0].first, 0x03u);
    CHECK_EQ(b.protoErrorCodes.size(), 0u);
    CHECK_EQ(b.ep->stats().rxMessages, 2u);  // HELLO + SET_MODE

    CHECK_EQ(b.ep->acknowledge(b.ackRequests[0].second, 0, espview::proto::ErrorCode::kNone),
             SendResult::kOk);
    a.pump();
    CHECK_EQ(a.ep->stats().ackReceived, 1u);
    CHECK_EQ(b.ep->stats().ackSent, 1u);

    a.clock.now = 600;  // 已过 500ms ACK deadline；ACK 已收 → 无重试
    a.ep->tick();
    CHECK_EQ(a.ackTimeouts.size(), 0u);
    CHECK_EQ(a.ep->stats().ackRetries, 0u);
    CHECK_EQ(a.ep->stats().ackFailures, 0u);
}

// 断线清空 pendingAck：断开后即使 ACK deadline 已过也不重试、无迟到 onAckTimeout。
void rx_tick_disconnect_clears_pending_ack() {
    RxHarness a, b;
    a.init(&b);
    b.init(&a);
    rxConnectPair(a, b);

    CHECK_EQ(a.ep->sendMessage(
                 espview::proto::makeSetMode(espview::proto::DisplayMode::kSplit)),
             SendResult::kOk);
    b.pump();
    CHECK_EQ(b.ackRequests.size(), 1u);

    a.ep->onTransportDisconnected();
    a.clock.now = 600;  // 已过 500ms ACK deadline
    a.ep->tick();
    CHECK_EQ(a.ackTimeouts.size(), 0u);
    CHECK_EQ(a.ep->stats().ackRetries, 0u);
    CHECK_EQ(a.ep->stats().ackFailures, 0u);
    CHECK_EQ(a.ep->state(), SessionState::kDisconnected);
}

// 帧消息流转：BEGIN/RECT/END 全通，无 decoder 错误（无 seq gap / chunk 违规）。
void rx_tick_frame_flow_no_gaps() {
    RxHarness a, b;
    a.init(&b);
    b.init(&a);
    rxConnectPair(a, b);

    const auto begin = espview::proto::makeFrameBegin(
        3, espview::proto::FrameType::kFull, espview::proto::PixelFormat::kRgb565, 10, 10, 200);
    CHECK(begin.has_value());
    CHECK_EQ(a.ep->sendMessage(*begin), SendResult::kOk);
    a.pump();
    b.pump();

    std::vector<uint8_t> pixels(200, 0x5A);
    const auto rect =
        espview::proto::makeFrameRect(0, 0, 10, 10, pixels.data(), pixels.size());
    CHECK(rect.has_value());
    CHECK_EQ(a.ep->sendMessage(*rect), SendResult::kOk);
    a.pump();
    b.pump();

    CHECK_EQ(a.ep->sendMessage(espview::proto::makeFrameEnd(3, 1, 200, false)),
             SendResult::kOk);
    a.pump();
    b.pump();

    CHECK_EQ(b.commits.size(), 1u);
    CHECK_EQ(b.ep->stats().decoderErrors, 0u);
    CHECK_EQ(b.ep->stats().rxMessages, 4u);  // HELLO + BEGIN + RECT + END
    if (!b.commits.empty()) {
        CHECK_EQ(b.commits[0].frameId, 3u);
        CHECK_EQ(b.commits[0].rectCount, 1u);
    }
}// 被动 HELLO 回复走 tryTransmit：长流式发送持有 sendMutex_ 时回复被放弃
// （kBackpressure）且 RX 线程不阻塞；会话保持 kConnecting，pendingHello_ 由
// 下一 tick 排空（单次发送、无重复），排空成功后完成握手。
void rx_tick_passive_hello_try_transmit_backpressure() {
    RxFakeClock clock;
    std::atomic<bool> sinkBlock{false};
    std::atomic<int> sinkCalls{0};
    std::vector<SessionState> states;
    std::unique_ptr<ProtocolEndpoint> ep;

    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&states](SessionState s) { states.push_back(s); };
    auto sink = [&](const uint8_t* d, size_t n) -> SendStatus {
        sinkCalls.fetch_add(1, std::memory_order_acq_rel);
        while (sinkBlock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        (void)d;
        (void)n;
        return SendStatus::kOk;
    };
    ep = std::make_unique<ProtocolEndpoint>(EndpointConfig{}, sink, cb,
                                            [&clock]() { return clock.now; });

    // 1) 先正常被动握手 → CONNECTED（HELLO 计数=1）。
    ep->onTransportConnected();
    {
        SequenceCounter seq(0);
        MessageEncoder enc(seq);
        const auto hello = espview::proto::makeHello(
            1, 0, 320, 240, espview::proto::PixelFormat::kRgb565, 0b111, "rx-bp");
        CHECK(hello.has_value());
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(*hello, pkts), PacketError::kNone);
        for (const auto& p : pkts) {
            ep->onTransportData(p.data(), p.size());
        }
    }
    CHECK_EQ(ep->state(), SessionState::kConnected);
    CHECK_EQ(ep->stats().txHello, 1u);

    // 2) 后台线程开始长流式发送；sink 阻塞 → sendMutex_ 被长期持有。
    sinkBlock.store(true, std::memory_order_release);
    struct BlockingSource : espview::proto::IMessagePayloadSource {
        std::vector<uint8_t> data;
        size_t off = 0;
        explicit BlockingSource(size_t n) : data(n, 0xAB) {}
        size_t read(uint8_t* dst, size_t maxBytes) override {
            const size_t n = std::min(maxBytes, data.size() - off);
            if (n > 0) {
                std::memcpy(dst, data.data() + off, n);
                off += n;
            }
            return n;
        }
    };
    std::atomic<bool> started{false};
    const int sinkBaseline = sinkCalls.load(std::memory_order_acquire);
    std::thread tx([&]() {
        started.store(true, std::memory_order_release);
        espview::proto::MessageHeader h;
        h.type = static_cast<uint8_t>(espview::proto::MessageType::kFrameRect);
        h.flags = 0;
        BlockingSource src(10000);
        ep->sendMessageStreaming(h, src);  // 阻塞在 sink，直到 sinkBlock 释放
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    // 确认后台线程已进入 sink（sinkCalls 超过握手 HELLO 的基线）→ sendMutex_ 已持有。
    while (sinkCalls.load(std::memory_order_acquire) == sinkBaseline) {
        std::this_thread::yield();
    }

    // 3) 断线 + 对端 HELLO → 被动恢复：tryTransmit 锁忙 → kBackpressure →
    //    pendingHello_ 暂存；保持 kConnecting（不完成握手）；RX 未阻塞。
    ep->onTransportDisconnected();
    {
        SequenceCounter seq(0);
        MessageEncoder enc(seq);
        const auto hello = espview::proto::makeHello(
            1, 0, 320, 240, espview::proto::PixelFormat::kRgb565, 0b111, "rx-bp");
        CHECK(hello.has_value());
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(*hello, pkts), PacketError::kNone);
        for (const auto& p : pkts) {
            ep->onTransportData(p.data(), p.size());
        }
    }
    CHECK_EQ(ep->state(), SessionState::kConnecting);  // 未完成握手（HELLO 未发出）
    CHECK_EQ(ep->stats().txHello, 1u);                 // 回复被放弃，无新 HELLO
    CHECK_EQ(ep->stats().decoderErrors, 0u);              // F1：被动恢复路径零 decoder 错误

    // 4) 释放 sink → 后台发送完成、sendMutex_ 释放；tick 排空 pendingHello_。
    sinkBlock.store(false, std::memory_order_release);
    tx.join();
    ep->tick();
    CHECK_EQ(ep->state(), SessionState::kConnected);  // 排空成功后完成握手
    CHECK_EQ(ep->stats().txHello, 2u);                // 恰好 2 次（首次 + 排空）
    CHECK_EQ(ep->stats().decoderErrors, 0u);              // tick 排空 completeHandshake 无 decoder 错误

    // 5) 后续 tick 不重复发送（单槽已清）。
    ep->tick();
    CHECK_EQ(ep->stats().txHello, 2u);
    CHECK_EQ(ep->stats().decoderErrors, 0u);              // 重复 tick 无副作用
    CHECK_EQ(ep->state(), SessionState::kConnected);
}

// sendCapabilities 走 tryTransmit：背压时返回 kBackpressure 并暂存单槽，
// 由 tick() 在 CONNECTED 后排空（单次发送、无重复）。
void rx_tick_capabilities_backpressure_drained() {
    RxFakeClock clock;
    std::atomic<bool> sinkBlock{false};
    std::atomic<int> sinkCalls{0};
    std::vector<SessionState> states;
    std::unique_ptr<ProtocolEndpoint> ep;

    ProtocolEndpoint::Callbacks cb;
    cb.onSessionState = [&states](SessionState s) { states.push_back(s); };
    auto sink = [&](const uint8_t* d, size_t n) -> SendStatus {
        sinkCalls.fetch_add(1, std::memory_order_acq_rel);
        while (sinkBlock.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        (void)d;
        (void)n;
        return SendStatus::kOk;
    };
    ep = std::make_unique<ProtocolEndpoint>(EndpointConfig{}, sink, cb,
                                            [&clock]() { return clock.now; });

    // 正常被动握手 → CONNECTED。
    ep->onTransportConnected();
    {
        SequenceCounter seq(0);
        MessageEncoder enc(seq);
        const auto hello = espview::proto::makeHello(
            1, 0, 320, 240, espview::proto::PixelFormat::kRgb565, 0b111, "rx-bp");
        CHECK(hello.has_value());
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(*hello, pkts), PacketError::kNone);
        for (const auto& p : pkts) {
            ep->onTransportData(p.data(), p.size());
        }
    }
    CHECK_EQ(ep->state(), SessionState::kConnected);

    // 后台长流式发送持有 sendMutex_。
    sinkBlock.store(true, std::memory_order_release);
    struct BlockingSource : espview::proto::IMessagePayloadSource {
        std::vector<uint8_t> data;
        size_t off = 0;
        explicit BlockingSource(size_t n) : data(n, 0xAB) {}
        size_t read(uint8_t* dst, size_t maxBytes) override {
            const size_t n = std::min(maxBytes, data.size() - off);
            if (n > 0) {
                std::memcpy(dst, data.data() + off, n);
                off += n;
            }
            return n;
        }
    };
    std::atomic<bool> started{false};
    const int sinkBaseline = sinkCalls.load(std::memory_order_acquire);
    std::thread tx([&]() {
        started.store(true, std::memory_order_release);
        espview::proto::MessageHeader h;
        h.type = static_cast<uint8_t>(espview::proto::MessageType::kFrameRect);
        h.flags = 0;
        BlockingSource src(10000);
        ep->sendMessageStreaming(h, src);
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    while (sinkCalls.load(std::memory_order_acquire) == sinkBaseline) {
        std::this_thread::yield();
    }

    // sendCapabilities → tryTransmit 锁忙 → kBackpressure + 单槽暂存。
    espview::proto::CapabilitiesInfo caps;
    caps.virtualPresent = true;
    caps.physicalPresent = true;
    caps.width = 320;
    caps.height = 240;
    caps.pixelFormat = espview::proto::PixelFormat::kRgb565;
    caps.colorDepth = 16;
    caps.virtualCanReadback = true;
    caps.modeMask = 0b1111;
    caps.physWidth = 128;
    caps.physHeight = 64;
    caps.physPixelFormat = espview::proto::PhysicalPixelFormat::kMono1;
    caps.physColorDepth = 1;
    caps.physMono = true;
    caps.physController = espview::proto::CapabilitiesController::kSsd1306;
    caps.physI2cAddress = 0x3C;
    caps.sceneSupport = 0b11;
    const SendResult r = ep->sendCapabilities(caps);
    CHECK_EQ(r, SendResult::kBackpressure);
    CHECK_EQ(ep->stats().txCapabilities, 0u);

    // 释放 → tick 排空（单次发送、无重复）。
    sinkBlock.store(false, std::memory_order_release);
    tx.join();
    ep->tick();
    CHECK_EQ(ep->state(), SessionState::kConnected);
    CHECK_EQ(ep->stats().txCapabilities, 1u);
    ep->tick();
    CHECK_EQ(ep->stats().txCapabilities, 1u);
}
