// ESPView — Wi-Fi Provisioning（TYPE 0x06..0x09）Host Tests（M7-D3）
//
// 规范来源：docs/DESIGN.md AF.2（payload 布局）/ AF.3（传输与 ACK 语义）。
// 覆盖：
//   1. WIFI_SCAN_REQ：布局/ACK_REQ/校验/解析
//   2. WIFI_CONFIG：103B 布局（ssid 填充、password 填充、serverIp 网络序、端口 LE）
//     校验（空/超长 ssid、1..7 短密码、>63 密码、0.0.0.0、端口 0）/ CLEAR / 解析
//   3. WIFI_SCAN_RESULT：record 42B 布局、1..64 条、truncated、total LE、解析/校验
//   4. WIFI_STATUS：≤49B 布局（phase/errorCode/rssi/ip 网络序/ssid）、绝无密码字段、
//     解析/校验
//   5. Endpoint：SCAN_RESULT/STATUS 分发（非法仅计数不 failSession）、
//     sendWifiScanResult/sendWifiStatus fire-and-forget（无 ACK_REQ）、未连接拒绝、
//     WIFI_SCAN_REQ/WIFI_CONFIG（ACK_REQ）→ onAckRequest 分派 → acknowledge
// 纯 C++17，零平台依赖。

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "message.h"
#include "protocol_endpoint.h"
#include "test_util.h"

namespace {

using espview::proto::BytesView;
using espview::proto::EndpointConfig;
using espview::proto::ErrorCode;
using espview::proto::kWifiConfigFlagClear;
using espview::proto::kWifiConfigPayloadSize;
using espview::proto::kWifiPasswordFieldBytes;
using espview::proto::kWifiScanReqMaxEntries;
using espview::proto::kWifiScanReqPayloadSize;
using espview::proto::kWifiScanResultFlagTruncated;
using espview::proto::kWifiScanResultMaxRecords;
using espview::proto::kWifiScanResultRecordBytes;
using espview::proto::kWifiSsidMaxBytes;
using espview::proto::kWifiStatusMaxPayload;
using espview::proto::kFlagAckReq;
using espview::proto::makeMessage;
using espview::proto::makeWifiClear;
using espview::proto::makeWifiConfig;
using espview::proto::makeWifiScanReq;
using espview::proto::makeWifiScanResult;
using espview::proto::makeWifiStatus;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::parseWifiConfig;
using espview::proto::parseWifiScanReq;
using espview::proto::parseWifiScanResult;
using espview::proto::parseWifiStatus;
using espview::proto::ProtocolEndpoint;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SequenceCounter;
using espview::proto::SessionError;
using espview::proto::SessionState;
using espview::proto::WifiConfigInfo;
using espview::proto::WifiScanRecordInfo;
using espview::proto::WifiScanResultInfo;
using espview::proto::WifiStatusInfo;

constexpr uint32_t kIp192_168_1_100 = 0xC0A80164u;  // 网络序 u32：192.168.1.100
constexpr uint32_t kIp10_0_0_2 = 0x0A000002u;        // 网络序 u32：10.0.0.2

// ---- 1. WIFI_SCAN_REQ ----

void testScanReqLayout() {
    const auto msg = makeWifiScanReq(0, 32);
    CHECK(msg.has_value());
    CHECK_EQ(msg->type, static_cast<uint8_t>(MessageType::kWifiScanReq));
    CHECK_EQ(msg->flags & kFlagAckReq, kFlagAckReq);  // AF.3：必须 ACK_REQ
    CHECK_EQ(msg->payload.size(), kWifiScanReqPayloadSize);
    CHECK_EQ(msg->payload[0], 0u);   // flags
    CHECK_EQ(msg->payload[1], 32u);  // maxEntries

    // maxEntries=0（默认 32）与上限 64 均合法。
    const auto d = makeWifiScanReq(0, 0);
    CHECK(d.has_value());
    CHECK_EQ(d->payload[1], 0u);
    const auto hi = makeWifiScanReq(0, 64);
    CHECK(hi.has_value());
    CHECK_EQ(hi->payload[1], 64u);
}

void testScanReqValidation() {
    CHECK(!makeWifiScanReq(0x01, 32).has_value());  // flags 保留位非 0
    CHECK(!makeWifiScanReq(0x80, 32).has_value());
    CHECK(!makeWifiScanReq(0, 65).has_value());     // maxEntries > 64
}

void testScanReqParse() {
    uint8_t flags = 0xFF, maxEntries = 0xFF;
    const std::vector<uint8_t> p = {0x00, 0x10};
    CHECK(parseWifiScanReq(BytesView(p), flags, maxEntries));
    CHECK_EQ(flags, 0u);
    CHECK_EQ(maxEntries, 0x10u);
    // 长度错误拒绝。
    const std::vector<uint8_t> p1 = {0x00};
    CHECK(!parseWifiScanReq(BytesView(p1), flags, maxEntries));
    const std::vector<uint8_t> p3 = {0x00, 0x01, 0x02};
    CHECK(!parseWifiScanReq(BytesView(p3), flags, maxEntries));
}

// ---- 2. WIFI_CONFIG ----

void testConfigLayout() {
    const auto msg = makeWifiConfig("MyNet", "password123", kIp192_168_1_100, 8765);
    CHECK(msg.has_value());
    CHECK_EQ(msg->type, static_cast<uint8_t>(MessageType::kWifiConfig));
    CHECK_EQ(msg->flags & kFlagAckReq, kFlagAckReq);
    CHECK_EQ(msg->payload.size(), kWifiConfigPayloadSize);
    CHECK_EQ(msg->payload[0], 0u);  // flags：非 CLEAR

    // ssid 定长 NUL 填充。
    const char* ssidExp = "MyNet";
    for (size_t i = 0; i < 32; ++i) {
        const uint8_t expected = i < std::strlen(ssidExp)
                                     ? static_cast<uint8_t>(ssidExp[i])
                                     : 0u;
        CHECK_EQ(msg->payload[1 + i], expected);
    }
    // password 定长 NUL 填充。
    const char* pwExp = "password123";
    for (size_t i = 0; i < 64; ++i) {
        const uint8_t expected = i < std::strlen(pwExp)
                                     ? static_cast<uint8_t>(pwExp[i])
                                     : 0u;
        CHECK_EQ(msg->payload[33 + i], expected);
    }
    // serverIp 网络序（大端写线：192.168.1.100 → C0 A8 01 64）。
    CHECK_EQ(msg->payload[97], 0xC0u);
    CHECK_EQ(msg->payload[98], 0xA8u);
    CHECK_EQ(msg->payload[99], 0x01u);
    CHECK_EQ(msg->payload[100], 0x64u);
    // serverPort LE。
    CHECK_EQ(msg->payload[101], 8765u & 0xFFu);
    CHECK_EQ(msg->payload[102], (8765u >> 8) & 0xFFu);
}

void testConfigValidation() {
    // 空 / 超长 ssid。
    CHECK(!makeWifiConfig("", "password123", kIp192_168_1_100, 8765).has_value());
    std::string longSsid(33, 'a');
    CHECK(!makeWifiConfig(longSsid, "password123", kIp192_168_1_100, 8765).has_value());
    // 1..7 字节短密码拒绝（AF.2：0 或 8..63）。
    CHECK(!makeWifiConfig("Net", "short", kIp192_168_1_100, 8765).has_value());
    // >63 字节拒绝（64-hex raw-PSK v0.1 不实现）。
    std::string longPw(64, 'x');
    CHECK(!makeWifiConfig("Net", longPw, kIp192_168_1_100, 8765).has_value());
    // 开放网络（空密码）合法。
    CHECK(makeWifiConfig("Net", "", kIp192_168_1_100, 8765).has_value());
    // serverIp 0.0.0.0 拒绝。
    CHECK(!makeWifiConfig("Net", "password123", 0, 8765).has_value());
    // serverPort 0 拒绝。
    CHECK(!makeWifiConfig("Net", "password123", kIp192_168_1_100, 0).has_value());
}

void testConfigClear() {
    const Message clear = makeWifiClear();
    CHECK_EQ(clear.type, static_cast<uint8_t>(MessageType::kWifiConfig));
    CHECK_EQ(clear.flags & kFlagAckReq, kFlagAckReq);
    CHECK_EQ(clear.payload.size(), kWifiConfigPayloadSize);
    CHECK_EQ(clear.payload[0], kWifiConfigFlagClear);
    // 其余字段全 0。
    for (size_t i = 1; i < clear.payload.size(); ++i) {
        CHECK_EQ(clear.payload[i], 0u);
    }
}

void testConfigParse() {
    const auto msg = makeWifiConfig("HomeNet", "secretpass", kIp10_0_0_2, 54321);
    CHECK(msg.has_value());
    WifiConfigInfo info;
    CHECK(parseWifiConfig(BytesView(msg->payload.data(), msg->payload.size()), info));
    CHECK_EQ(info.flags, 0u);
    CHECK(info.ssid == std::string("HomeNet"));
    CHECK(info.password == std::string("secretpass"));
    CHECK_EQ(info.serverIp, kIp10_0_0_2);
    CHECK_EQ(info.serverPort, 54321u);

    // 开放网络：password 空。
    const auto open = makeWifiConfig("OpenNet", "", kIp192_168_1_100, 8765);
    CHECK(open.has_value());
    WifiConfigInfo openInfo;
    CHECK(parseWifiConfig(BytesView(open->payload.data(), open->payload.size()), openInfo));
    CHECK(openInfo.password.empty());

    // CLEAR：flags bit0。
    const Message clear = makeWifiClear();
    WifiConfigInfo clearInfo;
    CHECK(parseWifiConfig(BytesView(clear.payload.data(), clear.payload.size()), clearInfo));
    CHECK_EQ(clearInfo.flags, kWifiConfigFlagClear);

    // 短包拒绝。
    const std::vector<uint8_t> shortP(kWifiConfigPayloadSize - 1, 0);
    CHECK(!parseWifiConfig(BytesView(shortP), info));
}

// ---- 3. WIFI_SCAN_RESULT ----

WifiScanRecordInfo makeRec(const std::string& ssid, int8_t rssi, uint8_t channel,
                           uint8_t authmode) {
    WifiScanRecordInfo r;
    r.ssid = ssid;
    r.rssi = rssi;
    r.channel = channel;
    r.authmode = authmode;
    // bssid 默认全 0；设几个非零字节便于 roundtrip 校验。
    r.bssid[0] = 0xAA;
    r.bssid[5] = 0xBB;
    return r;
}

void testScanResultLayout() {
    const WifiScanRecordInfo recs[] = {makeRec("ESP_AP_1", -45, 6, 3),
                                       makeRec("ESP_AP_2", -70, 1, 0)};
    const auto msg = makeWifiScanResult(7, true, 12, recs, 2);
    CHECK(msg.has_value());
    CHECK_EQ(msg->type, static_cast<uint8_t>(MessageType::kWifiScanResult));
    CHECK_EQ(msg->flags & kFlagAckReq, 0u);  // fire-and-forget
    CHECK_EQ(msg->payload.size(), 5u + 2u * kWifiScanResultRecordBytes);
    CHECK_EQ(msg->payload[0], 7u);           // scanSeq
    CHECK_EQ(msg->payload[1], 2u);           // count
    CHECK_EQ(msg->payload[2], kWifiScanResultFlagTruncated);  // truncated
    CHECK_EQ(msg->payload[3], 12u & 0xFFu);  // total LE
    CHECK_EQ(msg->payload[4], (12u >> 8) & 0xFFu);
    // record0 ssid 定长 32 NUL 填充。
    for (size_t i = 0; i < 32; ++i) {
        const uint8_t exp = i < 8 ? static_cast<uint8_t>("ESP_AP_1"[i]) : 0u;
        CHECK_EQ(msg->payload[5 + i], exp);
    }
    // record0 bssid[6] + rssi + channel + authmode + rsvd。
    CHECK_EQ(msg->payload[5 + 32 + 0], 0xAAu);
    CHECK_EQ(msg->payload[5 + 32 + 5], 0xBBu);
    CHECK_EQ(msg->payload[5 + 38], static_cast<uint8_t>(-45));
    CHECK_EQ(msg->payload[5 + 39], 6u);
    CHECK_EQ(msg->payload[5 + 40], 3u);
    CHECK_EQ(msg->payload[5 + 41], 0u);  // rsvd

    // 64 条 = 5 + 64*42 = 2693B（单包上限内）。
    std::vector<WifiScanRecordInfo> many;
    for (size_t i = 0; i < kWifiScanResultMaxRecords; ++i) {
        many.push_back(makeRec("AP" + std::to_string(i), -50, 1, 3));
    }
    const auto big = makeWifiScanResult(0, false, 100, many.data(), many.size());
    CHECK(big.has_value());
    CHECK_EQ(big->payload.size(), 5u + kWifiScanResultMaxRecords * kWifiScanResultRecordBytes);
    CHECK_EQ(big->payload.size(), 2693u);
}

void testScanResultValidation() {
    // 空记录（count=0，records=nullptr）合法。
    const auto empty = makeWifiScanResult(0, false, 0, nullptr, 0);
    CHECK(empty.has_value());
    CHECK_EQ(empty->payload.size(), 5u);
    // count>64 拒绝。
    const WifiScanRecordInfo one = makeRec("AP", -50, 1, 3);
    CHECK(!makeWifiScanResult(0, false, 0, &one, 65).has_value());
    // count>0 但 records==nullptr 拒绝。
    CHECK(!makeWifiScanResult(0, false, 0, nullptr, 1).has_value());
    // 空 ssid / 超长 ssid 拒绝。
    const WifiScanRecordInfo emptySsid;
    CHECK(!makeWifiScanResult(0, false, 0, &emptySsid, 1).has_value());
    WifiScanRecordInfo longSsid = makeRec(std::string(33, 'a'), -50, 1, 3);
    CHECK(!makeWifiScanResult(0, false, 0, &longSsid, 1).has_value());
    // authmode 越界（>8）拒绝。
    WifiScanRecordInfo badAuth = makeRec("AP", -50, 1, 9);
    CHECK(!makeWifiScanResult(0, false, 0, &badAuth, 1).has_value());
}

void testScanResultParse() {
    const WifiScanRecordInfo recs[] = {makeRec("WLAN-5G", -63, 11, 4),
                                       makeRec("WLAN-2G", -77, 1, 3)};
    const auto msg = makeWifiScanResult(3, true, 42, recs, 2);
    CHECK(msg.has_value());
    WifiScanResultInfo out;
    CHECK(parseWifiScanResult(BytesView(msg->payload.data(), msg->payload.size()), out));
    CHECK_EQ(out.scanSeq, 3u);
    CHECK_EQ(out.count, 2u);
    CHECK_EQ(out.flags, kWifiScanResultFlagTruncated);
    CHECK_EQ(out.total, 42u);
    CHECK_EQ(out.records.size(), 2u);
    CHECK(out.records[0].ssid == std::string("WLAN-5G"));
    CHECK_EQ(out.records[0].rssi, -63);
    CHECK_EQ(out.records[0].channel, 11u);
    CHECK_EQ(out.records[0].authmode, 4u);
    CHECK_EQ(out.records[0].bssid[0], 0xAAu);
    CHECK_EQ(out.records[0].bssid[5], 0xBBu);
    CHECK(out.records[1].ssid == std::string("WLAN-2G"));
    CHECK_EQ(out.records[1].rssi, -77);
    CHECK_EQ(out.records[1].channel, 1u);
    CHECK_EQ(out.records[1].authmode, 3u);

    // 短包（缺 record 尾部）拒绝。
    std::vector<uint8_t> shortP = msg->payload;
    shortP.resize(shortP.size() - 1);
    CHECK(!parseWifiScanResult(BytesView(shortP), out));
    // count 声明 65 拒绝。
    std::vector<uint8_t> overCount(5 + 42, 0);
    overCount[1] = 65;
    CHECK(!parseWifiScanResult(BytesView(overCount), out));
    // 尾部多余字节忽略（宽松）。
    std::vector<uint8_t> longP = msg->payload;
    longP.push_back(0xEE);
    WifiScanResultInfo out2;
    CHECK(parseWifiScanResult(BytesView(longP), out2));
    CHECK_EQ(out2.count, 2u);
    CHECK_EQ(out2.total, 42u);
}

// ---- 4. WIFI_STATUS ----

void testStatusLayout() {
    const auto msg = makeWifiStatus(5, 0, 0, -60, 6, kIp10_0_0_2, kIp192_168_1_100, 8765,
                                    "HomeNet");
    CHECK(msg.has_value());
    CHECK_EQ(msg->type, static_cast<uint8_t>(MessageType::kWifiStatus));
    CHECK_EQ(msg->flags & kFlagAckReq, 0u);  // fire-and-forget
    CHECK_EQ(msg->payload.size(), 17u + 7u);  // 17 + ssidLen
    CHECK_EQ(msg->payload[0], 5u);            // phase=GOT_IP
    CHECK_EQ(msg->payload[1], 0u);            // errorCode LE
    CHECK_EQ(msg->payload[2], 0u);
    CHECK_EQ(msg->payload[3], 0u);            // flags
    CHECK_EQ(msg->payload[4], static_cast<uint8_t>(-60));  // rssi
    CHECK_EQ(msg->payload[5], 6u);            // channel
    // ip 网络序（10.0.0.2 → 0A 00 00 02）。
    CHECK_EQ(msg->payload[6], 0x0Au);
    CHECK_EQ(msg->payload[7], 0x00u);
    CHECK_EQ(msg->payload[8], 0x00u);
    CHECK_EQ(msg->payload[9], 0x02u);
    // serverIp 网络序（192.168.1.100 → C0 A8 01 64）。
    CHECK_EQ(msg->payload[10], 0xC0u);
    CHECK_EQ(msg->payload[11], 0xA8u);
    CHECK_EQ(msg->payload[12], 0x01u);
    CHECK_EQ(msg->payload[13], 0x64u);
    // serverPort LE。
    CHECK_EQ(msg->payload[14], 8765u & 0xFFu);
    CHECK_EQ(msg->payload[15], (8765u >> 8) & 0xFFu);
    // ssidLen + ssid。
    CHECK_EQ(msg->payload[16], 7u);
    CHECK(std::string(reinterpret_cast<const char*>(msg->payload.data() + 17), 7) ==
          std::string("HomeNet"));

    // 最大 32 字节 ssid → 49B。
    const auto maxMsg = makeWifiStatus(9, 0, 0, -128, 0, 0, 0, 0, std::string(32, 's'));
    CHECK(maxMsg.has_value());
    CHECK_EQ(maxMsg->payload.size(), kWifiStatusMaxPayload);

    // 空 ssid → 17B。
    const auto noSsid = makeWifiStatus(0, 0, 0, -128, 0, 0, 0, 0, "");
    CHECK(noSsid.has_value());
    CHECK_EQ(noSsid->payload.size(), 17u);
    CHECK_EQ(noSsid->payload[16], 0u);
}

void testStatusValidation() {
    CHECK(!makeWifiStatus(10, 0, 0, -128, 0, 0, 0, 0, "").has_value());  // phase 越界
    CHECK(!makeWifiStatus(0, 0, 0, -128, 0, 0, 0, 0, std::string(33, 's')).has_value());
    // 错误码 5..12 与 0..4 均可表达（u16 字段）。
    const auto e = makeWifiStatus(8, static_cast<uint16_t>(ErrorCode::kAuthFailed), 0,
                                  -128, 0, 0, 0, 0, "");
    CHECK(e.has_value());
    CHECK_EQ(e->payload[1], 6u);
}

void testStatusParse() {
    const auto msg = makeWifiStatus(5, static_cast<uint16_t>(ErrorCode::kNone), 0, -58, 11,
                                    kIp10_0_0_2, kIp192_168_1_100, 9999, "WLAN-5G");
    CHECK(msg.has_value());
    WifiStatusInfo out;
    CHECK(parseWifiStatus(BytesView(msg->payload.data(), msg->payload.size()), out));
    CHECK_EQ(out.phase, 5u);
    CHECK_EQ(out.errorCode, 0u);
    CHECK_EQ(out.flags, 0u);
    CHECK_EQ(out.rssi, -58);
    CHECK_EQ(out.channel, 11u);
    CHECK_EQ(out.ip, kIp10_0_0_2);
    CHECK_EQ(out.serverIp, kIp192_168_1_100);
    CHECK_EQ(out.serverPort, 9999u);
    CHECK_EQ(out.ssidLen, 7u);
    CHECK(out.ssid == std::string("WLAN-5G"));

    // 短包拒绝 / ssidLen 越界拒绝。
    std::vector<uint8_t> shortP = msg->payload;
    shortP.pop_back();
    CHECK(!parseWifiStatus(BytesView(shortP), out));
    std::vector<uint8_t> badLen(17 + 5, 0);
    badLen[16] = 33;
    CHECK(!parseWifiStatus(BytesView(badLen), out));
    // 尾部多余字节忽略。
    std::vector<uint8_t> longP = msg->payload;
    longP.push_back(0xEE);
    WifiStatusInfo out2;
    CHECK(parseWifiStatus(BytesView(longP), out2));
    CHECK_EQ(out2.phase, 5u);
}

// ---- 5. Endpoint 分发 / 发送 / ACK_REQ ----

struct FakeClock {
    uint64_t now = 0;
    uint64_t operator()() { return now; }
};

struct WifiHarness {
    FakeClock clock;
    WifiHarness* peer = nullptr;
    std::vector<uint8_t> rx;
    std::vector<SessionState> states;
    std::vector<SessionError> protoErrors;
    std::vector<WifiScanResultInfo> scanResults;
    std::vector<WifiStatusInfo> statuses;
    std::vector<std::pair<uint8_t, uint16_t>> ackRequests;  // (type, ackSeq)
    std::unique_ptr<ProtocolEndpoint> ep;

    void init(WifiHarness* p) {
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
        cb.onWifiScanResult = [this](const WifiScanResultInfo& r) {
            scanResults.push_back(r);
        };
        cb.onWifiStatus = [this](const WifiStatusInfo& st) { statuses.push_back(st); };
        cb.onAckRequest = [this](uint8_t type, const std::vector<uint8_t>&, uint16_t ackSeq) {
            ackRequests.emplace_back(type, ackSeq);
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
    void pushTo(WifiHarness& to, const Message& msg) {
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

void connectPair(WifiHarness& a, WifiHarness& b) {
    a.ep->onTransportConnected();
    b.ep->onTransportConnected();
    a.pump();
    b.pump();
    CHECK_EQ(a.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
}

void runEndpointDispatch() {
    WifiHarness a;
    WifiHarness b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // 合法 WIFI_SCAN_RESULT：B 收到回调，会话保持 CONNECTED。
    Feeder feed;
    const WifiScanRecordInfo recs[] = {makeRec("NetA", -50, 1, 3)};
    const auto msg = makeWifiScanResult(1, false, 5, recs, 1);
    CHECK(msg.has_value());
    feed.pushTo(b, *msg);
    b.pump();
    CHECK_EQ(b.scanResults.size(), 1u);
    CHECK_EQ(b.scanResults[0].scanSeq, 1u);
    CHECK_EQ(b.scanResults[0].total, 5u);
    CHECK_EQ(b.scanResults[0].records.size(), 1u);
    CHECK(b.scanResults[0].records[0].ssid == std::string("NetA"));
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    CHECK_EQ(b.ep->stats().rxWifiScanResult, 1u);
    CHECK_EQ(b.ep->stats().wifiScanResultDropped, 0u);

    // 非法 SCAN_RESULT（count>64）：仅计数丢弃，不 failSession。
    std::vector<uint8_t> badScan(5 + 42, 0);
    badScan[1] = 65;
    feed.pushTo(b, makeMessage(static_cast<uint8_t>(MessageType::kWifiScanResult), 0,
                               badScan));
    b.pump();
    CHECK_EQ(b.scanResults.size(), 1u);
    CHECK_EQ(b.ep->stats().wifiScanResultDropped, 1u);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);

    // 合法 WIFI_STATUS：B 收到回调。
    const auto st = makeWifiStatus(5, 0, 0, -60, 6, kIp10_0_0_2, kIp192_168_1_100, 8765,
                                   "NetA");
    CHECK(st.has_value());
    feed.pushTo(b, *st);
    b.pump();
    CHECK_EQ(b.statuses.size(), 1u);
    CHECK_EQ(b.statuses[0].phase, 5u);
    CHECK_EQ(b.statuses[0].ip, kIp10_0_0_2);
    CHECK_EQ(b.ep->stats().rxWifiStatus, 1u);

    // 非法 WIFI_STATUS（短包）：计数丢弃，不 failSession。
    std::vector<uint8_t> badStatus(16, 0);
    feed.pushTo(b, makeMessage(static_cast<uint8_t>(MessageType::kWifiStatus), 0,
                               badStatus));
    b.pump();
    CHECK_EQ(b.statuses.size(), 1u);
    CHECK_EQ(b.ep->stats().wifiStatusDropped, 1u);
    CHECK_EQ(b.protoErrors.size(), 0u);
}

void runEndpointSend() {
    WifiHarness a;
    WifiHarness b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // sendWifiScanResult：fire-and-forget 成功，对端收到。
    WifiScanResultInfo info;
    info.scanSeq = 2;
    info.total = 3;
    info.records.push_back(makeRec("NetB", -55, 1, 3));
    CHECK_EQ(a.ep->sendWifiScanResult(info), SendResult::kOk);
    CHECK_EQ(a.ep->stats().txWifiScanResult, 1u);
    b.pump();
    CHECK_EQ(b.scanResults.size(), 1u);
    CHECK_EQ(b.scanResults[0].scanSeq, 2u);

    // sendWifiStatus：fire-and-forget 成功。
    WifiStatusInfo st;
    st.phase = 3;
    st.rssi = -70;
    st.channel = 1;
    CHECK_EQ(a.ep->sendWifiStatus(st), SendResult::kOk);
    CHECK_EQ(a.ep->stats().txWifiStatus, 1u);
    b.pump();
    CHECK_EQ(b.statuses.size(), 1u);
    CHECK_EQ(b.statuses[0].phase, 3u);

    // 违规输入 → kInvalidMessage。
    WifiScanResultInfo badInfo;
    WifiScanRecordInfo badRec;
    badRec.authmode = 9;
    badInfo.records.push_back(badRec);
    CHECK_EQ(a.ep->sendWifiScanResult(badInfo), SendResult::kInvalidMessage);

    // 未连接时拒绝（kNotConnected）。
    WifiHarness standalone;
    standalone.init(nullptr);
    WifiStatusInfo st2;
    CHECK_EQ(standalone.ep->sendWifiStatus(st2), SendResult::kNotConnected);
    CHECK_EQ(standalone.ep->sendWifiScanResult(info), SendResult::kNotConnected);
}

void runAckReqDispatch() {
    WifiHarness a;
    WifiHarness b;
    a.init(&b);
    b.init(&a);
    connectPair(a, b);

    // WIFI_SCAN_REQ（ACK_REQ）→ B 的 onAckRequest(type=0x06)。
    Feeder feed;
    const auto scanReq = makeWifiScanReq(0, 32);
    CHECK(scanReq.has_value());
    feed.pushTo(b, *scanReq);
    b.pump();
    CHECK_EQ(b.ackRequests.size(), 1u);
    CHECK_EQ(b.ackRequests[0].first, static_cast<uint8_t>(MessageType::kWifiScanReq));
    CHECK_EQ(b.protoErrors.size(), 0u);
    // 应用回 ACK → a 收到（ackReceived 计数）。
    b.ep->acknowledge(b.ackRequests[0].second, 0, ErrorCode::kNone);
    a.pump();
    CHECK_EQ(a.ep->stats().ackReceived, 1u);
    CHECK_EQ(b.ep->stats().ackSent, 1u);

    // WIFI_CONFIG（ACK_REQ）→ B 的 onAckRequest(type=0x08)。
    const auto cfg = makeWifiConfig("NetC", "password123", kIp192_168_1_100, 8765);
    CHECK(cfg.has_value());
    feed.pushTo(b, *cfg);
    b.pump();
    CHECK_EQ(b.ackRequests.size(), 2u);
    CHECK_EQ(b.ackRequests[1].first, static_cast<uint8_t>(MessageType::kWifiConfig));
    b.ep->acknowledge(b.ackRequests[1].second, 0, ErrorCode::kNone);
    a.pump();
    CHECK_EQ(a.ep->stats().ackReceived, 2u);

    // M8-A1 ACK_REQ 白名单：whitelist 外类型（INPUT_TOUCH）携带 ACK_REQ →
    // Encoder 拒绝（kInvalidAckReq）；RX 侧收到此类包 → 忽略 + 计数
    // （不回 ACK、不发任何 wire 错误、不 failSession）。
    const auto unknown = makeMessage(static_cast<uint8_t>(MessageType::kInputTouch), kFlagAckReq,
                                        std::vector<uint8_t>(1, 0));
    {
        SequenceCounter encSeq;
        MessageEncoder enc(encSeq);
        std::vector<std::vector<uint8_t>> pkts;
        CHECK_EQ(enc.encode(unknown, pkts), PacketError::kInvalidAckReq);
    }
    // RX 侧手工构造 whitelist 外 ACK_REQ 包（encodePacket 不做 ACK_REQ 校验）：
    // seq=2 与 feed 的后续序号一致（避免被 seq 规则丢弃）。
    const espview::proto::PacketHeader rh =
        espview::proto::makeHeader(static_cast<uint8_t>(MessageType::kInputTouch), kFlagAckReq,
                                   2, 1);
    std::array<uint8_t, espview::proto::kPacketHeaderSize + 1> raw{};
    size_t rawLen = 0;
    CHECK_EQ(espview::proto::encodePacket(rh, unknown.payload.data(), unknown.payload.size(),
                                          raw.data(), raw.size(), &rawLen),
             PacketError::kNone);
    b.rx.insert(b.rx.end(), raw.begin(), raw.begin() + rawLen);
    b.pump();
    CHECK_EQ(b.ackRequests.size(), 2u);  // 不再触发 onAckRequest
    CHECK_EQ(b.ep->stats().invalidAckReq, 1u);
    CHECK_EQ(b.protoErrors.size(), 0u);
    CHECK_EQ(b.ep->state(), SessionState::kConnected);
    a.pump();
    CHECK_EQ(a.ep->stats().ackReceived, 2u);  // 未回 ACK

    // WIFI_SCAN_RESULT / WIFI_STATUS 不带 ACK_REQ（fire-and-forget）→ 不触发
    // onAckRequest，直接走数据回调。
    CHECK_EQ(b.ackRequests.size(), 2u);
}

}  // namespace

void runWifiProvisioningTests() {
    testScanReqLayout();
    testScanReqValidation();
    testScanReqParse();
    testConfigLayout();
    testConfigValidation();
    testConfigClear();
    testConfigParse();
    testScanResultLayout();
    testScanResultValidation();
    testScanResultParse();
    testStatusLayout();
    testStatusValidation();
    testStatusParse();
    runEndpointDispatch();
    runEndpointSend();
    runAckReqDispatch();
}
