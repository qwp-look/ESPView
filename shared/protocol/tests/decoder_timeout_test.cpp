// ESPView — StreamDecoder 半包超时 + ProtocolEndpoint::tick() 接线 Host Tests（M8-A1 Task 3）
//
// 规范来源：docs/DESIGN.md E 节（半包：>500ms 未收齐 → 强制回 SYNC）+
// M8-A1 任务书：超时由 ProtocolEndpoint::tick() 驱动（上层每 100-200ms 调 tick），
// 调用 StreamDecoder::onTimeout()：丢弃滞留字节、回 SYNC、作废组装中的 Message、
// expectedSeq 保持不变；不清会话、不 failSession。
//
// 覆盖：
//   1. StreamDecoder 级矩阵：1..19B 半包头、半包载荷、garbage、新 MAGIC 恢复、
//      新完整包恰好派发一次、CHUNKED 中途超时清态、重复 onTimeout 是 no-op；
//   2. Endpoint 级（假时钟）：喂 10B 半包头 → +501ms → tick() 清 decoder →
//      补喂剩余字节不重join → 新完整包正常派发（expectedSeq 保留、无 seq gap）；
//      超时不 failSession（状态保持 CONNECTED、无 kPeerTimeout、无重复派发）。
// 纯 C++17，零平台依赖。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::CommittedFrame;
using espview::proto::DecoderError;
using espview::proto::EndpointConfig;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameType;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeFrameRect;
using espview::proto::makeMessage;
using espview::proto::makePing;
using espview::proto::PacketError;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::RectInfo;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionError;
using espview::proto::SessionState;
using espview::proto::StreamDecoder;

struct Collector {
    std::vector<Message> messages;
    std::vector<DecoderError> errors;
    void onMessage(const Message& m) { messages.push_back(m); }
    void onError(DecoderError e) { errors.push_back(e); }
};

StreamDecoder makeDecoder(Collector& c) {
    return StreamDecoder([&c](const Message& m) { c.onMessage(m); }, nullptr,
                         [&c](DecoderError e) { c.onError(e); });
}

// 用独立编码器生成单包字节（seq 从 given 起）。
std::vector<uint8_t> encodeOne(const Message& msg, SequenceCounter& seq) {
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> pkts;
    enc.encode(msg, pkts);
    std::vector<uint8_t> out;
    for (const auto& p : pkts) {
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// 找一枚 PING 时间戳：其完整包字节 [10..28) 内不含 'E'(0x45)——
// 半包超时后补喂剩余字节不会触发伪 MAGIC（保证 0 错误断言确定性）。
uint64_t findCleanPingTs() {
    for (uint64_t ts = 0; ts < 100000; ++ts) {
        SequenceCounter seq(0);
        const auto bytes = encodeOne(makePing(ts), seq);
        bool clean = true;
        for (size_t i = 10; i < bytes.size(); ++i) {
            if (bytes[i] == 0x45) {
                clean = false;
                break;
            }
        }
        if (clean) {
            return ts;
        }
    }
    return 0;
}

// ---- StreamDecoder 级矩阵 ----

// 1..19B 半包头 + onTimeout → 缓冲清空、无组装、expectedSeq 不变。
void partial_header_timeout_matrix() {
    const uint64_t ts = findCleanPingTs();
    SequenceCounter seq(0);
    const auto pingBytes = encodeOne(makePing(ts), seq);
    for (size_t cut = 1; cut < 20; ++cut) {
        Collector c;
        auto dec = makeDecoder(c);
        dec.feed(pingBytes.data(), cut);
        CHECK_EQ(dec.bufferedBytes(), cut);
        dec.onTimeout();
        CHECK_EQ(dec.bufferedBytes(), 0u);
        CHECK(!dec.assemblingMessage());
        CHECK_EQ(dec.expectedSeq(), 0u);  // expectedSeq 保持不变
        CHECK_EQ(c.errors.size(), 0u);
        CHECK_EQ(c.messages.size(), 0u);
    }
}

// 半包载荷（header + 10B）→ onTimeout → 补喂剩余字节：原消息不派发（不重join）、
// 0 错误；随后完整新包正常派发。
void partial_payload_no_rejoin() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    // 64 字节全 0 payload：剩余字节无 'E'，补喂不产生伪 MAGIC。
    const auto msg = makeMessage(static_cast<uint8_t>(MessageType::kInputMouse), 0,
                                 std::vector<uint8_t>(64, 0x00));
    const auto bytes = encodeOne(msg, seq);
    CHECK_EQ(bytes.size(), 20u + 64u);

    dec.feed(bytes.data(), 20 + 10);  // header + 10 payload
    CHECK_EQ(dec.bufferedBytes(), 30u);
    dec.onTimeout();
    CHECK_EQ(dec.bufferedBytes(), 0u);
    CHECK(!dec.assemblingMessage());

    // 补喂剩余 54 字节：不得重join 派发原消息。
    dec.feed(bytes.data() + 30, 64 - 10);
    CHECK_EQ(c.messages.size(), 0u);
    CHECK_EQ(c.errors.size(), 0u);

    // 完整新包（seq 0，expectedSeq 未变）→ 恰好派发一次。
    // 用新计数器从 0 起：首次 encodeOne 已把 seq 消耗到 1，此处必须与
    // expectedSeq(=0) 对齐，否则 seq gap 会把新包丢弃。
    SequenceCounter freshSeq(0);
    const auto ping = makePing(1);
    const auto pingBytes = encodeOne(ping, freshSeq);
    dec.feed(pingBytes.data(), pingBytes.size());
    CHECK_EQ(c.messages.size(), 1u);
    CHECK_EQ(c.messages[0].type, static_cast<uint8_t>(MessageType::kPing));
    CHECK_EQ(c.errors.size(), 0u);
}

// 垃圾字节 + 重复 onTimeout：无错误、无派发（no error loop）。
void garbage_no_error_loop() {
    Collector c;
    auto dec = makeDecoder(c);
    // 构造不含 'E' 的垃圾（pattern 0x41..0x44）。
    std::vector<uint8_t> garbage;
    for (int i = 0; i < 4096; ++i) {
        garbage.push_back(static_cast<uint8_t>(0x41 + (i % 4)));
    }
    for (int round = 0; round < 3; ++round) {
        dec.feed(garbage.data(), garbage.size());
        dec.onTimeout();
        CHECK_EQ(dec.bufferedBytes(), 0u);
        CHECK(!dec.assemblingMessage());
        CHECK_EQ(c.messages.size(), 0u);
        CHECK_EQ(c.errors.size(), 0u);
    }
    // 垃圾后新 MAGIC 正常恢复。
    SequenceCounter seq(0);
    const auto ping = makePing(2);
    const auto pingBytes = encodeOne(ping, seq);
    dec.feed(pingBytes.data(), pingBytes.size());
    CHECK_EQ(c.messages.size(), 1u);
    CHECK_EQ(c.errors.size(), 0u);
}

bool messageEqualPing(const Collector& c, const Message& ping) {
    return c.messages.size() == 1u && c.messages[0].payload == ping.payload &&
           c.messages[0].type == ping.type;
}

// 超时后新 MAGIC 恢复（半包头被丢弃后重新同步）。
void new_magic_recovers() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    const auto ping = makePing(3);
    const auto pingBytes = encodeOne(ping, seq);
    dec.feed(pingBytes.data(), 7);
    dec.onTimeout();
    dec.feed(pingBytes.data(), pingBytes.size());
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqualPing(c, ping));
    CHECK_EQ(c.errors.size(), 0u);
}

// 超时后完整 FULL 帧恰好派发一次（BEGIN/RECT/END 各一次，无重复）。
void full_frame_after_timeout_dispatches_once() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    const auto begin = *makeFrameBegin(1, FrameType::kFull, PixelFormat::kRgb565, 32, 32,
                                       2048);
    std::vector<uint8_t> pixels(2048, 0x7F);
    const auto rect = *makeFrameRect(0, 0, 32, 32, pixels.data(), pixels.size());
    const auto end = makeFrameEnd(1, 1, 2048, false);

    dec.feed(encodeOne(begin, seq).data(), 5);  // 半包头
    dec.onTimeout();
    CHECK_EQ(c.messages.size(), 0u);

    // 完整帧：BEGIN → RECT → END。用新计数器从 0 起（onTimeout 保留
    // expectedSeq=0；首次 encodeOne 已消耗 seq 0，直接用旧计数器会 seq gap）。
    SequenceCounter freshSeq(0);
    dec.feed(encodeOne(begin, freshSeq));
    dec.feed(encodeOne(rect, freshSeq));
    dec.feed(encodeOne(end, freshSeq));
    CHECK_EQ(c.messages.size(), 3u);
    CHECK_EQ(c.messages[0].type, static_cast<uint8_t>(MessageType::kFrameBegin));
    CHECK_EQ(c.messages[1].type, static_cast<uint8_t>(MessageType::kFrameRect));
    CHECK_EQ(c.messages[2].type, static_cast<uint8_t>(MessageType::kFrameEnd));
    CHECK_EQ(c.errors.size(), 0u);
}

// 中途 CHUNKED 组装 + 超时：清态；重复 onTimeout 是 no-op。
void chunked_timeout_clears_state_and_repeat_noop() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    std::vector<uint8_t> big(8192, 0x33);
    const auto msg =
        makeMessage(static_cast<uint8_t>(MessageType::kFrameRect), 0, std::move(big));
    const auto bytes = encodeOne(msg, seq);  // 2 packets（seq 0, 1）
    CHECK_EQ(bytes.size(), 2u * 20u + 8192u);

    dec.feed(bytes.data(), 20 + 4096 + 10);  // 包0 完整 + 包1 前 10B
    CHECK(dec.assemblingMessage());
    CHECK(dec.bufferedBytes() > 0u);
    dec.onTimeout();
    CHECK(!dec.assemblingMessage());
    CHECK_EQ(dec.bufferedBytes(), 0u);
    CHECK_EQ(dec.expectedSeq(), 1u);  // 包0 已消费，基线保留

    // 重复 onTimeout：no-op。
    dec.onTimeout();
    dec.onTimeout();
    CHECK(!dec.assemblingMessage());
    CHECK_EQ(dec.bufferedBytes(), 0u);

    // 新完整包（seq 1）恢复。
    SequenceCounter seq1(1);
    const auto pong = encodeOne(makePing(4), seq1);
    dec.feed(pong.data(), pong.size());
    CHECK_EQ(c.messages.size(), 1u);
    CHECK_EQ(c.errors.size(), 0u);
}// ---- Endpoint 级接线（假时钟 + tick() 驱动）----

struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct EpSide {
    FakeClock clock;
    std::vector<uint8_t> rx;
    std::vector<SessionState> states;
    std::vector<CommittedFrame> commits;
    std::vector<uint16_t> protoErrorCodes;
    std::unique_ptr<ProtocolEndpoint> ep;
    EpSide* peer = nullptr;

    void init(EpSide* peerSide) {
        peer = peerSide;
        ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](SessionState s) { states.push_back(s); };
        cb.onProtocolError = [this](SessionError e, std::string_view) {
            protoErrorCodes.push_back(static_cast<uint16_t>(e));
        };
        cb.onFrameBegin = [](const FrameBeginInfo&) {};
        cb.onFrameRect = [](const RectInfo&, const uint8_t*, size_t) {};
        cb.onFrameCommit = [this](const CommittedFrame& f) { commits.push_back(f); };
        cb.onFrameDiscard = [](FrameDiscardReason) {};
        auto sink = [this](const uint8_t* d, size_t n) {
            if (peer != nullptr) {
                peer->rx.insert(peer->rx.end(), d, d + n);
            }
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

// 双端握手 → 双方 CONNECTED（正常路径）。
void connectPair(EpSide& a, EpSide& b) {
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    a.pump();
    b.pump();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

// 完整 FULL 帧字节（seq 从给定 counter 起）：BEGIN(seq0) → RECT(seq1) → END(seq2)。
std::vector<uint8_t> encodeFrameBytes(SequenceCounter& seq) {
    const auto begin = *makeFrameBegin(1, FrameType::kFull, PixelFormat::kRgb565, 32, 32,
                                       2048);
    std::vector<uint8_t> pixels(2048, 0x7F);
    const auto rect = *makeFrameRect(0, 0, 32, 32, pixels.data(), pixels.size());
    const auto end = makeFrameEnd(1, 1, 2048, false);
    std::vector<uint8_t> out;
    const auto beginBytes = encodeOne(begin, seq);
    out.insert(out.end(), beginBytes.begin(), beginBytes.end());
    const auto rectBytes = encodeOne(rect, seq);
    out.insert(out.end(), rectBytes.begin(), rectBytes.end());
    const auto endBytes = encodeOne(end, seq);
    out.insert(out.end(), endBytes.begin(), endBytes.end());
    return out;
}

// 半包超时接线：tick() 驱动（10B 半包头）；501ms 后 flush；补喂剩余字节不重join；
// 新完整帧正常派发一次；会话保持 CONNECTED（无 kPeerTimeout）。
void endpoint_partial_header_timeout_flushes() {
    EpSide a, b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // 用无 0x45 尾巴的 PING（补喂剩余字节不产生伪 MAGIC，0 错误确定性）。
    const uint64_t ts = findCleanPingTs();
    SequenceCounter seq(0);
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> pkts;
    CHECK_EQ(enc.encode(makePing(ts), pkts), PacketError::kNone);
    const auto& pingBytes = pkts[0];
    CHECK_EQ(pingBytes.size(), 28u);  // 20B 头 + 8B 时间戳

    // 10B 半包头（t=0）：decoder 滞留，不派发（HELLO 已计入 rxMessages=1）。
    const uint64_t base = a.ep->stats().rxMessages;
    CHECK_EQ(base, 1u);
    a.ep->onTransportData(pingBytes.data(), 10);
    CHECK_EQ(a.ep->stats().rxMessages, base);

    // +501ms → tick() 触发 StreamDecoder::onTimeout（清滞留字节）。
    a.clock.now = 501;
    a.ep->tick();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);  // 不 failSession
    CHECK_EQ(a.protoErrorCodes.size(), 0u);             // 无 kPeerTimeout
    CHECK_EQ(a.ep->stats().rxMessages, base);

    // 补喂剩余 18B：半包已作废，不重join 派发（0 错误）。
    a.ep->onTransportData(pingBytes.data() + 10, pingBytes.size() - 10);
    CHECK_EQ(a.ep->stats().rxMessages, base);
    CHECK_EQ(a.ep->stats().decoderErrors, 0u);

    // 新完整帧（seq 从 0 起；expectedSeq 未变）→ 恰好派发 3 条 + 提交 1 次。
    SequenceCounter fresh(0);
    const auto frameBytes = encodeFrameBytes(fresh);
    a.ep->onTransportData(frameBytes.data(), frameBytes.size());
    CHECK_EQ(a.ep->stats().rxMessages, base + 3u);
    CHECK_EQ(a.commits.size(), 1u);
    if (!a.commits.empty()) {
        CHECK_EQ(a.commits[0].rectCount, 1u);
        CHECK_EQ(a.commits[0].byteCount, 2048u);
    }
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
}// 半包载荷（header+10B）超时：作废组装中的 Message；补喂剩余字节不重join。
void endpoint_partial_payload_timeout_no_rejoin() {
    EpSide a, b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // 64B 全 0 payload 的 INPUT_MOUSE：剩余字节无 'E'，补喂不产生伪 MAGIC。
    SequenceCounter seq(0);
    MessageEncoder enc(seq);
    const auto msg = makeMessage(static_cast<uint8_t>(MessageType::kInputMouse), 0,
                                 std::vector<uint8_t>(64, 0x00));
    std::vector<std::vector<uint8_t>> pkts;
    CHECK_EQ(enc.encode(msg, pkts), PacketError::kNone);
    const auto& bytes = pkts[0];
    CHECK_EQ(bytes.size(), 84u);  // 20B 头 + 64B payload

    // header + 10B payload（t=0）：decoder 滞留，不派发（HELLO 已计入 rxMessages=1）。
    const uint64_t base = a.ep->stats().rxMessages;
    CHECK_EQ(base, 1u);
    a.ep->onTransportData(bytes.data(), 30);
    CHECK_EQ(a.ep->stats().rxMessages, base);

    // +501ms → tick() 触发半包超时（作废组装中消息）。
    a.clock.now = 501;
    a.ep->tick();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);  // 不 failSession
    CHECK_EQ(a.protoErrorCodes.size(), 0u);             // 无 kPeerTimeout
    CHECK_EQ(a.ep->stats().rxMessages, base);

    // 补喂剩余 54B：原消息不得派发（不重join；0 错误）。
    a.ep->onTransportData(bytes.data() + 30, bytes.size() - 30);
    CHECK_EQ(a.ep->stats().rxMessages, base);
    CHECK_EQ(a.ep->stats().decoderErrors, 0u);

    // 新完整帧（seq 从 0 起）→ 正常恢复：派发 3 条 + 提交 1 次（无重复）。
    SequenceCounter fresh(0);
    const auto frameBytes = encodeFrameBytes(fresh);
    a.ep->onTransportData(frameBytes.data(), frameBytes.size());
    CHECK_EQ(a.ep->stats().rxMessages, base + 3u);
    CHECK_EQ(a.commits.size(), 1u);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
}

}  // namespace

void runDecoderTimeoutTests() {
    std::printf("  partial_header_timeout_matrix\n");
    partial_header_timeout_matrix();
    std::printf("  partial_payload_no_rejoin\n");
    partial_payload_no_rejoin();
    std::printf("  garbage_no_error_loop\n");
    garbage_no_error_loop();
    std::printf("  new_magic_recovers\n");
    new_magic_recovers();
    std::printf("  full_frame_after_timeout_dispatches_once\n");
    full_frame_after_timeout_dispatches_once();
    std::printf("  chunked_timeout_clears_state_and_repeat_noop\n");
    chunked_timeout_clears_state_and_repeat_noop();
    std::printf("  endpoint_partial_header_timeout_flushes\n");
    endpoint_partial_header_timeout_flushes();
    std::printf("  endpoint_partial_payload_timeout_no_rejoin\n");
    endpoint_partial_payload_timeout_no_rejoin();
}
