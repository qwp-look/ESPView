// Message Encoder 单元测试（M0-B1）。
// 规范来源：docs/DESIGN.md E 节「三层概念 / 帧消息 Payload Layout / 控制消息 Payload Layout」。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "encoder.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "test_util.h"

namespace {

using espview::proto::decodeHeader;
using espview::proto::DisplayMode;
using espview::proto::ErrorCode;
using espview::proto::FrameType;
using espview::proto::kFlagAckReq;
using espview::proto::kFlagChunked;
using espview::proto::kFrameEndFlagAborted;
using espview::proto::kMaxMessagePayload;
using espview::proto::kMaxPacketPayload;
using espview::proto::kPacketHeaderSize;
using espview::proto::kProtocolVersion;
using espview::proto::makeAck;
using espview::proto::makeError;
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
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::SequenceCounter;
using espview::proto::verifyPacketCrc;

struct DecodedPacket {
    PacketHeader header;
    std::vector<uint8_t> payload;
};

// 解码并校验一个完整包（CRC 必须通过）；失败时记录并返回空。
DecodedPacket decodePacket(const std::vector<uint8_t>& bytes, const char* label) {
    DecodedPacket dp;
    const PacketError e1 = decodeHeader(bytes.data(), bytes.size(), &dp.header);
    CHECK_MSG(e1 == PacketError::kNone, std::string(label) + ": decodeHeader " +
                                              espview::proto::toString(e1));
    if (e1 == PacketError::kNone) {
        const PacketError e2 = verifyPacketCrc(dp.header, bytes.data() + kPacketHeaderSize,
                                               dp.header.length);
        CHECK_MSG(e2 == PacketError::kNone, std::string(label) + ": verifyPacketCrc " +
                                                    espview::proto::toString(e2));
        dp.payload.assign(bytes.begin() + kPacketHeaderSize, bytes.end());
    }
    return dp;
}

// 按序解码包序列并拼接载荷，恢复完整 Message 载荷（Decoder 的关键恢复路径）。
std::vector<uint8_t> splicePackets(const std::vector<std::vector<uint8_t>>& packets,
                                   const char* label) {
    std::vector<uint8_t> spliced;
    for (size_t i = 0; i < packets.size(); ++i) {
        const DecodedPacket dp = decodePacket(packets[i], label);
        spliced.insert(spliced.end(), dp.payload.begin(), dp.payload.end());
    }
    return spliced;
}

// 1. HELLO → single packet
void hello_single_packet() {
    const std::string name = "esp32-demo";
    auto msg = makeHello(kProtocolVersion, 0, 320, 240, PixelFormat::kRgb565, 0b101, name);
    CHECK(msg.has_value());
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    const DecodedPacket dp = decodePacket(packets[0], "hello");
    CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kHello));
    CHECK_EQ(dp.header.flags, 0u);  // 小消息：无 CHUNKED
    CHECK_EQ(dp.header.length, 9u + name.size());
    CHECK_EQ(dp.payload.size(), 9u + name.size());
    CHECK_EQ(dp.payload[0], kProtocolVersion);
    CHECK_EQ(dp.payload[1], 0u);
    CHECK_EQ(dp.payload[2], 0x40);  // width=320 LE
    CHECK_EQ(dp.payload[3], 0x01);
    CHECK_EQ(dp.payload[4], 0xF0);  // height=240 LE
    CHECK_EQ(dp.payload[5], 0x00);
    CHECK_EQ(dp.payload[6], 0u);    // pixelFormat=RGB565
    CHECK_EQ(dp.payload[7], 0b101); // modeMask
    CHECK_EQ(dp.payload[8], name.size());
    CHECK(std::memcmp(dp.payload.data() + 9, name.data(), name.size()) == 0);
}

// 2. PING / PONG → single packet
void ping_pong_single_packet() {
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;

    const Message ping = makePing(0x0102030405060708ull);
    CHECK_EQ(enc.encode(ping, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    DecodedPacket dp = decodePacket(packets[0], "ping");
    CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kPing));
    CHECK_EQ(dp.header.flags, 0u);
    CHECK_EQ(dp.payload.size(), 8u);
    for (int i = 0; i < 8; ++i) {
        CHECK_EQ(dp.payload[i], static_cast<uint8_t>(0x08 - i));  // LE: 08 07 06 05 04 03 02 01
    }

    const Message pong = makePong(7);
    packets.clear();
    CHECK_EQ(enc.encode(pong, packets), PacketError::kNone);
    dp = decodePacket(packets[0], "pong");
    CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kPong));
    CHECK_EQ(dp.payload[0], 7u);
    CHECK_EQ(dp.payload[1], 0u);
}

// 3. SET_MODE + ACK_REQ
void set_mode_ack_req() {
    const Message sm = makeSetMode(DisplayMode::kWindow);
    CHECK_EQ(sm.flags, kFlagAckReq);  // DESIGN.md：SET_MODE 必须 ACK_REQ
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(sm, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    const DecodedPacket dp = decodePacket(packets[0], "set_mode");
    CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kSetMode));
    CHECK_EQ(dp.header.flags, kFlagAckReq);
    CHECK_EQ(dp.header.length, 1u);
    CHECK_EQ(dp.payload[0], static_cast<uint8_t>(DisplayMode::kWindow));
}

// 4. FRAME_BEGIN
void frame_begin() {
    auto msg = makeFrameBegin(42, FrameType::kPartial, PixelFormat::kRgb565, 320, 240, 153600);
    CHECK(msg.has_value());
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    const DecodedPacket dp = decodePacket(packets[0], "frame_begin");
    CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kFrameBegin));
    CHECK_EQ(dp.header.length, 12u);
    CHECK_EQ(dp.payload[0], 42u);  // frameId LE
    CHECK_EQ(dp.payload[1], 0u);
    CHECK_EQ(dp.payload[2], 1u);  // frameType=PARTIAL
    CHECK_EQ(dp.payload[3], 0u);  // pixelFormat=RGB565
    CHECK_EQ(dp.payload[4], 0x40);  // width=320 LE
    CHECK_EQ(dp.payload[5], 0x01);
    CHECK_EQ(dp.payload[6], 0xF0);  // height=240 LE
    CHECK_EQ(dp.payload[7], 0x00);
    // byteHint=153600=0x00025800 LE：00 58 02 00
    CHECK_EQ(dp.payload[8], 0x00);
    CHECK_EQ(dp.payload[9], 0x58);
    CHECK_EQ(dp.payload[10], 0x02);
    CHECK_EQ(dp.payload[11], 0x00);
}

// 5. FRAME_END（含 ABORTED 标志）
void frame_end() {
    const Message fe = makeFrameEnd(42, 3, 153600, true);
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(fe, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    const DecodedPacket dp = decodePacket(packets[0], "frame_end");
    CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kFrameEnd));
    CHECK_EQ(dp.header.length, 9u);
    CHECK_EQ(dp.payload[0], 42u);
    CHECK_EQ(dp.payload[1], 0u);
    CHECK_EQ(dp.payload[2], 3u);
    CHECK_EQ(dp.payload[3], 0u);
    CHECK_EQ(dp.payload[4], 0x00);  // byteCount=153600 LE
    CHECK_EQ(dp.payload[5], 0x58);
    CHECK_EQ(dp.payload[6], 0x02);
    CHECK_EQ(dp.payload[7], 0x00);
    CHECK_EQ(dp.payload[8], kFrameEndFlagAborted);
}

// 6. 最小 FRAME_RECT（1x1 → 2 像素字节）
void min_frame_rect() {
    const uint8_t pixels[] = {0x00, 0xF8};
    auto msg = makeFrameRect(0, 0, 1, 1, pixels, sizeof(pixels));
    CHECK(msg.has_value());
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    const DecodedPacket dp = decodePacket(packets[0], "min_rect");
    CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kFrameRect));
    CHECK_EQ(dp.header.flags, 0u);
    CHECK_EQ(dp.header.length, 10u);  // 8 字节矩形头 + 2 像素
    CHECK_EQ(dp.payload[0], 0u);      // x
    CHECK_EQ(dp.payload[4], 1u);      // w
    CHECK_EQ(dp.payload[8], 0x00);    // 像素
    CHECK_EQ(dp.payload[9], 0xF8);
}

// 7. FRAME_RECT 恰好 4096 payload（像素 4088 → 1 包）
void frame_rect_exact_4096() {
    const size_t pixelBytes = 4088;  // 2044x1 RGB565
    std::vector<uint8_t> pixels(pixelBytes, 0xAB);
    auto msg = makeFrameRect(0, 0, 2044, 1, pixels.data(), pixels.size());
    CHECK(msg.has_value());
    CHECK_EQ(msg->payload.size(), 4096u);
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    const DecodedPacket dp = decodePacket(packets[0], "rect_4096");
    CHECK_EQ(dp.header.length, 4096u);
    CHECK_EQ(dp.header.flags & kFlagChunked, 0u);
}

// 8. FRAME_RECT 刚好超过 4096（payload 4098 → 2 包：4096 CHUNKED=1 + 2 CHUNKED=0）
void frame_rect_over_4096() {
    const size_t pixelBytes = 4090;  // 2045x1 → payload 4098
    std::vector<uint8_t> pixels(pixelBytes, 0xCD);
    auto msg = makeFrameRect(0, 0, 2045, 1, pixels.data(), pixels.size());
    CHECK(msg.has_value());
    CHECK_EQ(msg->payload.size(), 4098u);
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 2u);
    const DecodedPacket p0 = decodePacket(packets[0], "over0");
    const DecodedPacket p1 = decodePacket(packets[1], "over1");
    CHECK_EQ(p0.header.length, 4096u);
    CHECK_EQ(p0.header.flags & kFlagChunked, kFlagChunked);
    CHECK_EQ(p1.header.length, 2u);
    CHECK_EQ(p1.header.flags & kFlagChunked, 0u);
}

// 9/10/11/12. 大 FRAME_RECT 拆多包：CHUNKED 模式、末包清除、TYPE 一致、SEQ 连续、CRC 正确。
void large_frame_rect_chunking() {
    const size_t pixelBytes = 153600;  // 320x240
    std::vector<uint8_t> pixels(pixelBytes);
    for (size_t i = 0; i < pixelBytes; ++i) {
        pixels[i] = static_cast<uint8_t>(i);
    }
    auto msg = makeFrameRect(0, 0, 320, 240, pixels.data(), pixels.size());
    CHECK(msg.has_value());
    const size_t payloadSize = msg->payload.size();  // 153608
    const size_t expectedPackets = (payloadSize + kMaxPacketPayload - 1) / kMaxPacketPayload;
    CHECK_EQ(expectedPackets, 38u);

    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), expectedPackets);

    uint16_t expectedSeq = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        const DecodedPacket dp = decodePacket(packets[i], "large_rect");
        CHECK_EQ(dp.header.type, static_cast<uint8_t>(MessageType::kFrameRect));  // 12. TYPE 一致
        CHECK_EQ(dp.header.seq, expectedSeq++);                                   // 13. SEQ 连续
        const bool last = (i == packets.size() - 1);
        if (last) {
            CHECK_EQ(dp.header.flags & kFlagChunked, 0u);       // 11. 末包清除 CHUNKED
        } else {
            CHECK_EQ(dp.header.flags & kFlagChunked, kFlagChunked);  // 10. CHUNKED 正确
        }
    }
    // 拼接恢复原 Message 载荷
    const std::vector<uint8_t> spliced = splicePackets(packets, "large_rect_splice");
    CHECK_EQ(spliced.size(), payloadSize);
    CHECK(std::memcmp(spliced.data(), msg->payload.data(), payloadSize) == 0);
    // 矩形头可从拼接载荷解析
    CHECK_EQ(spliced[4], static_cast<uint8_t>(320 & 0xFF));
    CHECK_EQ(spliced[5], static_cast<uint8_t>((320 >> 8) & 0xFF));
    CHECK_EQ(spliced[6], static_cast<uint8_t>(240 & 0xFF));
    CHECK_EQ(spliced[7], static_cast<uint8_t>((240 >> 8) & 0xFF));
}

// 13. SEQ 连续递增（小消息序列）
void seq_consecutive() {
    SequenceCounter seq;
    MessageEncoder enc(seq);
    for (int i = 0; i < 3; ++i) {
        const Message m = makePing(static_cast<uint64_t>(i));
        std::vector<std::vector<uint8_t>> packets;
        CHECK_EQ(enc.encode(m, packets), PacketError::kNone);
        const DecodedPacket dp = decodePacket(packets[0], "seq_inc");
        CHECK_EQ(dp.header.seq, static_cast<uint16_t>(i));
    }
}

// 14. SEQ 从 65535 → 0（多包消息）
void seq_wrap_65535_to_0() {
    const size_t pixelBytes = 12288;  // 6144x1 → payload 12296 → 4 包
    std::vector<uint8_t> pixels(pixelBytes, 0xEE);
    auto msg = makeFrameRect(0, 0, 6144, 1, pixels.data(), pixels.size());
    CHECK(msg.has_value());
    CHECK_EQ(msg->payload.size(), 12296u);
    SequenceCounter seq(65535);
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 4u);
    const uint16_t expected[] = {65535, 0, 1, 2};
    for (size_t i = 0; i < packets.size(); ++i) {
        const DecodedPacket dp = decodePacket(packets[i], "seq_wrap");
        CHECK_EQ(dp.header.seq, expected[i]);
    }
}

// 15. 多包消息的每个 Packet CRC 全部正确
void packet_crc_all_valid() {
    const size_t pixelBytes = 9000;  // 4500x1 → payload 9008 → 3 包
    std::vector<uint8_t> pixels(pixelBytes, 0x12);
    auto msg = makeFrameRect(0, 0, 4500, 1, pixels.data(), pixels.size());
    CHECK(msg.has_value());
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 3u);
    for (size_t i = 0; i < packets.size(); ++i) {
        PacketHeader h;
        CHECK_EQ(decodeHeader(packets[i].data(), packets[i].size(), &h), PacketError::kNone);
        CHECK_EQ(verifyPacketCrc(h, packets[i].data() + kPacketHeaderSize, h.length),
                 PacketError::kNone);
    }
}

// 16. 空 payload 行为：1 个包，LENGTH=0，无 CHUNKED。
void empty_payload() {
    const Message m = makeMessage(static_cast<uint8_t>(MessageType::kPing), 0, {});
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(m, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 1u);
    const DecodedPacket dp = decodePacket(packets[0], "empty");
    CHECK_EQ(dp.header.length, 0u);
    CHECK_EQ(dp.header.flags, 0u);
}

// 18. 超过 MAX_MESSAGE_PAYLOAD（1 MiB）必须拒绝（M0-C 协议修订）
void oversized_message_rejected() {
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;

    // 1 MiB + 1 byte → kMessageTooLarge，不产生任何包。
    const std::vector<uint8_t> tooBig(kMaxMessagePayload + 1, 0x01);
    const Message m =
        makeMessage(static_cast<uint8_t>(MessageType::kFrameRect), 0, tooBig);
    CHECK_EQ(enc.encode(m, packets), PacketError::kMessageTooLarge);
    CHECK_EQ(packets.size(), 0u);

    // 恰好 1 MiB → 允许，256 包（kMaxMessagePayload / kMaxPacketPayload）。
    const std::vector<uint8_t> exact(kMaxMessagePayload, 0x02);
    const Message m2 =
        makeMessage(static_cast<uint8_t>(MessageType::kFrameRect), 0, exact);
    CHECK_EQ(enc.encode(m2, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), kMaxMessagePayload / kMaxPacketPayload);
    for (const auto& p : packets) {
        PacketHeader h;
        CHECK_EQ(decodeHeader(p.data(), p.size(), &h), PacketError::kNone);
        CHECK_EQ(verifyPacketCrc(h, p.data() + kPacketHeaderSize, h.length), PacketError::kNone);
    }
}
// 17. 非法输入被拒绝
void invalid_rejected() {
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;

    // TYPE 越界（< 0x01 / > 0x51）
    const Message bad0 = makeMessage(0x00, 0, {});
    CHECK_EQ(enc.encode(bad0, packets), PacketError::kInvalidType);
    CHECK_EQ(packets.size(), 0u);
    const Message bad1 = makeMessage(0x52, 0, {});
    CHECK_EQ(enc.encode(bad1, packets), PacketError::kInvalidType);

    // FRAME_RECT 像素数不匹配（RGB565 应为 w*h*2）
    const uint8_t px3[3] = {1, 2, 3};
    CHECK(!makeFrameRect(0, 0, 2, 1, px3, 3).has_value());
    // 零尺寸矩形
    CHECK(!makeFrameRect(0, 0, 0, 1, nullptr, 0).has_value());
    // HELLO 设备名过长（> 32）
    CHECK(!makeHello(kProtocolVersion, 0, 320, 240, PixelFormat::kRgb565, 0,
                     std::string(33, 'x')).has_value());
    // HELLO 分辨率越界
    CHECK(!makeHello(kProtocolVersion, 0, 0, 240, PixelFormat::kRgb565, 0, "").has_value());
    // ERROR 文本过长（> 64）
    CHECK(!makeError(ErrorCode::kInternal, std::string(65, 'y')).has_value());
    // FRAME_BEGIN 分辨率越界
    CHECK(!makeFrameBegin(1, FrameType::kFull, PixelFormat::kRgb565, 0, 240, 0).has_value());
}

// 重建：多包 FRAME_RECT → 解码 → 拼接 → 恢复原 Message 载荷并解析矩形头。
void reconstruct_frame_rect() {
    const size_t pixelBytes = 8200;  // 4100x1 → payload 8208 → 3 包
    std::vector<uint8_t> pixels(pixelBytes);
    for (size_t i = 0; i < pixelBytes; ++i) {
        pixels[i] = static_cast<uint8_t>(i * 7);
    }
    auto msg = makeFrameRect(10, 20, 4100, 1, pixels.data(), pixels.size());
    CHECK(msg.has_value());
    SequenceCounter seq;
    MessageEncoder enc(seq);
    std::vector<std::vector<uint8_t>> packets;
    CHECK_EQ(enc.encode(*msg, packets), PacketError::kNone);
    CHECK_EQ(packets.size(), 3u);

    const std::vector<uint8_t> spliced = splicePackets(packets, "reconstruct");
    CHECK_EQ(spliced.size(), msg->payload.size());
    CHECK(std::memcmp(spliced.data(), msg->payload.data(), msg->payload.size()) == 0);

    // 矩形头：x=10, y=20, w=4100(0x1004), h=1
    CHECK_EQ(spliced[0], 10u);
    CHECK_EQ(spliced[1], 0u);
    CHECK_EQ(spliced[2], 20u);
    CHECK_EQ(spliced[3], 0u);
    CHECK_EQ(spliced[4], 0x04);
    CHECK_EQ(spliced[5], 0x10);
    CHECK_EQ(spliced[6], 1u);
    CHECK_EQ(spliced[7], 0u);
    CHECK(std::memcmp(spliced.data() + 8, pixels.data(), pixelBytes) == 0);
}

}  // namespace

void runEncoderTests() {
    std::printf("  hello_single_packet\n");
    hello_single_packet();
    std::printf("  ping_pong_single_packet\n");
    ping_pong_single_packet();
    std::printf("  set_mode_ack_req\n");
    set_mode_ack_req();
    std::printf("  frame_begin\n");
    frame_begin();
    std::printf("  frame_end\n");
    frame_end();
    std::printf("  min_frame_rect\n");
    min_frame_rect();
    std::printf("  frame_rect_exact_4096\n");
    frame_rect_exact_4096();
    std::printf("  frame_rect_over_4096\n");
    frame_rect_over_4096();
    std::printf("  large_frame_rect_chunking\n");
    large_frame_rect_chunking();
    std::printf("  seq_consecutive\n");
    seq_consecutive();
    std::printf("  seq_wrap_65535_to_0\n");
    seq_wrap_65535_to_0();
    std::printf("  packet_crc_all_valid\n");
    packet_crc_all_valid();
    std::printf("  empty_payload\n");
    empty_payload();
    std::printf("  invalid_rejected\n");
    invalid_rejected();
    std::printf("  oversized_message_rejected\n");
    oversized_message_rejected();
    std::printf("  reconstruct_frame_rect\n");
    reconstruct_frame_rect();
}
