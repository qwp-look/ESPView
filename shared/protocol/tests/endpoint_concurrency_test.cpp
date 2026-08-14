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

void runEndpointConcurrencyTests() {
    std::printf("  concurrent_send_serializes_messages\n");
    concurrent_send_serializes_messages();
    std::printf("  encode_stream_matches_encode\n");
    encode_stream_matches_encode();
    std::printf("  heartbeat_does_not_block_during_streaming_transmit\n");
    heartbeat_does_not_block_during_streaming_transmit();
}
