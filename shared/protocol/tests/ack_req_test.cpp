// ESPView — ACK_REQ 白名单 Host Tests（M8-A1 Task 5）
//
// 规范来源：docs/DESIGN.md E 节 ACK 语义（v0.1：SET_MODE；M7-D3 追加
//   WIFI_SCAN_REQ/WIFI_CONFIG——均"必须 ACK_REQ"）。
// 白名单 = { SET_MODE(0x03), WIFI_SCAN_REQ(0x06), WIFI_CONFIG(0x08) }；
//   其余类型（FRAME_*/INPUT_*/PING/PONG/ACK/ERROR/HELLO/CAPABILITIES/
//   PHYSICAL_PREVIEW/WIFI_RESULT/WIFI_STATUS）禁止携带 ACK_REQ。
// 覆盖：
//   - Encoder（encode/encodeStream/encodeStreaming）：白名单外 + ACK_REQ →
//     kInvalidAckReq（实现层错误，非 wire 格式变化）；白名单内 encode()/
//     encodeStream() 正常编码且每包保留 ACK_REQ；ACK_REQ + payload>4096
//     （单包规则）→ kInvalidAckReq；encodeStreaming() 对任何类型一律拒绝
//     ACK_REQ（与 Endpoint 流式发送面一致）；
//   - Endpoint RX：白名单内 → onAckRequest 恰好一次（SET_MODE/WIFI_SCAN_REQ/
//     WIFI_CONFIG）；白名单外 → 忽略消息 + stats.invalidAckReq 计数
//     （不 invoke onAckRequest、不回 ACK、不发任何 wire 错误、不 failSession、
//     不投递 onOtherMessage、PING 不回 PONG）；
//   - StreamDecoder 保持 wire-transparent（不解释 ACK_REQ）——见 decoder 测试。
// 纯 C++17，零平台依赖。

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "encoder.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::DisplayMode;
using espview::proto::EndpointConfig;
using espview::proto::ErrorCode;
using espview::proto::IMessagePayloadSource;
using espview::proto::kFlagAckReq;
using espview::proto::kMaxPacketPayload;
using espview::proto::kPacketHeaderSize;
using espview::proto::makeHeader;
using espview::proto::makeHello;
using espview::proto::makeMessage;
using espview::proto::makeSetMode;
using espview::proto::makeWifiConfig;
using espview::proto::makeWifiScanReq;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageHeader;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionError;
using espview::proto::SessionState;

// 测试用 payload source（不持有整段逻辑视图之外的副本）。
class VecSource : public IMessagePayloadSource {
public:
    explicit VecSource(std::vector<uint8_t> data) : data_(std::move(data)) {}
    size_t read(uint8_t* dst, size_t maxBytes) override {
        const size_t n = std::min(maxBytes, data_.size() - off_);
        if (n > 0) {
            std::memcpy(dst, data_.data() + off_, n);
            off_ += n;
        }
        return n;
    }

private:
    std::vector<uint8_t> data_;
    size_t off_ = 0;
};

// 白名单内类型 + ACK_REQ：encode()/encodeStream() 成功且每包保留 ACK_REQ 位；
// encodeStreaming() 对任何类型一律拒绝 ACK_REQ（kInvalidAckReq，M8-A1）。
void encoder_whitelist_legal_types_accepted() {
    std::vector<Message> cases;
    cases.push_back(makeSetMode(DisplayMode::kWindow));  // SET_MODE 自动置 ACK_REQ
    const auto scanReq = makeWifiScanReq(0, 32);          // WIFI_SCAN_REQ 自动置 ACK_REQ
    CHECK(scanReq.has_value());
    cases.push_back(*scanReq);
    const auto cfg = makeWifiConfig("NetC", "password123", 0xC0A80164u, 8765);
    CHECK(cfg.has_value());
    cases.push_back(*cfg);

    for (const auto& m : cases) {
        SequenceCounter seqA, seqB, seqC;
        MessageEncoder encA(seqA), encB(seqB), encC(seqC);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(encA.encode(m, pkts), PacketError::kNone);
        CHECK(!pkts.empty());
        for (const auto& p : pkts) {
            PacketHeader h;
            CHECK_EQ(decodeHeader(p.data(), p.size(), &h), PacketError::kNone);
            CHECK_EQ(h.flags & kFlagAckReq, kFlagAckReq);
        }
        std::vector<std::vector<uint8_t>> streamed;
        CHECK_EQ(encB.encodeStream(m, [&streamed](const uint8_t* d, size_t n) {
                     streamed.emplace_back(d, d + n);
                     return true;
                 }),
                 PacketError::kNone);
        CHECK_EQ(streamed.size(), pkts.size());
        for (size_t i = 0; i < pkts.size(); ++i) {
            CHECK(pkts[i] == streamed[i]);
        }
        // M8-A1：encodeStreaming 路径对任何类型一律拒绝 ACK_REQ（ACK_REQ 只允许
        // 单包控制消息，见 transmitStreamingImpl）——即使类型在白名单内。
        MessageHeader hdr{m.type, m.flags};
        VecSource src(m.payload);
        CHECK_EQ(encC.encodeStreaming(hdr, src, [](const uint8_t*, size_t) { return true; }),
                 PacketError::kInvalidAckReq);
    }
}

// 白名单外类型 + ACK_REQ：三条编码路径均 kInvalidAckReq，且不产生任何包。
void encoder_whitelist_illegal_types_rejected() {
    const std::vector<uint8_t> payload(4, 0x11);
    const uint8_t illegalTypes[] = {
        static_cast<uint8_t>(MessageType::kFrameBegin),
        static_cast<uint8_t>(MessageType::kFrameRect),
        static_cast<uint8_t>(MessageType::kFrameEnd),
        static_cast<uint8_t>(MessageType::kInputKey),
        static_cast<uint8_t>(MessageType::kInputMouse),
        static_cast<uint8_t>(MessageType::kInputTouch),
        static_cast<uint8_t>(MessageType::kPing),
        static_cast<uint8_t>(MessageType::kPong),
        static_cast<uint8_t>(MessageType::kAck),
        static_cast<uint8_t>(MessageType::kHello),
        static_cast<uint8_t>(MessageType::kError),
        static_cast<uint8_t>(MessageType::kCapabilities),
        static_cast<uint8_t>(MessageType::kWifiScanResult),
        static_cast<uint8_t>(MessageType::kWifiStatus),
    };
    for (const uint8_t t : illegalTypes) {
        const Message m = makeMessage(t, kFlagAckReq, payload);
        SequenceCounter seqA, seqB, seqC;
        MessageEncoder encA(seqA), encB(seqB), encC(seqC);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(encA.encode(m, pkts), PacketError::kInvalidAckReq);
        CHECK_EQ(pkts.size(), 0u);
        CHECK_EQ(encB.encodeStream(m, [](const uint8_t*, size_t) { return true; }),
                 PacketError::kInvalidAckReq);
        MessageHeader hdr{t, kFlagAckReq};
        VecSource src(payload);
        CHECK_EQ(encC.encodeStreaming(hdr, src, [](const uint8_t*, size_t) { return true; }),
                 PacketError::kInvalidAckReq);
    }
}

// ACK_REQ + payload > kMaxPacketPayload：单包规则防御性拒绝（encode/encodeStream）。
void encoder_ackreq_large_payload_rejected() {
    const Message m = makeMessage(static_cast<uint8_t>(MessageType::kSetMode), kFlagAckReq,
                                  std::vector<uint8_t>(kMaxPacketPayload + 1, 0));
    SequenceCounter seqA, seqB;
    MessageEncoder encA(seqA), encB(seqB);
    std::vector<std::vector<uint8_t>> pkts;
    CHECK_EQ(encA.encode(m, pkts), PacketError::kInvalidAckReq);
    CHECK_EQ(encB.encodeStream(m, [](const uint8_t*, size_t) { return true; }),
             PacketError::kInvalidAckReq);
}// ---- Endpoint RX 侧（假时钟；无真实对端）----

struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct AckHarness {
    FakeClock clock;
    std::vector<std::pair<uint8_t, uint16_t>> ackRequests;
    std::vector<SessionState> states;
    std::vector<uint16_t> protoErrorCodes;
    std::vector<Message> others;
    std::vector<std::vector<uint8_t>> txPackets;
    std::unique_ptr<ProtocolEndpoint> ep;

    void init() {
        ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](SessionState s) { states.push_back(s); };
        cb.onProtocolError = [this](SessionError e, std::string_view) {
            protoErrorCodes.push_back(static_cast<uint16_t>(e));
        };
        cb.onAckRequest = [this](uint8_t t, const std::vector<uint8_t>&, uint16_t s) {
            ackRequests.emplace_back(t, s);
        };
        cb.onOtherMessage = [this](const Message& m) { others.push_back(m); };
        auto sink = [this](const uint8_t* d, size_t n) {
            txPackets.emplace_back(d, d + n);
            return SendStatus::kOk;
        };
        ep = std::make_unique<ProtocolEndpoint>(EndpointConfig{}, sink, cb,
                                                [this]() { return clock.now; });
    }
    void feed(const uint8_t* d, size_t n) { ep->onTransportData(d, n); }
};

// 被动握手 → CONNECTED（无真实对端；喂入对端 HELLO）。
void connectHarness(AckHarness& h) {
    h.ep->onTransportConnected();
    SequenceCounter seq(0);
    MessageEncoder enc(seq);
    const auto hello = makeHello(1, 0, 320, 240, PixelFormat::kRgb565, 0b111, "ack-test");
    CHECK(hello.has_value());
    std::vector<std::vector<uint8_t>> pkts;
    CHECK_EQ(enc.encode(*hello, pkts), PacketError::kNone);
    for (const auto& p : pkts) {
        h.ep->onTransportData(p.data(), p.size());
    }
    CHECK_EQ(h.ep->state(), SessionState::kConnected);
}

// 白名单内类型到达 RX：onAckRequest 恰好一次（SET_MODE/WIFI_SCAN_REQ/WIFI_CONFIG）。
void endpoint_legal_ackreq_dispatches_on_ack_request() {
    AckHarness h;
    h.init();
    connectHarness(h);
    CHECK_EQ(h.ackRequests.size(), 0u);

    SequenceCounter seq(0);
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> pkts;

    CHECK_EQ(enc.encode(makeSetMode(DisplayMode::kWindow), pkts), PacketError::kNone);
    for (const auto& p : pkts) {
        h.ep->onTransportData(p.data(), p.size());
    }
    CHECK_EQ(h.ackRequests.size(), 1u);
    CHECK_EQ(h.ackRequests[0].first, static_cast<uint8_t>(MessageType::kSetMode));
    CHECK_EQ(h.protoErrorCodes.size(), 0u);

    pkts.clear();
    const auto scanReq = makeWifiScanReq(0, 32);
    CHECK(scanReq.has_value());
    CHECK_EQ(enc.encode(*scanReq, pkts), PacketError::kNone);
    for (const auto& p : pkts) {
        h.ep->onTransportData(p.data(), p.size());
    }
    CHECK_EQ(h.ackRequests.size(), 2u);
    CHECK_EQ(h.ackRequests[1].first, static_cast<uint8_t>(MessageType::kWifiScanReq));

    pkts.clear();
    const auto cfg = makeWifiConfig("NetC", "password123", 0xC0A80164u, 8765);
    CHECK(cfg.has_value());
    CHECK_EQ(enc.encode(*cfg, pkts), PacketError::kNone);
    for (const auto& p : pkts) {
        h.ep->onTransportData(p.data(), p.size());
    }
    CHECK_EQ(h.ackRequests.size(), 3u);
    CHECK_EQ(h.ackRequests[2].first, static_cast<uint8_t>(MessageType::kWifiConfig));

    CHECK_EQ(h.protoErrorCodes.size(), 0u);
    CHECK_EQ(h.ep->state(), SessionState::kConnected);
}

// 白名单外类型携带 ACK_REQ 的 wire 包：encodePacket 不做 ACK_REQ 校验，可
// 构造真实（CRC 正确）的违规包；Message 层 Encoder 已拒绝同场景（见
// encoder_whitelist_illegal_types_rejected）。
std::vector<uint8_t> encodeIllegalAckReqPacket(uint8_t type, uint16_t seq,
                                               const std::vector<uint8_t>& payload) {
    const PacketHeader h = makeHeader(type, kFlagAckReq, seq,
                                      static_cast<uint32_t>(payload.size()));
    std::array<uint8_t, kPacketHeaderSize + kMaxPacketPayload> raw{};
    size_t written = 0;
    CHECK_EQ(encodePacket(h, payload.data(), payload.size(), raw.data(), raw.size(), &written),
             PacketError::kNone);
    return std::vector<uint8_t>(raw.begin(), raw.begin() + written);
}

// 白名单外类型到达 RX：忽略 + stats.invalidAckReq 计数
// （不 invoke onAckRequest、不回 ACK、不发任何 wire 错误、不 failSession、
// 不投递 onOtherMessage、PING 不回 PONG）。
void endpoint_illegal_ackreq_ignored_and_counted() {
    AckHarness h;
    h.init();
    connectHarness(h);

    // seq 与 expectedSeq 对齐：握手后基线为 0，逐包递增。
    uint16_t seq = 0;
    const std::vector<uint8_t> payload(6, 0x00);
    auto feedIllegal = [&](uint8_t type) {
        const auto raw = encodeIllegalAckReqPacket(type, seq++, payload);
        h.ep->onTransportData(raw.data(), raw.size());
    };

    feedIllegal(static_cast<uint8_t>(MessageType::kInputMouse));
    CHECK_EQ(h.ackRequests.size(), 0u);  // 不触发 onAckRequest
    CHECK_EQ(h.others.size(), 0u);       // 不投递 onOtherMessage
    CHECK_EQ(h.ep->stats().invalidAckReq, 1u);
    CHECK_EQ(h.ep->stats().ackSent, 0u);      // 不回 ACK
    CHECK_EQ(h.protoErrorCodes.size(), 0u);   // 无 wire 错误
    CHECK_EQ(h.ep->state(), SessionState::kConnected);

    feedIllegal(static_cast<uint8_t>(MessageType::kFrameBegin));
    CHECK_EQ(h.ep->stats().invalidAckReq, 2u);
    CHECK_EQ(h.others.size(), 0u);

    // PING + ACK_REQ：不得触发自动 PONG（也不进入 handlePing）。
    feedIllegal(static_cast<uint8_t>(MessageType::kPing));
    CHECK_EQ(h.ep->stats().invalidAckReq, 3u);
    CHECK_EQ(h.ep->stats().txPong, 0u);
    CHECK_EQ(h.ep->stats().rxPing, 0u);

    feedIllegal(static_cast<uint8_t>(MessageType::kFrameRect));
    feedIllegal(static_cast<uint8_t>(MessageType::kFrameEnd));
    feedIllegal(static_cast<uint8_t>(MessageType::kInputKey));
    feedIllegal(static_cast<uint8_t>(MessageType::kPong));
    feedIllegal(static_cast<uint8_t>(MessageType::kAck));
    CHECK_EQ(h.ep->stats().invalidAckReq, 8u);

    // 全程无 ACK/回复发出；txPackets 仅含握手 HELLO。
    CHECK_EQ(h.txPackets.size(), 1u);
    CHECK_EQ(h.ackRequests.size(), 0u);
    CHECK_EQ(h.protoErrorCodes.size(), 0u);
    CHECK_EQ(h.ep->state(), SessionState::kConnected);
    // 非法消息被静默丢弃后，合法消息仍可正常处理（会话未受污染）。
    // seq 与 expectedSeq 对齐：8 个非法包已消耗 seq 0..7。
    SequenceCounter seq2(8);
    MessageEncoder enc2(seq2);
    std::vector<std::vector<uint8_t>> pkts;
    CHECK_EQ(enc2.encode(makeSetMode(DisplayMode::kMirror), pkts), PacketError::kNone);
    for (const auto& p : pkts) {
        h.ep->onTransportData(p.data(), p.size());
    }
    CHECK_EQ(h.ackRequests.size(), 1u);
    CHECK_EQ(h.ackRequests[0].first, static_cast<uint8_t>(MessageType::kSetMode));
}

}  // namespace

void runAckReqTests() {
    std::printf("  encoder_whitelist_legal_types_accepted\n");
    encoder_whitelist_legal_types_accepted();
    std::printf("  encoder_whitelist_illegal_types_rejected\n");
    encoder_whitelist_illegal_types_rejected();
    std::printf("  encoder_ackreq_large_payload_rejected\n");
    encoder_ackreq_large_payload_rejected();
    std::printf("  endpoint_legal_ackreq_dispatches_on_ack_request\n");
    endpoint_legal_ackreq_dispatches_on_ack_request();
    std::printf("  endpoint_illegal_ackreq_ignored_and_counted\n");
    endpoint_illegal_ackreq_ignored_and_counted();
}
