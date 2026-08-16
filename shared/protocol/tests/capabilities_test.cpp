// ESPView — CAPABILITIES（TYPE 0x02）Host Tests（M7-D1）
//
// 规范来源：docs/DESIGN.md AD.2（payload 布局）/ AD.3（解析与兼容性规则）。
// 覆盖：
//   1. makeCapabilities → parseCapabilities roundtrip（全部字段）
//   2. 默认/全 0 字段（wire 宽松：不校验几何范围）
//   3. 短 payload（31B）拒绝
//   4. 长 payload（40B）忽略尾部
//   5. version=0x00 / 0x02 拒绝
//   6. 未知 controller 值（0x7F / 0xFF）→ UNKNOWN；0/1/2 白名单直通
//   7. modeMask 位解析
//   8. 旧 peer 兼容：无 CAPABILITIES 时 endpoint 正常；非法 0x02 不 failSession
//   9. little-endian 多字节字段验证
// 纯 C++17，零平台依赖。

#include <cstdint>
#include <memory>
#include <vector>

#include "message.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::BytesView;
using espview::proto::CapabilitiesController;
using espview::proto::CapabilitiesInfo;
using espview::proto::kCapabilitiesPayloadSize;
using espview::proto::makeCapabilities;
using espview::proto::makeMessage;
using espview::proto::MessageType;
using espview::proto::parseCapabilities;
using espview::proto::PhysicalPixelFormat;
using espview::proto::PixelFormat;

// 全字段样本（与 AD.2 表逐字段对应）。
CapabilitiesInfo sampleCaps() {
    CapabilitiesInfo c;
    c.virtualPresent = true;
    c.physicalPresent = true;
    c.width = 320;
    c.height = 240;
    c.pixelFormat = PixelFormat::kRgb565;
    c.colorDepth = 16;
    c.virtualMono = false;
    c.virtualCanReadback = true;
    c.modeMask = 0b1111;
    c.physWidth = 128;
    c.physHeight = 64;
    c.physPixelFormat = PhysicalPixelFormat::kMono1;
    c.physColorDepth = 1;
    c.physMono = true;
    c.physCanReadback = false;
    c.physController = CapabilitiesController::kSsd1306;
    c.physI2cAddress = 0x3C;
    c.sceneSupport = 0b11;
    return c;
}

// 手工构造原始 32 字节 payload（version 独立可控；测试 wire 级宽松语义）。
std::vector<uint8_t> rawPayload(uint8_t version) {
    std::vector<uint8_t> p(kCapabilitiesPayloadSize, 0);
    p[0] = version;
    return p;
}

void testRoundtripAllFields() {
    const CapabilitiesInfo c = sampleCaps();
    const auto msg = makeCapabilities(
        c.virtualPresent, c.physicalPresent, c.width, c.height, c.pixelFormat,
        c.colorDepth, c.virtualMono, c.virtualCanReadback, c.modeMask, c.physWidth,
        c.physHeight, c.physPixelFormat, c.physColorDepth, c.physMono,
        c.physCanReadback, c.physController, c.physI2cAddress, c.sceneSupport);
    CHECK(msg.has_value());
    CHECK_EQ(msg->type, static_cast<uint8_t>(MessageType::kCapabilities));
    CHECK_EQ(msg->flags, 0u);  // AD.3：不带 ACK_REQ
    CHECK_EQ(msg->payload.size(), kCapabilitiesPayloadSize);

    CapabilitiesInfo out;
    CHECK(parseCapabilities(BytesView(msg->payload.data(), msg->payload.size()), out));
    CHECK_EQ(out.version, 1u);
    CHECK(out.virtualPresent);
    CHECK(out.physicalPresent);
    CHECK_EQ(out.width, 320u);
    CHECK_EQ(out.height, 240u);
    CHECK_EQ(out.pixelFormat, PixelFormat::kRgb565);
    CHECK_EQ(out.colorDepth, 16u);
    CHECK(!out.virtualMono);
    CHECK(out.virtualCanReadback);
    CHECK_EQ(out.modeMask, 0b1111u);
    CHECK_EQ(out.physWidth, 128u);
    CHECK_EQ(out.physHeight, 64u);
    CHECK_EQ(out.physPixelFormat, PhysicalPixelFormat::kMono1);
    CHECK_EQ(out.physColorDepth, 1u);
    CHECK(out.physMono);
    CHECK(!out.physCanReadback);
    CHECK_EQ(out.physController, CapabilitiesController::kSsd1306);
    CHECK_EQ(out.physI2cAddress, 0x3Cu);
    CHECK_EQ(out.sceneSupport, 0b11u);
}

void testDefaultZeroFields() {
    // 全 0 字段（仅 version=0x01）：parse 宽松接受，不校验几何范围。
    const std::vector<uint8_t> p = rawPayload(0x01);
    CapabilitiesInfo out;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.version, 1u);
    CHECK(!out.virtualPresent);
    CHECK(!out.physicalPresent);
    CHECK_EQ(out.width, 0u);
    CHECK_EQ(out.height, 0u);
    CHECK_EQ(out.colorDepth, 0u);
    CHECK(!out.virtualMono);
    CHECK(!out.virtualCanReadback);
    CHECK_EQ(out.modeMask, 0u);
    CHECK_EQ(out.physWidth, 0u);
    CHECK_EQ(out.physHeight, 0u);
    CHECK_EQ(out.physPixelFormat, PhysicalPixelFormat::kRgb565);
    CHECK_EQ(out.physColorDepth, 0u);
    CHECK(!out.physMono);
    CHECK(!out.physCanReadback);
    // 全 0 字段：controller 字节 0 是白名单值 kAuto（AD.2 0=AUTO），非 UNKNOWN。
    CHECK_EQ(out.physController, CapabilitiesController::kAuto);
    CHECK_EQ(out.physI2cAddress, 0u);
    CHECK_EQ(out.sceneSupport, 0u);
}

void testShortPayloadRejected() {
    std::vector<uint8_t> p = rawPayload(0x01);
    p.resize(kCapabilitiesPayloadSize - 1);  // 31B
    CapabilitiesInfo out;
    CHECK(!parseCapabilities(BytesView(p), out));
    // 更短的 0B / 16B 同样拒绝。
    CHECK(!parseCapabilities(BytesView(p.data(), 0), out));
    CHECK(!parseCapabilities(BytesView(p.data(), 16), out));
}

void testLongPayloadIgnoresTail() {
    std::vector<uint8_t> p(40, 0xAA);  // 40B：尾部 8B 应被忽略
    p[0] = 0x01;
    p[1] = 0x03;  // virtual + physical
    p[2] = 0x40;
    p[3] = 0x01;  // width=320 LE
    p[4] = 0xF0;
    p[5] = 0x00;  // height=240 LE
    p[16] = 0x80;
    p[17] = 0x00;  // physWidth=128
    p[24] = 0x01;  // SSD1306
    p[25] = 0x3C;
    CapabilitiesInfo out;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.width, 320u);
    CHECK_EQ(out.height, 240u);
    CHECK(out.virtualPresent);
    CHECK(out.physicalPresent);
    CHECK_EQ(out.physWidth, 128u);
    CHECK_EQ(out.physController, CapabilitiesController::kSsd1306);
    CHECK_EQ(out.physI2cAddress, 0x3Cu);
}

void testVersionRejected() {
    CapabilitiesInfo out;
    CHECK(!parseCapabilities(BytesView(rawPayload(0x00)), out));  // version=0x00
    CHECK(!parseCapabilities(BytesView(rawPayload(0x02)), out));  // version>0x01
    CHECK(!parseCapabilities(BytesView(rawPayload(0xFF)), out));
}

void testUnknownControllerMapped() {
    // 未知值 0x7F → UNKNOWN（AD.3：杜绝 UI 数值注入）。
    std::vector<uint8_t> p = rawPayload(0x01);
    p[24] = 0x7F;
    CapabilitiesInfo out;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.physController, CapabilitiesController::kUnknown);

    // 0xFF（UNKNOWN 语义值）→ UNKNOWN。
    p[24] = 0xFF;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.physController, CapabilitiesController::kUnknown);

    // 白名单直通：0=AUTO 1=SSD1306 2=SH1106。
    p[24] = 0x00;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.physController, CapabilitiesController::kAuto);
    p[24] = 0x01;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.physController, CapabilitiesController::kSsd1306);
    p[24] = 0x02;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.physController, CapabilitiesController::kSh1106);
}

void testModeMaskBits() {
    // modeMask bit0..3 = WINDOW/DEVICE/MIRROR/SPLIT；位解析逐位保持。
    std::vector<uint8_t> p = rawPayload(0x01);
    p[10] = 0b1011;  // WINDOW + MIRROR + SPLIT
    CapabilitiesInfo out;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.modeMask, 0b1011u);

    // makeCapabilities 侧：保留位拒绝；合法位 roundtrip。
    CHECK(!makeCapabilities(true, false, 320, 240, PixelFormat::kRgb565, 16, false,
                            true, 0x1Fu, 0, 0, PhysicalPixelFormat::kRgb565, 0, false,
                            false, CapabilitiesController::kUnknown, 0, 0).has_value());
    const auto msg = makeCapabilities(true, false, 320, 240, PixelFormat::kRgb565, 16,
                                      false, true, 0b0101u, 0, 0,
                                      PhysicalPixelFormat::kRgb565, 0, false, false,
                                      CapabilitiesController::kUnknown, 0, 0);
    CHECK(msg.has_value());
    CHECK(parseCapabilities(BytesView(msg->payload), out));
    CHECK_EQ(out.modeMask, 0b0101u);
}

void testLittleEndianMultiByte() {
    // 手工 payload：width=0x1234 height=0x5678 physWidth=0x9ABC physHeight=0xDEF0，
    // 验证 LE 解码（低字节在前）。
    std::vector<uint8_t> p(kCapabilitiesPayloadSize, 0);
    p[0] = 0x01;
    p[2] = 0x34;
    p[3] = 0x12;
    p[4] = 0x78;
    p[5] = 0x56;
    p[16] = 0xBC;
    p[17] = 0x9A;
    p[18] = 0xF0;
    p[19] = 0xDE;
    CapabilitiesInfo out;
    CHECK(parseCapabilities(BytesView(p), out));
    CHECK_EQ(out.width, 0x1234u);
    CHECK_EQ(out.height, 0x5678u);
    CHECK_EQ(out.physWidth, 0x9ABCu);
    CHECK_EQ(out.physHeight, 0xDEF0u);

    // makeCapabilities 编码侧：payload 字节序小端（值须在 1..4096 白名单内，
    // 用高/低字节可区分的样本：0x0A08/0x0B09/0x0C0A/0x0D0B）。
    const auto msg = makeCapabilities(true, false, 0x0A08u, 0x0B09u,
                                      PixelFormat::kRgb565, 16, false, true, 0x01u,
                                      0x0C0Au, 0x0D0Bu, PhysicalPixelFormat::kMono1,
                                      1, true, false, CapabilitiesController::kSh1106,
                                      0x3C, 0x03u);
    CHECK(msg.has_value());
    CHECK_EQ(msg->payload[2], 0x08u);
    CHECK_EQ(msg->payload[3], 0x0Au);
    CHECK_EQ(msg->payload[4], 0x09u);
    CHECK_EQ(msg->payload[5], 0x0Bu);
    CHECK_EQ(msg->payload[16], 0x0Au);
    CHECK_EQ(msg->payload[17], 0x0Cu);
    CHECK_EQ(msg->payload[18], 0x0Bu);
    CHECK_EQ(msg->payload[19], 0x0Du);
}

void testBuilderValidation() {
    // 虚拟几何越界 → nullopt。
    CHECK(!makeCapabilities(true, false, 0, 240, PixelFormat::kRgb565, 16, false, true,
                            0x01u, 0, 0, PhysicalPixelFormat::kRgb565, 0, false, false,
                            CapabilitiesController::kUnknown, 0, 0).has_value());
    CHECK(!makeCapabilities(true, false, 4097, 240, PixelFormat::kRgb565, 16, false,
                            true, 0x01u, 0, 0, PhysicalPixelFormat::kRgb565, 0, false,
                            false, CapabilitiesController::kUnknown, 0, 0).has_value());
    // 物理几何越界（>4096）→ nullopt；0=未知 合法。
    CHECK(!makeCapabilities(true, false, 320, 240, PixelFormat::kRgb565, 16, false,
                            true, 0x01u, 4097, 0, PhysicalPixelFormat::kRgb565, 0,
                            false, false, CapabilitiesController::kUnknown, 0, 0)
               .has_value());
    CHECK(makeCapabilities(true, false, 320, 240, PixelFormat::kRgb565, 16, false, true,
                           0x01u, 0, 0, PhysicalPixelFormat::kRgb565, 0, false, false,
                           CapabilitiesController::kUnknown, 0, 0)
              .has_value());
    // 枚举白名单外 → nullopt。
    CHECK(!makeCapabilities(true, false, 320, 240, static_cast<PixelFormat>(0x55), 16,
                            false, true, 0x01u, 0, 0, PhysicalPixelFormat::kRgb565, 0,
                            false, false, CapabilitiesController::kUnknown, 0, 0)
               .has_value());
    CHECK(!makeCapabilities(true, false, 320, 240, PixelFormat::kRgb565, 16, false,
                            true, 0x01u, 0, 0, static_cast<PhysicalPixelFormat>(0x55),
                            0, false, false, CapabilitiesController::kUnknown, 0, 0)
               .has_value());
    CHECK(!makeCapabilities(true, false, 320, 240, PixelFormat::kRgb565, 16, false,
                            true, 0x01u, 0, 0, PhysicalPixelFormat::kRgb565, 0, false,
                            false, static_cast<CapabilitiesController>(0x55), 0, 0)
               .has_value());
    // sceneSupport 保留位 → nullopt。
    CHECK(!makeCapabilities(true, false, 320, 240, PixelFormat::kRgb565, 16, false,
                            true, 0x01u, 0, 0, PhysicalPixelFormat::kRgb565, 0, false,
                            false, CapabilitiesController::kUnknown, 0, 0x04u)
               .has_value());
}

}  // namespace

// ---- Endpoint 级：旧 peer 兼容 + 0x02 不产生错误（复用协议_endpoint 测试模式）----
namespace endpoint_caps {

using espview::proto::CapabilitiesInfo;
using espview::proto::EndpointConfig;
using espview::proto::HelloInfo;
using espview::proto::kCapabilitiesPayloadSize;
using espview::proto::makeCapabilities;
using espview::proto::makeMessage;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
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

// 简化双端 harness：A 发、B 收（B 侧捕获 capabilities 回调与错误）。
struct CapHarness {
    FakeClock clock;
    CapHarness* peer = nullptr;  // sink 目标（对端 rx）
    std::vector<uint8_t> rx;
    std::vector<SessionState> states;
    std::vector<SessionError> protoErrors;
    std::vector<CapabilitiesInfo> caps;
    std::vector<Message> others;
    std::unique_ptr<ProtocolEndpoint> ep;

    void init(CapHarness* p) {
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
        cb.onCapabilities = [this](const CapabilitiesInfo& c) { caps.push_back(c); };
        cb.onOtherMessage = [this](const Message& m) { others.push_back(m); };
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

// 独立编码器：把 Message 编码成字节喂给对端（seq 从 0 起，与握手后基线一致）。
struct Feeder {
    SequenceCounter seq;
    MessageEncoder enc;
    Feeder() : enc(seq) {}
    void pushTo(CapHarness& to, const Message& msg) {
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

void connectPair(CapHarness& a, CapHarness& b) {
    // init 已把 a 的 sink 指向 b、b 的 sink 指向 a（与 protocol_endpoint_test
    // 的 in-memory 互连一致；握手 HELLO 经真实 Encoder→Decoder 交换）。
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    a.pump();
    b.pump();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

void runEndpointOldPeerCompatibility() {
    CapHarness a;
    CapHarness b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // 8a. 旧 peer（从不发 CAPABILITIES）：会话正常，无任何协议错误。
    CHECK_EQ(a.protoErrors.size(), 0u);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);

    // 8b. 合法 CAPABILITIES：B 收到回调，会话保持 CONNECTED，无错误。
    Feeder feed;
    const CapabilitiesInfo c = sampleCaps();
    const auto msg = makeCapabilities(
        c.virtualPresent, c.physicalPresent, c.width, c.height, c.pixelFormat,
        c.colorDepth, c.virtualMono, c.virtualCanReadback, c.modeMask, c.physWidth,
        c.physHeight, c.physPixelFormat, c.physColorDepth, c.physMono,
        c.physCanReadback, c.physController, c.physI2cAddress, c.sceneSupport);
    CHECK(msg.has_value());
    feed.pushTo(b, *msg);
    b.pump();
    CHECK_EQ(b.caps.size(), 1u);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->stats().rxCapabilities, 1u);
    CHECK_EQ(b.ep->stats().capabilitiesDropped, 0u);

    // 8c. 短 payload（31B）：丢弃并计数，不 failSession。
    std::vector<uint8_t> shortPayload(kCapabilitiesPayloadSize - 1, 0);
    feed.pushTo(b, makeMessage(static_cast<uint8_t>(MessageType::kCapabilities), 0,
                               shortPayload));
    b.pump();
    CHECK_EQ(b.caps.size(), 1u);  // 未新增回调
    CHECK_EQ(b.ep->stats().capabilitiesDropped, 1u);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);

    // 8d. 未知 version（0x02）：丢弃并计数，不 failSession。
    std::vector<uint8_t> badVersion = rawPayload(0x02);
    feed.pushTo(b, makeMessage(static_cast<uint8_t>(MessageType::kCapabilities), 0,
                               badVersion));
    b.pump();
    CHECK_EQ(b.caps.size(), 1u);
    CHECK_EQ(b.ep->stats().capabilitiesDropped, 2u);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);

    // 8e. 会话仍可继续正常收发（PING 不被 CAPABILITIES 破坏）。
    a.clock.now = 100;
    b.clock.now = 100;
    const auto ping = makeMessage(static_cast<uint8_t>(MessageType::kPing), 0,
                                  std::vector<uint8_t>(8, 0));
    feed.pushTo(a, ping);
    a.pump();
    CHECK_EQ(a.protoErrors.size(), 0u);
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
}

void runEndpointSendCapabilities() {
    CapHarness a;
    CapHarness b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // sendCapabilities：CONNECTED 后发送成功，wire 不带 ACK_REQ（fire-and-forget）。
    const CapabilitiesInfo c = sampleCaps();
    CHECK_EQ(a.ep->sendCapabilities(c), SendResult::kOk);
    CHECK_EQ(a.ep->stats().txCapabilities, 1u);

    // 未连接时拒绝（kNotConnected）。
    CapHarness standalone;
    standalone.init(nullptr);
    CHECK_EQ(standalone.ep->sendCapabilities(c), SendResult::kNotConnected);
}

}  // namespace endpoint_caps

void runCapabilitiesTests() {
    testRoundtripAllFields();
    testDefaultZeroFields();
    testShortPayloadRejected();
    testLongPayloadIgnoresTail();
    testVersionRejected();
    testUnknownControllerMapped();
    testModeMaskBits();
    testLittleEndianMultiByte();
    testBuilderValidation();
    endpoint_caps::runEndpointOldPeerCompatibility();
    endpoint_caps::runEndpointSendCapabilities();
}
