// ESPView — PHYSICAL_PREVIEW（TYPE 0x13）Host Tests（M7-D2）
//
// 规范来源：docs/DESIGN.md AE.2（payload 布局）/ AE.3（传输语义）。
// 覆盖：
//   1. makePhysicalPreview → parsePhysicalPreview roundtrip（全部字段 + 1024B 像素）
//   2. 字段 LE 编码（frameId/width/height 高字节位置）
//   3. 小 payload（1031B）拒绝；长 payload（1040B）忽略尾部
//   4. 构造检验：width/height 超范围、pixelFormat 非 kMono1、flags 保留位、pixels=nullptr 均拒绝
//   5. 未知 pixelFormat wire 值兑底 kMono1
//   6. frameId 0 / 65535 边界
//   7. endpoint 分发：合法包触发回调、短包不 failSession
//   8. sendPhysicalPreview：CONNECTED 发送成功，未连接拒绝
// 纯 C++17，零平台依赖。

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "encoder.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::BytesView;
using espview::proto::kPhysicalPreviewPayloadSize;
using espview::proto::kPhysicalPreviewPixelBytes;
using espview::proto::kPhysicalPreviewPixelOffset;
using espview::proto::makeMessage;
using espview::proto::makePhysicalPreview;
using espview::proto::Message;
using espview::proto::MessageType;
using espview::proto::parsePhysicalPreview;
using espview::proto::PhysicalPixelFormat;
using espview::proto::PhysicalPreviewInfo;

// 填充模式：与 OledFb 无关，仅验证字节保真。
std::vector<uint8_t> pixelsPattern(uint8_t seed = 0x5A) {
    std::vector<uint8_t> px(kPhysicalPreviewPixelBytes);
    for (size_t i = 0; i < px.size(); ++i) {
        px[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
    }
    return px;
}

void testRoundtripAllFields() {
    const std::vector<uint8_t> px = pixelsPattern();
    const auto msg = makePhysicalPreview(0x0102, 128, 64, PhysicalPixelFormat::kMono1,
                                         0, px.data());
    CHECK(msg.has_value());
    CHECK_EQ(msg->type, static_cast<uint8_t>(MessageType::kPhysicalPreview));
    CHECK_EQ(msg->flags, 0u);  // AE.3：不带 ACK_REQ
    CHECK_EQ(msg->payload.size(), kPhysicalPreviewPayloadSize);

    // 字段 LE 编码（逐字节验证）
    CHECK_EQ(msg->payload[0], 0x02u);  // frameId 低
    CHECK_EQ(msg->payload[1], 0x01u);  // frameId 高
    CHECK_EQ(msg->payload[2], 128u);   // width 低
    CHECK_EQ(msg->payload[3], 0u);     // width 高
    CHECK_EQ(msg->payload[4], 64u);    // height 低
    CHECK_EQ(msg->payload[5], 0u);     // height 高
    CHECK_EQ(msg->payload[6], 1u);     // pixelFormat=kMono1
    CHECK_EQ(msg->payload[7], 0u);     // flags
    CHECK(memcmp(msg->payload.data() + kPhysicalPreviewPixelOffset, px.data(),
                 kPhysicalPreviewPixelBytes) == 0);

    PhysicalPreviewInfo out;
    CHECK(parsePhysicalPreview(BytesView(msg->payload.data(), msg->payload.size()), out));
    CHECK_EQ(out.frameId, 0x0102u);
    CHECK_EQ(out.width, 128u);
    CHECK_EQ(out.height, 64u);
    CHECK(out.pixelFormat == PhysicalPixelFormat::kMono1);
    CHECK_EQ(out.flags, 0u);
}

void testLittleEndianFields() {
    const std::vector<uint8_t> px = pixelsPattern();
    // 宽高取 4096 内可区分值（frameId 无范围限制）。
    const auto msg = makePhysicalPreview(0xABCD, 0x0A08u, 0x0B09u,
                                         PhysicalPixelFormat::kMono1, 0, px.data());
    CHECK(msg.has_value());
    CHECK_EQ(msg->payload[0], 0xCDu);
    CHECK_EQ(msg->payload[1], 0xABu);
    CHECK_EQ(msg->payload[2], 0x08u);
    CHECK_EQ(msg->payload[3], 0x0Au);
    CHECK_EQ(msg->payload[4], 0x09u);
    CHECK_EQ(msg->payload[5], 0x0Bu);
}

void testShortPayloadRejected() {
    const auto msg = makePhysicalPreview(1, 128, 64, PhysicalPixelFormat::kMono1, 0,
                                         pixelsPattern().data());
    CHECK(msg.has_value());
    std::vector<uint8_t> shortP(msg->payload.begin(), msg->payload.begin() + 1031);
    PhysicalPreviewInfo out;
    CHECK(!parsePhysicalPreview(BytesView(shortP.data(), shortP.size()), out));
    CHECK(!parsePhysicalPreview(BytesView(shortP.data(), 0), out));
}

void testLongPayloadIgnoresTail() {
    const auto msg = makePhysicalPreview(7, 128, 64, PhysicalPixelFormat::kMono1, 0,
                                         pixelsPattern().data());
    CHECK(msg.has_value());
    std::vector<uint8_t> longP = msg->payload;
    longP.push_back(0xEE);  // 尾部额外字节
    PhysicalPreviewInfo out;
    CHECK(parsePhysicalPreview(BytesView(longP.data(), longP.size()), out));
    CHECK_EQ(out.frameId, 7u);
}

void testBuilderValidation() {
    const std::vector<uint8_t> px = pixelsPattern();
    // width/height 超范围
    CHECK(!makePhysicalPreview(1, 0, 64, PhysicalPixelFormat::kMono1, 0, px.data()).has_value());
    CHECK(!makePhysicalPreview(1, 4097, 64, PhysicalPixelFormat::kMono1, 0, px.data()).has_value());
    CHECK(!makePhysicalPreview(1, 128, 0, PhysicalPixelFormat::kMono1, 0, px.data()).has_value());
    // pixelFormat 非 kMono1
    CHECK(!makePhysicalPreview(1, 128, 64, PhysicalPixelFormat::kRgb565, 0, px.data()).has_value());
    // flags 保留位
    CHECK(!makePhysicalPreview(1, 128, 64, PhysicalPixelFormat::kMono1, 0x80, px.data()).has_value());
    // pixels 必填
    CHECK(!makePhysicalPreview(1, 128, 64, PhysicalPixelFormat::kMono1, 0, nullptr).has_value());
}

void testUnknownPixelFormatFallsBack() {
    // wire 上 pixelFormat=0xFF（未知）→ 兑底 kMono1，不拒绝
    const auto msg = makePhysicalPreview(3, 128, 64, PhysicalPixelFormat::kMono1, 0,
                                         pixelsPattern().data());
    CHECK(msg.has_value());
    std::vector<uint8_t> raw = msg->payload;
    raw[6] = 0xFF;
    PhysicalPreviewInfo out;
    CHECK(parsePhysicalPreview(BytesView(raw.data(), raw.size()), out));
    CHECK(out.pixelFormat == PhysicalPixelFormat::kMono1);
}

void testFrameIdBoundaries() {
    const std::vector<uint8_t> px = pixelsPattern();
    for (uint16_t fid : {0u, 65535u}) {
        const auto msg = makePhysicalPreview(fid, 128, 64, PhysicalPixelFormat::kMono1,
                                             0, px.data());
        CHECK(msg.has_value());
        PhysicalPreviewInfo out;
        CHECK(parsePhysicalPreview(BytesView(msg->payload.data(), msg->payload.size()), out));
        CHECK_EQ(out.frameId, fid);
    }
}

}  // namespace

// ---- Endpoint 级：分发 + 发送（复用 capabilities 测试 harness 模式）----
namespace endpoint_pp {

using espview::proto::EndpointConfig;
using espview::proto::HelloInfo;
using espview::proto::makeHello;
using espview::proto::MessageEncoder;
using espview::proto::PacketError;
using espview::proto::ProtocolEndpoint;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionError;
using espview::proto::SessionState;

struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct PpHarness {
    FakeClock clock;
    PpHarness* peer = nullptr;
    std::vector<uint8_t> rx;
    std::vector<SessionState> states;
    std::vector<SessionError> protoErrors;
    std::vector<PhysicalPreviewInfo> previews;
    std::vector<std::vector<uint8_t>> previewPixels;
    std::unique_ptr<ProtocolEndpoint> ep;

    void init(PpHarness* p) {
        peer = p;
        EndpointConfig cfg;
        cfg.protocol_version = 1;
        cfg.width = 320;
        cfg.height = 240;
        ProtocolEndpoint::Callbacks cb;
        cb.onSessionState = [this](SessionState s) { states.push_back(s); };
        cb.onProtocolError = [this](SessionError e, std::string_view) {
            protoErrors.push_back(e);
        };
        cb.onPhysicalPreview = [this](const PhysicalPreviewInfo& info,
                                      const uint8_t* px, size_t n) {
            previews.push_back(info);
            previewPixels.emplace_back(px, px + n);
        };
        ep = std::make_unique<ProtocolEndpoint>(
            cfg, [this](const uint8_t* d, size_t n) {
                if (peer != nullptr) {
                    peer->rx.insert(peer->rx.end(), d, d + n);
                }
                return SendStatus::kOk;
            },
            cb, [this]() { return clock.now; });
    }

    void pump() {
        std::vector<uint8_t> data = std::move(rx);
        rx.clear();
        if (!data.empty()) {
            ep->onTransportData(data.data(), data.size());
        }
    }
};

struct Feeder {
    SequenceCounter seq;
    MessageEncoder enc;
    Feeder() : enc(seq) {}
    void pushTo(PpHarness& to, const Message& msg) {
        std::vector<std::vector<uint8_t>> pkts;
        if (enc.encode(msg, pkts) != PacketError::kNone) {
            CHECK_MSG(false, "encode failed");
            return;
        }
        for (const auto& p : pkts) {
            to.rx.insert(to.rx.end(), p.begin(), p.end());
        }
    }
};

void connectPair(PpHarness& a, PpHarness& b) {
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    a.pump();
    b.pump();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

void runEndpointDispatch() {
    PpHarness a;
    PpHarness b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // 合法 PHYSICAL_PREVIEW：B 收到回调 + 像素，会话保持 CONNECTED。
    Feeder feed;
    const std::vector<uint8_t> px = pixelsPattern(0x33);
    const auto msg = makePhysicalPreview(0x0102, 128, 64, PhysicalPixelFormat::kMono1,
                                         0, px.data());
    CHECK(msg.has_value());
    feed.pushTo(b, *msg);
    b.pump();
    CHECK_EQ(b.previews.size(), 1u);
    CHECK_EQ(b.previews[0].frameId, 0x0102u);
    CHECK_EQ(b.previews[0].width, 128u);
    CHECK_EQ(b.previews[0].height, 64u);
    CHECK_EQ(b.previewPixels.size(), 1u);
    CHECK_EQ(b.previewPixels[0].size(), kPhysicalPreviewPixelBytes);
    CHECK(memcmp(b.previewPixels[0].data(), px.data(), kPhysicalPreviewPixelBytes) == 0);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->stats().rxPhysicalPreview, 1u);
    CHECK_EQ(b.ep->stats().physicalPreviewDropped, 0u);

    // 短 payload（1031B）：丢弃并计数，不 failSession。
    std::vector<uint8_t> shortPayload(kPhysicalPreviewPayloadSize - 1, 0);
    feed.pushTo(b, makeMessage(static_cast<uint8_t>(MessageType::kPhysicalPreview), 0,
                               shortPayload));
    b.pump();
    CHECK_EQ(b.previews.size(), 1u);  // 未新增回调
    CHECK_EQ(b.ep->stats().physicalPreviewDropped, 1u);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

void runEndpointSendPhysicalPreview() {
    PpHarness a;
    PpHarness b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    const std::vector<uint8_t> px = pixelsPattern(0x77);
    PhysicalPreviewInfo info;
    info.frameId = 9;
    info.width = 128;
    info.height = 64;
    info.pixelFormat = PhysicalPixelFormat::kMono1;
    info.flags = 0;
    CHECK_EQ(a.ep->sendPhysicalPreview(info, px.data()), SendResult::kOk);
    CHECK_EQ(a.ep->stats().txPhysicalPreview, 1u);
    b.pump();
    CHECK_EQ(b.previews.size(), 1u);
    CHECK_EQ(b.previews[0].frameId, 9u);

    // 未连接时拒绝（kNotConnected）。
    PpHarness standalone;
    standalone.init(nullptr);
    CHECK_EQ(standalone.ep->sendPhysicalPreview(info, px.data()),
             SendResult::kNotConnected);
}

}  // namespace endpoint_pp

void runPhysicalPreviewTests() {
    testRoundtripAllFields();
    testLittleEndianFields();
    testShortPayloadRejected();
    testLongPayloadIgnoresTail();
    testBuilderValidation();
    testUnknownPixelFormatFallsBack();
    testFrameIdBoundaries();
    endpoint_pp::runEndpointDispatch();
    endpoint_pp::runEndpointSendPhysicalPreview();
}
