// Stream Decoder 单元测试（M0-B2）。
// 规范来源：docs/DESIGN.md E 节「字节流解码状态机 / 三层概念」+ M0-B2 阶段要求。
// 覆盖：半包/粘包/逐字节 feed/伪 MAGIC 重同步/CRC 失败重同步/SEQ 连续性/CHUNKED 组装/
//       超时回 SYNC/Encoder→Decoder 逐字节 round-trip。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "test_util.h"

namespace {

using espview::proto::DecoderError;
using espview::proto::DisplayMode;
using espview::proto::FrameType;
using espview::proto::kMaxMessagePayload;
using espview::proto::kFlagChunked;
using espview::proto::kMaxPacketPayload;
using espview::proto::kPacketHeaderSize;
using espview::proto::kProtocolVersion;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeFrameRect;
using espview::proto::makeHello;
using espview::proto::makeInputKey;
using espview::proto::makeInputMouse;
using espview::proto::makeMessage;
using espview::proto::makePing;
using espview::proto::makePong;
using espview::proto::makeSetMode;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
using espview::proto::encodePacket;
using espview::proto::makeHeader;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::SequenceCounter;
using espview::proto::StreamDecoder;
using espview::proto::toString;

// 收集回调输出，供断言使用。
struct Collector {
    std::vector<Message> messages;
    std::vector<PacketHeader> packets;
    std::vector<DecoderError> errors;

    void onMessage(const Message& m) { messages.push_back(m); }
    void onPacket(const PacketHeader& h, const uint8_t*, size_t) { packets.push_back(h); }
    void onError(DecoderError e) { errors.push_back(e); }
};

StreamDecoder makeDecoder(Collector& c,
                          size_t maxPayload = kMaxMessagePayload) {
    return StreamDecoder([&c](const Message& m) { c.onMessage(m); },
                         [&c](const PacketHeader& h, const uint8_t* p, size_t n) {
                             c.onPacket(h, p, n);
                         },
                         [&c](DecoderError e) { c.onError(e); }, maxPayload);
}

// 用统一 SequenceCounter 编码多条消息，返回拼接字节流；同时输出总包数。
std::vector<uint8_t> encodeStream(const std::vector<Message>& msgs, SequenceCounter& seq,
                                  size_t* packetCount) {
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> stream;
    size_t count = 0;
    for (const auto& m : msgs) {
        packets.clear();
        const PacketError err = enc.encode(m, packets);
        CHECK_EQ(err, PacketError::kNone);
        count += packets.size();
        for (const auto& p : packets) {
            stream.insert(stream.end(), p.begin(), p.end());
        }
    }
    if (packetCount != nullptr) {
        *packetCount = count;
    }
    return stream;
}

// 编码单条消息（使用给定计数器），返回拼接字节流。
std::vector<uint8_t> encodeOne(const Message& m, SequenceCounter& seq, size_t* packetCount) {
    const std::vector<Message> one{m};
    return encodeStream(one, seq, packetCount);
}

bool messageEqual(const Message& a, const Message& b) {
    return a.type == b.type && a.flags == b.flags && a.payload == b.payload;
}

bool hasError(const std::vector<DecoderError>& errors, DecoderError e) {
    return std::find(errors.begin(), errors.end(), e) != errors.end();
}

size_t countError(const std::vector<DecoderError>& errors, DecoderError e) {
    return static_cast<size_t>(std::count(errors.begin(), errors.end(), e));
}

bool anyMessageWithPayload(const Collector& c, const std::vector<uint8_t>& payload) {
    for (const auto& m : c.messages) {
        if (m.payload == payload) {
            return true;
        }
    }
    return false;
}

// 大 FRAME_RECT：payload = 8 + 9000 = 9008 → 3 包（4096+4096+816）。
Message bigFrameRect(uint16_t seed) {
    std::vector<uint8_t> pixels(9000);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>(seed + i * 7u);
    }
    auto m = makeFrameRect(0, 0, 4500, 1, pixels.data(), pixels.size());
    CHECK(m.has_value());
    return *m;
}

// 1. 空输入
void empty_input() {
    Collector c;
    auto dec = makeDecoder(c);
    dec.feed(nullptr, 0);
    dec.feed(std::vector<uint8_t>{});
    CHECK_EQ(c.messages.size(), 0u);
    CHECK_EQ(c.packets.size(), 0u);
    CHECK_EQ(c.errors.size(), 0u);
    CHECK_EQ(dec.bufferedBytes(), 0u);
    CHECK(!dec.assemblingMessage());
    CHECK_EQ(dec.expectedSeq(), 0u);
}

// 2. 一个完整包
void one_full_packet() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message ping = makePing(0x0102030405060708ull);
    const std::vector<uint8_t> stream = encodeOne(ping, seq, nullptr);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], ping));
    CHECK_EQ(c.packets.size(), 1u);
    CHECK_EQ(c.errors.size(), 0u);
    CHECK_EQ(dec.bufferedBytes(), 0u);
}

// 3. 逐字节 feed 与整包 feed 结果一致
void byte_at_a_time_feed() {
    SequenceCounter seq;
    std::vector<Message> msgs;
    msgs.push_back(*makeHello(kProtocolVersion, 0, 320, 240, PixelFormat::kRgb565, 0b101, "esp"));
    msgs.push_back(makeSetMode(DisplayMode::kWindow));
    msgs.push_back(bigFrameRect(3));
    msgs.push_back(makeFrameEnd(1, 1, 9000, false));
    size_t packetCount = 0;
    const std::vector<uint8_t> stream = encodeStream(msgs, seq, &packetCount);

    Collector bulk;
    auto decBulk = makeDecoder(bulk);
    decBulk.feed(stream);

    Collector perByte;
    auto decByte = makeDecoder(perByte);
    for (size_t i = 0; i < stream.size(); ++i) {
        decByte.feed(&stream[i], 1);
    }

    CHECK_EQ(perByte.messages.size(), bulk.messages.size());
    CHECK_EQ(perByte.packets.size(), packetCount);
    CHECK_EQ(perByte.errors.size(), 0u);
    CHECK_EQ(bulk.errors.size(), 0u);
    for (size_t i = 0; i < bulk.messages.size(); ++i) {
        CHECK_MSG(messageEqual(perByte.messages[i], bulk.messages[i]),
                  "byte-at-a-time message " + std::to_string(i));
    }
    CHECK_EQ(decByte.bufferedBytes(), 0u);
    CHECK_EQ(decBulk.bufferedBytes(), 0u);
}

// 4. 半包头
void split_header() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message pong = makePong(7);
    const std::vector<uint8_t> stream = encodeOne(pong, seq, nullptr);
    dec.feed(stream.data(), 10);
    CHECK_EQ(c.messages.size(), 0u);
    dec.feed(stream.data() + 10, stream.size() - 10);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], pong));
}

// 5. 半包载荷
void split_payload() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const std::vector<uint8_t> pixels(3000, 0x5A);
    auto m = makeFrameRect(0, 0, 1500, 1, pixels.data(), pixels.size());
    CHECK(m.has_value());
    const std::vector<uint8_t> stream = encodeOne(*m, seq, nullptr);
    const size_t cut = kPacketHeaderSize + 100;
    dec.feed(stream.data(), cut);
    CHECK_EQ(c.messages.size(), 0u);
    dec.feed(stream.data() + cut, stream.size() - cut);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], *m));
}

// 6. 半 CRC（包尾部逐段喂）
void split_crc() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message ping = makePing(42);
    const std::vector<uint8_t> stream = encodeOne(ping, seq, nullptr);
    dec.feed(stream.data(), 16);
    dec.feed(stream.data() + 16, 2);
    CHECK_EQ(c.messages.size(), 0u);
    dec.feed(stream.data() + 18, stream.size() - 18);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], ping));
}

// 7. 一次 feed 多个包
void multiple_packets_one_feed() {
    SequenceCounter seq;
    std::vector<Message> msgs;
    msgs.push_back(makePing(1));
    msgs.push_back(makePong(2));
    msgs.push_back(makeInputKey(0x29, 0, true));
    size_t packetCount = 0;
    const std::vector<uint8_t> stream = encodeStream(msgs, seq, &packetCount);

    Collector c;
    auto dec = makeDecoder(c);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 3u);
    CHECK_EQ(c.packets.size(), packetCount);
    for (size_t i = 0; i < msgs.size(); ++i) {
        CHECK(messageEqual(c.messages[i], msgs[i]));
    }
}

// 8. MAGIC 前有垃圾
void garbage_before_magic() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message ping = makePing(9);
    const std::vector<uint8_t> stream = encodeOne(ping, seq, nullptr);
    std::vector<uint8_t> input = {0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    input.insert(input.end(), stream.begin(), stream.end());
    dec.feed(input);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], ping));
    CHECK_EQ(c.errors.size(), 0u);  // 扫描垃圾不是协议错误
}

// 9. 垃圾中的伪 MAGIC（VERSION 非法）
void fake_magic_in_garbage() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message ping = makePing(9);
    const std::vector<uint8_t> stream = encodeOne(ping, seq, nullptr);
    // "ESPVxxxxESPVxxxx" + 合法包；两个伪 MAGIC 的 VERSION 都是 'x'(0x78) → 非法。
    std::vector<uint8_t> input;
    const char* fake = "ESPVxxxxESPVxxxx";
    input.assign(fake, fake + 16);
    input.insert(input.end(), stream.begin(), stream.end());
    dec.feed(input);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], ping));
    CHECK_EQ(countError(c.errors, DecoderError::kBadVersion), 2u);
}

// 10. payload 中出现 MAGIC 不应被当作新包
void fake_magic_in_payload() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    // PING payload 恰好是 'ESPV'（45 53 50 56）。
    const std::vector<uint8_t> magicBytes = {0x45, 0x53, 0x50, 0x56};
    const Message ping = makeMessage(static_cast<uint8_t>(MessageType::kPing), 0, magicBytes);
    const Message hello =
        *makeHello(kProtocolVersion, 0, 320, 240, PixelFormat::kRgb565, 0b101, "espv2");
    std::vector<Message> msgs{ping, hello};
    size_t packetCount = 0;
    const std::vector<uint8_t> stream = encodeStream(msgs, seq, &packetCount);

    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 2u);
    CHECK(messageEqual(c.messages[0], ping));
    CHECK(messageEqual(c.messages[1], hello));
    CHECK_EQ(c.errors.size(), 0u);
}

// 11. 非法 VERSION → 重同步后恢复
// 注：HEADER 校验失败时候选头不可信（seq 字段可能也是垃圾），seq 基线不动；
// 因此紧随其后的 seq=1 合法包被判为跳变丢弃，seq=2 的包恢复（自愈）。
void invalid_version() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq0(0);
    const Message bad = makePing(11);
    const std::vector<uint8_t> badBytes = encodeOne(bad, seq0, nullptr);
    SequenceCounter seq1(1);
    SequenceCounter seq2(2);
    const Message good1 = makePong(12);
    const Message good2 = makePong(13);
    const std::vector<uint8_t> good1Bytes = encodeOne(good1, seq1, nullptr);
    const std::vector<uint8_t> good2Bytes = encodeOne(good2, seq2, nullptr);

    std::vector<uint8_t> input = badBytes;
    input[4] = 0x02;  // VERSION 非法
    input.insert(input.end(), good1Bytes.begin(), good1Bytes.end());
    input.insert(input.end(), good2Bytes.begin(), good2Bytes.end());
    dec.feed(input);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], good2));
    CHECK(hasError(c.errors, DecoderError::kBadVersion));
    CHECK(hasError(c.errors, DecoderError::kSequenceGap));
}

// 12. 非法 TYPE → 重同步后恢复（同 invalid_version 的 seq 自愈语义）
void invalid_type() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq0(0);
    const Message bad = makePing(13);
    const std::vector<uint8_t> badBytes = encodeOne(bad, seq0, nullptr);
    SequenceCounter seq1(1);
    SequenceCounter seq2(2);
    const Message good1 = makePong(14);
    const Message good2 = makePong(15);
    const std::vector<uint8_t> good1Bytes = encodeOne(good1, seq1, nullptr);
    const std::vector<uint8_t> good2Bytes = encodeOne(good2, seq2, nullptr);

    std::vector<uint8_t> input = badBytes;
    input[5] = 0x00;  // TYPE 越界
    input.insert(input.end(), good1Bytes.begin(), good1Bytes.end());
    input.insert(input.end(), good2Bytes.begin(), good2Bytes.end());
    dec.feed(input);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], good2));
    CHECK(hasError(c.errors, DecoderError::kBadType));
    CHECK(hasError(c.errors, DecoderError::kSequenceGap));
}

// 13. 非法 LENGTH（>4096）→ 重同步后恢复（同 invalid_version 的 seq 自愈语义）
void invalid_length() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq0(0);
    const Message bad = makePing(15);
    const std::vector<uint8_t> badBytes = encodeOne(bad, seq0, nullptr);
    SequenceCounter seq1(1);
    SequenceCounter seq2(2);
    const Message good1 = makePong(16);
    const Message good2 = makePong(17);
    const std::vector<uint8_t> good1Bytes = encodeOne(good1, seq1, nullptr);
    const std::vector<uint8_t> good2Bytes = encodeOne(good2, seq2, nullptr);

    std::vector<uint8_t> input = badBytes;
    input[10] = 0xFF;
    input[11] = 0xFF;
    input[12] = 0xFF;
    input[13] = 0xFF;  // LENGTH = 0xFFFFFFFF > 4096
    input.insert(input.end(), good1Bytes.begin(), good1Bytes.end());
    input.insert(input.end(), good2Bytes.begin(), good2Bytes.end());
    dec.feed(input);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], good2));
    CHECK(hasError(c.errors, DecoderError::kBadLength));
    CHECK(hasError(c.errors, DecoderError::kSequenceGap));
}

// 14+15. CRC 错误：整包丢弃，错误包消息不派发；随后合法包恢复
void crc_error_then_valid() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message bad = makePing(17);
    const Message good = makePong(18);
    const std::vector<uint8_t> badBytes = encodeOne(bad, seq, nullptr);
    const std::vector<uint8_t> goodBytes = encodeOne(good, seq, nullptr);
    std::vector<uint8_t> input = badBytes;
    input[20 + 1] ^= 0xFF;  // 破坏 payload → CRC 失败
    input.insert(input.end(), goodBytes.begin(), goodBytes.end());
    dec.feed(input);
    // 错误包（ping）不派发；合法包（pong）恢复。
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], good));
    CHECK(hasError(c.errors, DecoderError::kCrcMismatch));
    // 错误包未通过 CRC，不触发 PacketCallback。
    CHECK_EQ(c.packets.size(), 1u);
}

// 16. 正常连续 SEQ
void normal_sequence() {
    SequenceCounter seq;
    std::vector<Message> msgs{makePing(1), makePong(2), makeInputMouse(1, 10, 20, 0, 1)};
    const std::vector<uint8_t> stream = encodeStream(msgs, seq, nullptr);
    Collector c;
    auto dec = makeDecoder(c);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 3u);
    CHECK_EQ(c.errors.size(), 0u);
}

// 17. SEQ 回绕 65535 → 0
// 注：接收端初始基线为 0（DESIGN.md：握手后 seq 清零），流从 65534 开始意味着
// 首包相对基线跳变（丢弃并重定位基线到 65535）；此后 65535→0→1→2 均按回绕正常接受。
void seq_wrap_65535_to_0() {
    SequenceCounter seq(65534);
    std::vector<Message> msgs{makePing(1), makePong(2), makePing(3), makePong(4), makePing(5)};
    const std::vector<uint8_t> stream = encodeStream(msgs, seq, nullptr);  // 65534,65535,0,1,2
    Collector c;
    auto dec = makeDecoder(c);
    dec.feed(stream);
    // 65534 首包跳变丢弃；65535,0,1,2 全部接受。
    CHECK_EQ(c.messages.size(), 4u);
    CHECK_EQ(countError(c.errors, DecoderError::kSequenceGap), 1u);
    CHECK_EQ(c.packets.size(), 5u);  // 跳变包仍通过 CRC → PacketCallback 可见
    CHECK_EQ(dec.expectedSeq(), 3u);  // 2 之后 +1；0xFFFF+1 回绕为 0 已在上一步验证
}

// 18. SEQ 跳变：当前包丢弃、基线重定位、后续恢复
void sequence_gap() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq0(0);   // m0: seq 0
    SequenceCounter seq5(5);   // m1: seq 5（跳变）
    SequenceCounter seq6(6);   // m2: seq 6（新基线）
    const Message m0 = makePing(21);
    const Message m1 = makePong(22);
    const Message m2 = makeInputKey(0x29, 0, true);
    const std::vector<uint8_t> b0 = encodeOne(m0, seq0, nullptr);
    const std::vector<uint8_t> b1 = encodeOne(m1, seq5, nullptr);
    const std::vector<uint8_t> b2 = encodeOne(m2, seq6, nullptr);

    std::vector<uint8_t> input = b0;
    input.insert(input.end(), b1.begin(), b1.end());
    input.insert(input.end(), b2.begin(), b2.end());
    dec.feed(input);
    CHECK_EQ(c.messages.size(), 2u);
    CHECK(messageEqual(c.messages[0], m0));
    CHECK(messageEqual(c.messages[1], m2));  // m1 因 seq 跳变被丢弃
    CHECK_EQ(countError(c.errors, DecoderError::kSequenceGap), 1u);
    // 跳变包仍通过 CRC → PacketCallback 可见。
    CHECK_EQ(c.packets.size(), 3u);
}

// 19+26. CHUNKED 组装中途 SEQ 跳变：消息作废
void seq_gap_during_chunked() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    const Message rect = bigFrameRect(5);  // 3 包：seq 0,1,2
    size_t rectPackets = 0;
    const std::vector<uint8_t> rectBytes = encodeOne(rect, seq, &rectPackets);
    CHECK_EQ(rectPackets, 3u);
    SequenceCounter seq9(9);
    SequenceCounter seq10(10);
    const Message gapPing = makePing(31);
    const Message okPing = makePong(32);
    const std::vector<uint8_t> gapBytes = encodeOne(gapPing, seq9, nullptr);
    const std::vector<uint8_t> okBytes = encodeOne(okPing, seq10, nullptr);

    // 只喂前两个 CHUNKED 包，然后插入 seq=9 的包（跳变），再喂恢复包。
    dec.feed(rectBytes.data(), kPacketHeaderSize + 4096);               // 包0
    dec.feed(rectBytes.data() + kPacketHeaderSize + 4096, kPacketHeaderSize + 4096);  // 包1
    CHECK(dec.assemblingMessage());
    dec.feed(gapBytes);
    CHECK(!dec.assemblingMessage());  // 消息已作废
    dec.feed(okBytes);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], okPing));  // 完整 RECT 未派发
    CHECK(!anyMessageWithPayload(c, rect.payload));
    CHECK_EQ(countError(c.errors, DecoderError::kSequenceGap), 1u);
}

// 20. 单包消息（FRAME_END）→ 一条消息
void single_packet_message() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message end = makeFrameEnd(7, 3, 12345, false);
    const std::vector<uint8_t> stream = encodeOne(end, seq, nullptr);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], end));
}

// 21. 两包消息（payload 4097 = 4096 + 1）
void two_packet_message() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const std::vector<uint8_t> payload(kMaxPacketPayload + 1, 0xAB);
    const Message m =
        makeMessage(static_cast<uint8_t>(MessageType::kFrameRect), 0, payload);
    size_t packetCount = 0;
    const std::vector<uint8_t> stream = encodeOne(m, seq, &packetCount);
    CHECK_EQ(packetCount, 2u);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], m));
    CHECK_EQ(c.packets.size(), 2u);
    CHECK_EQ(c.errors.size(), 0u);
}

// 22+23. 三包 FRAME_RECT 载荷重建（逐字节一致，无截断/重排/重复/丢失）
void three_packet_frame_rect() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message rect = bigFrameRect(9);  // payload 9008 → 3 包
    size_t packetCount = 0;
    const std::vector<uint8_t> stream = encodeOne(rect, seq, &packetCount);
    CHECK_EQ(packetCount, 3u);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], rect));
    CHECK_EQ(rect.payload.size(), 8u + 9000u);
    CHECK(std::memcmp(c.messages[0].payload.data(), rect.payload.data(), rect.payload.size()) == 0);
}

// 24. 组装中收到 CHUNKED=1 但 TYPE 不同：消息作废，新类型重新组装
void chunk_type_mismatch() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    const Message rect = bigFrameRect(11);  // 3 包 seq 0,1,2
    const std::vector<uint8_t> rectBytes = encodeOne(rect, seq, nullptr);
    // 非法但类型不同的 5000 字节消息（TYPE=INPUT_KEY）→ 2 包（CH=1, CH=0）。
    // 插入位置在 rect 包1（seq1）之后，因此用 seq=2 起的新计数器编码。
    SequenceCounter intrSeq(2);
    const std::vector<uint8_t> intrPayload(5000, 0x77);
    const Message intr = makeMessage(static_cast<uint8_t>(MessageType::kInputKey), 0, intrPayload);
    const std::vector<uint8_t> intrBytes = encodeOne(intr, intrSeq, nullptr);

    // rect 包0、包1（CH=1）→ intr 包0（CH=1, TYPE 不同）→ intr 包1（CH=0）
    dec.feed(rectBytes.data(), kPacketHeaderSize + 4096);
    dec.feed(rectBytes.data() + kPacketHeaderSize + 4096, kPacketHeaderSize + 4096);
    dec.feed(intrBytes.data(), kPacketHeaderSize + 4096);
    dec.feed(intrBytes.data() + kPacketHeaderSize + 4096, intrBytes.size() - (kPacketHeaderSize + 4096));

    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], intr));  // 新类型消息完整恢复
    CHECK(!anyMessageWithPayload(c, rect.payload));
    CHECK_EQ(countError(c.errors, DecoderError::kChunkTypeMismatch), 1u);
}

// 25. CHUNKED 中途 CRC 错误：整条消息作废
void crc_error_inside_chunked() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    const Message rect = bigFrameRect(13);  // 3 包 seq 0,1,2
    const std::vector<uint8_t> rectBytes = encodeOne(rect, seq, nullptr);

    // 破坏包1的 payload 一个字节（CRC 失败）。
    std::vector<uint8_t> badP1 = std::vector<uint8_t>(
        rectBytes.begin() + kPacketHeaderSize + 4096,
        rectBytes.begin() + (kPacketHeaderSize + 4096) * 2);
    badP1[kPacketHeaderSize + 5] ^= 0xFF;

    dec.feed(rectBytes.data(), kPacketHeaderSize + 4096);               // 包0 CH=1
    dec.feed(badP1);                                                    // 包1 CRC 失败
    CHECK(!dec.assemblingMessage());
    dec.feed(rectBytes.data() + (kPacketHeaderSize + 4096) * 2,         // 包2（末包）
             rectBytes.size() - (kPacketHeaderSize + 4096) * 2);
    CHECK_EQ(countError(c.errors, DecoderError::kCrcMismatch), 1u);
    // 完整 RECT 未派发；包2 作为独立消息派发（仅其自身载荷）。
    CHECK(!anyMessageWithPayload(c, rect.payload));
    CHECK_EQ(c.messages.size(), 1u);
    CHECK_EQ(c.messages[0].payload.size(), rectBytes.size() - (kPacketHeaderSize + 4096) * 2 - kPacketHeaderSize);
}

// 27. 控制消息非法插入 CHUNKED 序列：chunked 消息作废，控制消息派发
void control_inserted_into_chunked() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    const Message rect = bigFrameRect(15);  // 3 包 seq 0,1,2
    const std::vector<uint8_t> rectBytes = encodeOne(rect, seq, nullptr);
    // 控制消息按“插入位置”编码：位于 rect 包1（seq1）之后 → seq=2。
    SequenceCounter seq2(2);
    const Message ping = makePing(100);
    const std::vector<uint8_t> pingBytes = encodeOne(ping, seq2, nullptr);

    dec.feed(rectBytes.data(), kPacketHeaderSize + 4096);               // 包0 CH=1（seq0）
    dec.feed(rectBytes.data() + kPacketHeaderSize + 4096, kPacketHeaderSize + 4096);  // 包1 CH=1（seq1）
    dec.feed(pingBytes);                                                // PING（seq2）插入

    // PING 被派发；完整 RECT 未派发。
    CHECK_MSG(anyMessageWithPayload(c, ping.payload), "PING must be dispatched");
    CHECK(!anyMessageWithPayload(c, rect.payload));
    CHECK_EQ(countError(c.errors, DecoderError::kChunkViolation), 1u);
    CHECK_EQ(c.messages.size(), 1u);
}

// 28. 半包滞留 + 超时：回 SYNC 后重新喂完整包可恢复；超时不改 seq 基线
void partial_packet_timeout() {
    Collector c;
    auto dec = makeDecoder(c);

    // 阶段 A：半包（seq0 完整包）→ 超时 → 重新喂完整包，恢复。
    SequenceCounter seq0(0);
    const Message ping = makePing(200);
    const std::vector<uint8_t> pingBytes = encodeOne(ping, seq0, nullptr);
    dec.feed(pingBytes.data(), 7);
    CHECK_EQ(dec.bufferedBytes(), 7u);
    dec.onTimeout();
    CHECK_EQ(dec.bufferedBytes(), 0u);
    CHECK(!dec.assemblingMessage());
    dec.feed(pingBytes);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], ping));

    // 阶段 B：接受 seq0 后基线为 1；半包 seq1 → 超时 → 完整包 seq1 仍被接受。
    SequenceCounter seq1(1);
    const Message pong = makePong(201);
    const std::vector<uint8_t> pongBytes = encodeOne(pong, seq1, nullptr);
    dec.feed(pongBytes.data(), 9);
    dec.onTimeout();
    dec.feed(pongBytes);
    CHECK_EQ(c.messages.size(), 2u);
    CHECK(messageEqual(c.messages[1], pong));
    CHECK_EQ(c.errors.size(), 0u);
}

// 29. 部分 CHUNKED Message + 超时：消息作废，之后可恢复
void partial_chunked_timeout() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq(0);
    const Message rect = bigFrameRect(17);  // 3 包 seq 0,1,2
    const std::vector<uint8_t> rectBytes = encodeOne(rect, seq, nullptr);

    // 包0 + 包1 的前 50 字节。
    dec.feed(rectBytes.data(), kPacketHeaderSize + 4096);
    dec.feed(rectBytes.data() + kPacketHeaderSize + 4096, 50);
    CHECK(dec.assemblingMessage());
    CHECK(dec.bufferedBytes() > 0u);

    dec.onTimeout();
    CHECK(!dec.assemblingMessage());
    CHECK_EQ(dec.bufferedBytes(), 0u);

    // 超时后 seq 基线保持为 1（包0 已消费）；喂 seq=1 的 PONG 恢复。
    SequenceCounter seq1(1);
    const Message ok = makePong(300);
    const std::vector<uint8_t> okBytes = encodeOne(ok, seq1, nullptr);
    dec.feed(okBytes);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], ok));
    CHECK(!anyMessageWithPayload(c, rect.payload));
}

// 30. Encoder → Decoder 逐字节 round-trip（TYPE/FLAGS/payload 全一致）
void encoder_decoder_roundtrip() {
    std::vector<Message> msgs;
    msgs.push_back(*makeHello(kProtocolVersion, 0, 320, 240, PixelFormat::kRgb565, 0b101,
                              "espview-demo"));
    msgs.push_back(makeSetMode(DisplayMode::kWindow));
    msgs.push_back(makePing(12345));
    msgs.push_back(makePong(67890));
    msgs.push_back(makeInputKey(0x29, 0, true));
    msgs.push_back(makeInputMouse(1, 120, 80, 0, 1));
    msgs.push_back(*makeFrameBegin(1, FrameType::kFull, PixelFormat::kRgb565, 320, 240, 153600));
    std::vector<uint8_t> small(64 * 32 * 2);
    for (size_t i = 0; i < small.size(); ++i) {
        small[i] = static_cast<uint8_t>(i);
    }
    msgs.push_back(*makeFrameRect(0, 0, 64, 32, small.data(), small.size()));
    msgs.push_back(bigFrameRect(19));
    msgs.push_back(makeFrameEnd(1, 2, 153600, false));

    SequenceCounter seq;
    size_t packetCount = 0;
    const std::vector<uint8_t> stream = encodeStream(msgs, seq, &packetCount);

    Collector c;
    auto dec = makeDecoder(c);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), msgs.size());
    CHECK_EQ(c.packets.size(), packetCount);
    CHECK_EQ(c.errors.size(), 0u);
    for (size_t i = 0; i < msgs.size(); ++i) {
        CHECK_MSG(messageEqual(c.messages[i], msgs[i]), "roundtrip message " + std::to_string(i));
    }
    CHECK_EQ(dec.bufferedBytes(), 0u);
}

// 附加：伪 MAGIC 链（ESPVxxxxESPVxxxxESPV）不能跳过真正的 MAGIC
void resync_after_fake_magic_chain() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message ping = makePing(400);
    const std::vector<uint8_t> stream = encodeOne(ping, seq, nullptr);
    std::vector<uint8_t> input;
    const char* chain = "ESPVxxxxESPVxxxxESPV";  // 20 字节：两个伪 MAGIC + 真实 MAGIC 前缀
    input.assign(chain, chain + 20);
    input.insert(input.end(), stream.begin() + 4, stream.end());  // 拼接真实 MAGIC 之后的部分
    dec.feed(input);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], ping));
    CHECK_EQ(countError(c.errors, DecoderError::kBadVersion), 2u);
}

// 手工构造 CHUNKED 消息字节流（Encoder 已按 MAX_MESSAGE_PAYLOAD 拒绝超限消息，
// 因此默认上限测试直接用 Packet API 构造 >1 MiB 的合法包序列）。
std::vector<uint8_t> buildChunkedStream(size_t totalPayload, uint16_t startSeq) {
    std::vector<uint8_t> stream;
    size_t offset = 0;
    uint16_t seq = startSeq;
    std::vector<uint8_t> chunk(kMaxPacketPayload, 0x33);
    while (offset < totalPayload) {
        const size_t n = std::min<size_t>(kMaxPacketPayload, totalPayload - offset);
        const bool last = (offset + n == totalPayload);
        const uint8_t flags = last ? 0u : kFlagChunked;
        const PacketHeader h =
            makeHeader(static_cast<uint8_t>(MessageType::kFrameRect), flags, seq++,
                       static_cast<uint32_t>(n));
        std::vector<uint8_t> buf(kPacketHeaderSize + n);
        size_t written = 0;
        const PacketError err =
            encodePacket(h, chunk.data(), n, buf.data(), buf.size(), &written);
        CHECK_EQ(err, PacketError::kNone);
        stream.insert(stream.end(), buf.begin(), buf.end());
        offset += n;
    }
    return stream;
}
// 附加：Message payload 上限防护
void message_too_large() {
    constexpr size_t kCap = 5000;
    Collector c;
    auto dec = makeDecoder(c, kCap);
    SequenceCounter seq;
    const std::vector<uint8_t> payload(kCap + 1008, 0x11);  // 6008 → 2 包
    const Message m =
        makeMessage(static_cast<uint8_t>(MessageType::kFrameRect), 0, payload);
    size_t packetCount = 0;
    const std::vector<uint8_t> stream = encodeOne(m, seq, &packetCount);
    CHECK_EQ(packetCount, 2u);
    dec.feed(stream);
    CHECK_EQ(c.messages.size(), 0u);
    CHECK_EQ(countError(c.errors, DecoderError::kMessageTooLarge), 1u);
    CHECK(!dec.assemblingMessage());

    // 恰好等于上限：应能派发。
    Collector c2;
    auto dec2 = makeDecoder(c2, kCap);
    SequenceCounter seq2;
    const std::vector<uint8_t> exact(kCap, 0x22);  // 5000 → 2 包（4096+904）
    const Message m2 = makeMessage(static_cast<uint8_t>(MessageType::kFrameRect), 0, exact);
    const std::vector<uint8_t> stream2 = encodeOne(m2, seq2, nullptr);
    dec2.feed(stream2);
    CHECK_EQ(c2.messages.size(), 1u);
    CHECK(messageEqual(c2.messages[0], m2));
    CHECK_EQ(c2.errors.size(), 0u);
}


// 附加：默认上限（MAX_MESSAGE_PAYLOAD = 1 MiB）拒绝与边界
void message_too_large_default_cap() {
    // 1 MiB + 1 byte → 拒绝，不派发。
    Collector c;
    auto dec = makeDecoder(c);
    const std::vector<uint8_t> over = buildChunkedStream(kMaxMessagePayload + 1, 0);
    dec.feed(over);
    CHECK_EQ(c.messages.size(), 0u);
    CHECK_EQ(countError(c.errors, DecoderError::kMessageTooLarge), 1u);
    CHECK(!dec.assemblingMessage());

    // 恰好 1 MiB（256 包）→ 正常派发。
    Collector c2;
    auto dec2 = makeDecoder(c2);
    const std::vector<uint8_t> exact = buildChunkedStream(kMaxMessagePayload, 0);
    dec2.feed(exact);
    CHECK_EQ(c2.messages.size(), 1u);
    CHECK_EQ(c2.messages[0].payload.size(), kMaxMessagePayload);
    CHECK_EQ(c2.errors.size(), 0u);
}

// 附加：reset 后从消息边界重新开始
void reset_mid_stream() {
    Collector c;
    auto dec = makeDecoder(c);
    SequenceCounter seq;
    const Message ping = makePing(500);
    const std::vector<uint8_t> stream = encodeOne(ping, seq, nullptr);

    dec.feed(stream.data(), 8);  // 半包
    dec.reset();
    CHECK_EQ(dec.bufferedBytes(), 0u);
    CHECK_EQ(dec.expectedSeq(), 0u);
    CHECK(!dec.assemblingMessage());

    SequenceCounter seq2;  // reset 后 seq 从 0 开始
    const Message pong = makePong(501);
    const std::vector<uint8_t> stream2 = encodeOne(pong, seq2, nullptr);
    dec.feed(stream2);
    CHECK_EQ(c.messages.size(), 1u);
    CHECK(messageEqual(c.messages[0], pong));
}

// 附加：feed 边界恰好在包中间（多次分割）
void feed_at_arbitrary_boundaries() {
    SequenceCounter seq;
    std::vector<Message> msgs{makePing(1), bigFrameRect(21), makePong(2)};
    const std::vector<uint8_t> stream = encodeStream(msgs, seq, nullptr);

    Collector bulk;
    auto decBulk = makeDecoder(bulk);
    decBulk.feed(stream);

    Collector split;
    auto decSplit = makeDecoder(split);
    size_t pos = 0;
    size_t step = 1;
    while (pos < stream.size()) {
        const size_t n = std::min(step, stream.size() - pos);
        decSplit.feed(&stream[pos], n);
        pos += n;
        step = (step * 2 + 3) % 37 + 1;  // 变化的步长
    }
    CHECK_EQ(split.messages.size(), bulk.messages.size());
    for (size_t i = 0; i < bulk.messages.size(); ++i) {
        CHECK_MSG(messageEqual(split.messages[i], bulk.messages[i]),
                  "split-boundary message " + std::to_string(i));
    }
    CHECK_EQ(split.errors.size(), 0u);
}

}  // namespace

void runDecoderTests() {
    std::printf("  empty_input\n");
    empty_input();
    std::printf("  one_full_packet\n");
    one_full_packet();
    std::printf("  byte_at_a_time_feed\n");
    byte_at_a_time_feed();
    std::printf("  split_header\n");
    split_header();
    std::printf("  split_payload\n");
    split_payload();
    std::printf("  split_crc\n");
    split_crc();
    std::printf("  multiple_packets_one_feed\n");
    multiple_packets_one_feed();
    std::printf("  garbage_before_magic\n");
    garbage_before_magic();
    std::printf("  fake_magic_in_garbage\n");
    fake_magic_in_garbage();
    std::printf("  fake_magic_in_payload\n");
    fake_magic_in_payload();
    std::printf("  invalid_version\n");
    invalid_version();
    std::printf("  invalid_type\n");
    invalid_type();
    std::printf("  invalid_length\n");
    invalid_length();
    std::printf("  crc_error_then_valid\n");
    crc_error_then_valid();
    std::printf("  normal_sequence\n");
    normal_sequence();
    std::printf("  seq_wrap_65535_to_0\n");
    seq_wrap_65535_to_0();
    std::printf("  sequence_gap\n");
    sequence_gap();
    std::printf("  seq_gap_during_chunked\n");
    seq_gap_during_chunked();
    std::printf("  single_packet_message\n");
    single_packet_message();
    std::printf("  two_packet_message\n");
    two_packet_message();
    std::printf("  three_packet_frame_rect\n");
    three_packet_frame_rect();
    std::printf("  chunk_type_mismatch\n");
    chunk_type_mismatch();
    std::printf("  crc_error_inside_chunked\n");
    crc_error_inside_chunked();
    std::printf("  control_inserted_into_chunked\n");
    control_inserted_into_chunked();
    std::printf("  partial_packet_timeout\n");
    partial_packet_timeout();
    std::printf("  partial_chunked_timeout\n");
    partial_chunked_timeout();
    std::printf("  encoder_decoder_roundtrip\n");
    encoder_decoder_roundtrip();
    std::printf("  resync_after_fake_magic_chain\n");
    resync_after_fake_magic_chain();
    std::printf("  message_too_large\n");
    message_too_large();
    std::printf("  reset_mid_stream\n");
    reset_mid_stream();
    std::printf("  feed_at_arbitrary_boundaries\n");
    feed_at_arbitrary_boundaries();
    std::printf("  message_too_large_default_cap\n");
    message_too_large_default_cap();
}