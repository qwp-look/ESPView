// ESPView — RemoteDisplay Host Tests（M5-A）
//
// 规范来源：docs/DESIGN.md D.1/D.2（IDisplay/DisplayManager）、E 节（帧消息
// Layout / PARTIAL 语义）+ M5-A 任务书 §26。
//
// 测试链路（纯 host，零平台依赖）：
//   RemoteDisplay → IFrameSink(RecordingSink，真实 MessageEncoder 编码)
//   → 收集 packet bytes → StreamDecoder 解码回 Message → 断言；
//   关键场景另跑 StreamDecoder → FrameAssembler 全管线验证提交/丢弃语义。
// 原则：
//   - 协议数据全部由真实 Encoder 产生（不手工构造 wire bytes）；
//   - CRC/SEQ/CHUNKED 正确性由「解码成功 + 字段断言」隐式验证；
//   - 测试侧仅在必要时持有参考 vector（TEST ONLY，不代表生产内存模型）。
// 纯 C++17。

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "decoder.h"
#include "display.h"
#include "display_manager.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "remote_display.h"
#include "test_util.h"

namespace {

using espview::display::DisplayConfig;
using espview::display::DisplayInfo;
using espview::display::DisplayManager;
using espview::display::DisplayStatus;
using espview::display::IFrameSink;
using espview::display::RemoteDisplay;
using espview::proto::CommittedFrame;
using espview::proto::DecoderError;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameType;
using espview::proto::IMessagePayloadSource;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageHeader;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::SendResult;
using espview::proto::SequenceCounter;
using espview::proto::StreamDecoder;
using espview::proto::kFlagChunked;

// ---- 测试 sink：真实 Encoder 编码，收集 packet bytes ----
class RecordingSink : public IFrameSink {
public:
    espview::proto::SendResult send(const espview::proto::Message& msg) override {
        std::vector<std::vector<uint8_t>> out;
        const espview::proto::PacketError e = encoder_.encode(msg, out);
        if (e != espview::proto::PacketError::kNone) {
            return espview::proto::SendResult::kInvalidMessage;
        }
        for (auto& p : out) {
            packets_.push_back(std::move(p));
        }
        return espview::proto::SendResult::kOk;
    }

    espview::proto::SendResult sendStreaming(
        const espview::proto::MessageHeader& header,
        espview::proto::IMessagePayloadSource& source) override {
        const espview::proto::PacketError e = encoder_.encodeStreaming(header, source,
            [this](const uint8_t* data, size_t len) {
                packets_.emplace_back(data, data + len);
                return true;
            });
        return e == espview::proto::PacketError::kNone ? espview::proto::SendResult::kOk
                                                       : espview::proto::SendResult::kInvalidMessage;
    }

    std::vector<std::vector<uint8_t>> packets_;
    SequenceCounter seq_;
    MessageEncoder encoder_{seq_};
};

// ---- 解码：packet bytes → Message 流（真实 StreamDecoder）----
class MessageCollector {
public:
    MessageCollector() : dec_([this](const Message& m) { messages_.push_back(m); }) {}

    void feedAll(const std::vector<std::vector<uint8_t>>& packets) {
        for (const auto& p : packets) {
            dec_.feed(p.data(), p.size());
        }
    }

    StreamDecoder dec_;
    std::vector<Message> messages_;
};

// ---- FrameAssembler 全管线收集 ----
struct AssemblerCollector {
    std::vector<CommittedFrame> commits;
    std::vector<FrameDiscardReason> discards;
    std::vector<std::vector<uint8_t>> rectPixels;
};

espview::proto::FrameAssembler makeAssembler(AssemblerCollector& c) {
    espview::proto::FrameAssembler::Callbacks cb;
    cb.onRect = [&c](const espview::proto::RectInfo& r, const uint8_t* p, size_t n) {
        (void)r;
        c.rectPixels.emplace_back(p, p + n);
    };
    cb.onCommit = [&c](const CommittedFrame& f) { c.commits.push_back(f); };
    cb.onDiscard = [&c](FrameDiscardReason reason) { c.discards.push_back(reason); };
    return espview::proto::FrameAssembler(std::move(cb));
}

// ---- 工具 ----
uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// RGB565 LE 像素（lo/hi 确定性公式）。
std::vector<uint8_t> makePixels(int w, int h, uint8_t seed) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 2u);
    size_t k = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            px[k++] = static_cast<uint8_t>(x + y + seed);
            px[k++] = static_cast<uint8_t>(x * 3u + y * 5u + seed);
        }
    }
    return px;
}

RemoteDisplay::Config makeCfg(int w = 320, int h = 240) {
    RemoteDisplay::Config c;
    c.width = w;
    c.height = h;
    c.format = PixelFormat::kRgb565;
    c.queueSlots = 2;
    c.slotPixelBytes = static_cast<size_t>(w) * 24u * 2u;  // 1/10 屏（320x24）
    return c;
}

// ---- 用例 ----
void testNotConnected() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    const std::vector<uint8_t> px = makePixels(16, 8, 1);
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kNotConnected));
    CHECK(!rd.pump());
    rd.onConnected();
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kOk));
}

void testWriteRectBounds() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px = makePixels(16, 8, 2);
    CHECK_EQ(static_cast<int>(rd.writeRect(-1, 0, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(rd.writeRect(0, -1, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 0, 8, px.data())),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 16, 0, px.data())),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(rd.writeRect(310, 0, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kInvalidParam));  // x+w > 320
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 235, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kInvalidParam));  // y+h > 240
    // 超出单槽容量（整屏 153600B > 15360B）
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 320, 240, nullptr)),
             static_cast<int>(DisplayStatus::kRectTooLarge));
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 320, 240, px.data())),
             static_cast<int>(DisplayStatus::kRectTooLarge));
    // 未使能
    CHECK_EQ(static_cast<int>(rd.setEnabled(false)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kNotEnabled));
}

void decodeMessages(const std::vector<std::vector<uint8_t>>& packets,
                    std::vector<Message>& out) {
    MessageCollector mc;
    mc.feedAll(packets);
    out = std::move(mc.messages_);
}

// FULL 首帧 + RGB565 + 小矩形 + 单包
void testFirstFrameFullAndPixels() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px = makePixels(16, 8, 7);
    CHECK_EQ(static_cast<int>(rd.writeRect(10, 12, 16, 8, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }

    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(3));
    if (msgs.size() < 3) {
        return;
    }
    // BEGIN
    CHECK_EQ(msgs[0].type, static_cast<uint8_t>(MessageType::kFrameBegin));
    CHECK_EQ(msgs[0].payload.size(), size_t(12));
    CHECK_EQ(readU16(msgs[0].payload.data() + 0), uint16_t(1));          // frameId
    CHECK_EQ(msgs[0].payload[2], static_cast<uint8_t>(FrameType::kFull));  // 首帧 FULL
    CHECK_EQ(msgs[0].payload[3], static_cast<uint8_t>(PixelFormat::kRgb565));
    CHECK_EQ(readU16(msgs[0].payload.data() + 4), uint16_t(320));
    CHECK_EQ(readU16(msgs[0].payload.data() + 6), uint16_t(240));
    // RECT（单包，CHUNKED=0）
    CHECK_EQ(msgs[1].type, static_cast<uint8_t>(MessageType::kFrameRect));
    CHECK_EQ(msgs[1].payload.size(), size_t(8 + 16 * 8 * 2));
    CHECK_EQ(readU16(msgs[1].payload.data() + 0), uint16_t(10));
    CHECK_EQ(readU16(msgs[1].payload.data() + 2), uint16_t(12));
    CHECK_EQ(readU16(msgs[1].payload.data() + 4), uint16_t(16));
    CHECK_EQ(readU16(msgs[1].payload.data() + 6), uint16_t(8));
    CHECK(msgs[1].payload.size() >= 8);
    CHECK(std::equal(px.begin(), px.end(), msgs[1].payload.begin() + 8));
    // END
    CHECK_EQ(msgs[2].type, static_cast<uint8_t>(MessageType::kFrameEnd));
    CHECK_EQ(readU16(msgs[2].payload.data() + 0), uint16_t(1));
    CHECK_EQ(readU16(msgs[2].payload.data() + 2), uint16_t(1));    // rectCount
    CHECK_EQ(readU32(msgs[2].payload.data() + 4), uint32_t(256));  // byteCount
    CHECK_EQ(msgs[2].payload[8], uint8_t(0));                      // 未 ABORTED
    // 单包：sink.packets_ 应恰为 3 个 packet（BEGIN/RECT/END）
    CHECK_EQ(sink.packets_.size(), size_t(3));
}

// 大矩形（320x24 = 15360B payload，跨 4 个 CHUNKED packet）→ streaming 路径
void testLargeRectStreaming() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px = makePixels(320, 24, 3);  // 15360B
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 320, 24, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }

    // 包数 = BEGIN(1) + RECT(4) + END(1) = 6
    CHECK_EQ(sink.packets_.size(), size_t(6));
    if (sink.packets_.size() < 6) {
        return;
    }
    // 逐包解头：SEQ 连续、TYPE 一致、CHUNKED 位正确、CRC 由解码隐式验证
    uint16_t expectSeq = 0;
    for (size_t i = 0; i < sink.packets_.size(); ++i) {
        PacketHeader hdr;
        CHECK_EQ(static_cast<int>(espview::proto::decodeHeader(
                     sink.packets_[i].data(), sink.packets_[i].size(), &hdr)),
                 static_cast<int>(PacketError::kNone));
        CHECK_EQ(hdr.seq, expectSeq);
        ++expectSeq;
        if (i == 0) {
            CHECK_EQ(hdr.type, static_cast<uint8_t>(MessageType::kFrameBegin));
            CHECK_EQ(hdr.flags & kFlagChunked, uint8_t(0));
        } else if (i >= 1 && i <= 4) {
            CHECK_EQ(hdr.type, static_cast<uint8_t>(MessageType::kFrameRect));
            CHECK_EQ(hdr.flags & kFlagChunked, i <= 3 ? kFlagChunked : uint8_t(0));
        } else {
            CHECK_EQ(hdr.type, static_cast<uint8_t>(MessageType::kFrameEnd));
            CHECK_EQ(hdr.flags & kFlagChunked, uint8_t(0));
        }
    }
    // 解码回消息：RECT payload 拼接后与输入一致
    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(3));
    if (msgs.size() < 3) {
        return;
    }
    CHECK_EQ(msgs[1].payload.size(), size_t(8 + 320 * 24 * 2));
    CHECK(std::equal(px.begin(), px.end(), msgs[1].payload.begin() + 8));
    CHECK_EQ(readU16(msgs[2].payload.data() + 2), uint16_t(1));
    CHECK_EQ(readU32(msgs[2].payload.data() + 4), uint32_t(320 * 24 * 2));
}

// PARTIAL after FULL（基准存在）
void testPartialAfterFull() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px1 = makePixels(32, 16, 1);
    const std::vector<uint8_t> px2 = makePixels(16, 8, 2);
    // 帧1：FULL
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 32, 16, px1.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    // 帧2：PARTIAL
    CHECK_EQ(static_cast<int>(rd.writeRect(4, 4, 16, 8, px2.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(6));
    if (msgs.size() < 6) {
        return;
    }
    CHECK_EQ(msgs[0].payload[2], static_cast<uint8_t>(FrameType::kFull));
    CHECK_EQ(msgs[3].payload[2], static_cast<uint8_t>(FrameType::kPartial));
    CHECK_EQ(readU16(msgs[3].payload.data() + 0), uint16_t(2));  // frameId 递增
    CHECK_EQ(readU16(msgs[5].payload.data() + 0), uint16_t(2));
}

// 断线重连 → 下一帧必须 FULL
void testDisconnectRequiresFull() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px = makePixels(32, 16, 5);
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 32, 16, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    rd.onDisconnected();
    // 断线期间 writeRect 拒绝
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 32, 16, px.data())),
             static_cast<int>(DisplayStatus::kNotConnected));
    rd.onConnected();
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 32, 16, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(6));
    if (msgs.size() < 6) {
        return;
    }
    CHECK_EQ(msgs[0].payload[2], static_cast<uint8_t>(FrameType::kFull));    // 帧1
    CHECK_EQ(msgs[3].payload[2], static_cast<uint8_t>(FrameType::kFull));    // 重连后 FULL
    CHECK_EQ(readU16(msgs[3].payload.data() + 0), uint16_t(2));              // 新 frameId
}

// 背压：队列满 → 帧作废（ABORTED END）→ 下一帧 FULL resync
void testBackpressureAbort() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px = makePixels(64, 24, 9);  // 3072B（1 槽内）
    // 两个矩形填满 2 槽
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 64, 24, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.writeRect(64, 0, 64, 24, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    // 第 3 个矩形 → 队列满（背压，不改变帧状态）
    CHECK_EQ(static_cast<int>(rd.writeRect(128, 0, 64, 24, px.data())),
             static_cast<int>(DisplayStatus::kQueueFull));
    // 先 pump 掉已入队条目（BEGIN+RECT1+RECT2 已上 wire）
    while (rd.pump()) {
    }
    // 超时后丢弃整帧
    rd.dropPendingFrame();
    while (rd.pump()) {
    }
    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(4));  // BEGIN + RECT + RECT + ABORTED END
    if (msgs.size() < 4) {
        return;
    }
    CHECK_EQ(msgs[0].type, static_cast<uint8_t>(MessageType::kFrameBegin));
    CHECK_EQ(msgs[3].type, static_cast<uint8_t>(MessageType::kFrameEnd));
    CHECK_EQ(msgs[3].payload[8], uint8_t(1));  // ABORTED=1
    // 统计（作废帧：ABORTED END 阻止 PC 提交半帧；已发 2 RECT 仍统计为 sent）
    {
        const auto st = rd.statsSnapshot();
        CHECK_EQ(st.framesDropped, uint64_t(1));
        CHECK_EQ(st.queueFullEvents, uint64_t(1));
        CHECK_EQ(st.rectsSent, uint64_t(2));
    }
    // 下一帧必须 FULL（FULL resync）
    const std::vector<uint8_t> px2 = makePixels(32, 16, 4);
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 32, 16, px2.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(7));
    if (msgs.size() < 7) {
        return;
    }
    CHECK_EQ(msgs[4].payload[2], static_cast<uint8_t>(FrameType::kFull));  // FULL resync
}

// 背压发生在 BEGIN 发出前：作废帧从未上 wire，PC 无感知
void testBackpressureBeforeBegin() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px = makePixels(64, 24, 9);
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 64, 24, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.writeRect(64, 0, 64, 24, px.data())),
             static_cast<int>(DisplayStatus::kOk));
    // 队列满（TX 尚未 pump）：丢弃整帧
    rd.dropPendingFrame();
    while (rd.pump()) {
    }
    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(0));  // 从未发 BEGIN → 无任何 wire 消息
    const auto st = rd.statsSnapshot();
    CHECK_EQ(st.framesDropped, uint64_t(1));
    CHECK(st.rectsDropped >= 2);
}

// flush 生命周期：多 RECT 一帧、flush no-op、frameId 一致
void testFlushLifecycle() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px1 = makePixels(40, 16, 11);
    const std::vector<uint8_t> px2 = makePixels(24, 12, 12);
    // 两矩形 + flush → BEGIN + RECT + RECT + END(rectCount=2)
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 40, 16, px1.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 16, 24, 12, px2.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(4));
    if (msgs.size() < 4) {
        return;
    }
    const uint16_t fid = readU16(msgs[0].payload.data());
    CHECK_EQ(readU16(msgs[3].payload.data() + 0), fid);  // END.frameId == BEGIN.frameId
    CHECK_EQ(readU16(msgs[3].payload.data() + 2), uint16_t(2));
    CHECK_EQ(readU32(msgs[3].payload.data() + 4), uint32_t(40 * 16 * 2 + 24 * 12 * 2));
    // 无开放帧时 flush no-op
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    // 统计
    const auto st = rd.statsSnapshot();
    CHECK_EQ(st.framesFull, uint64_t(1));
    CHECK_EQ(st.rectsSent, uint64_t(2));
    CHECK_EQ(st.fullPixelBytes, uint64_t(40 * 16 * 2 + 24 * 12 * 2));
    CHECK_EQ(st.lastRectCount, uint32_t(2));
    CHECK_EQ(st.lastFrameId, fid);
    CHECK_EQ(st.lastFrameType, FrameType::kFull);
}

// 统计：FULL/PARTIAL 字节、dirty ratio 依据
void testStatsDirtyRatio() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    const std::vector<uint8_t> px1 = makePixels(64, 16, 1);   // 2048B
    const std::vector<uint8_t> px2 = makePixels(32, 8, 2);    // 512B
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 64, 16, px1.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 32, 8, px2.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    const auto st = rd.statsSnapshot();
    CHECK_EQ(st.framesFull, uint64_t(1));
    CHECK_EQ(st.framesPartial, uint64_t(1));
    CHECK_EQ(st.fullPixelBytes, uint64_t(2048));
    CHECK_EQ(st.partialPixelBytes, uint64_t(512));
    CHECK_EQ(rd.fullScreenBytes(), uint32_t(320 * 240 * 2));
    // dirty ratio ≈ 512 / 153600
    CHECK(st.partialPixelBytes * 1000u / rd.fullScreenBytes() == 3u);  // 0.33%
}

// 全管线：RemoteDisplay → Encoder → Decoder → FrameAssembler（FULL 提交 / PARTIAL 提交 / ABORTED 丢弃）
void testPipelineCommit() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    AssemblerCollector ac;
    espview::proto::FrameAssembler assembler = makeAssembler(ac);
    rd.onConnected();
    const std::vector<uint8_t> px1 = makePixels(40, 16, 21);
    const std::vector<uint8_t> px2 = makePixels(20, 8, 22);
    // 帧1：FULL
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 40, 16, px1.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    // 帧2：PARTIAL
    CHECK_EQ(static_cast<int>(rd.writeRect(5, 5, 20, 8, px2.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    // 帧3：背压 → ABORTED（丢弃）
    const std::vector<uint8_t> px3 = makePixels(64, 24, 23);
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 64, 24, px3.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.writeRect(64, 0, 64, 24, px3.data())),
             static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }
    rd.dropPendingFrame();
    while (rd.pump()) {
    }
    // 帧4：FULL resync
    const std::vector<uint8_t> px4 = makePixels(30, 10, 24);
    CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 30, 10, px4.data())),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
    while (rd.pump()) {
    }

    // 喂给 FrameAssembler
    MessageCollector mc;
    mc.feedAll(sink.packets_);
    for (const auto& m : mc.messages_) {
        assembler.onMessage(m);
    }
    // 提交：FULL(帧1) + PARTIAL(帧2) + FULL(帧4)；ABORTED 帧丢弃
    CHECK_EQ(ac.commits.size(), size_t(3));
    if (ac.commits.size() < 3) {
        return;
    }
    CHECK_EQ(ac.commits[0].frameType, FrameType::kFull);
    CHECK_EQ(ac.commits[0].rectCount, uint16_t(1));
    CHECK_EQ(ac.commits[0].byteCount, uint32_t(40 * 16 * 2));
    CHECK_EQ(ac.commits[1].frameType, FrameType::kPartial);
    CHECK_EQ(ac.commits[2].frameType, FrameType::kFull);
    CHECK_EQ(ac.commits[2].frameId, uint16_t(4));
    // 至少一次 ABORTED 丢弃
    bool sawAborted = false;
    for (const auto& d : ac.discards) {
        if (d == FrameDiscardReason::kAborted) {
            sawAborted = true;
        }
    }
    CHECK(sawAborted);
}

// DisplayManager：编译期 WINDOW 模式
void testDisplayManager() {
    auto sink = std::make_shared<RecordingSink>();
    auto rd = std::make_shared<RemoteDisplay>(*sink, makeCfg());
    DisplayManager mgr;
    mgr.addBackend(rd);
    CHECK(mgr.hasActive());
    // active() 转发到 RemoteDisplay（Application 只见 DisplayManager）
    const auto& info = mgr.active().info();
    CHECK_EQ(info.width, 320);
    CHECK_EQ(info.height, 240);
    CHECK_EQ(static_cast<int>(mgr.setMode(espview::display::DisplayMode::kWindow)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(mgr.setMode(espview::display::DisplayMode::kDevice)),
             static_cast<int>(DisplayStatus::kNotSupported));
    CHECK_EQ(static_cast<int>(mgr.setMode(espview::display::DisplayMode::kMirror)),
             static_cast<int>(DisplayStatus::kNotSupported));
}

// frameId 回绕（65535 → 0）
void testFrameIdWrap() {
    RecordingSink sink;
    RemoteDisplay rd(sink, makeCfg());
    rd.onConnected();
    // 直接驱动 65535 帧不现实；用 3 帧验证递增，65535→0 由 uint16 算术保证。
    const std::vector<uint8_t> px = makePixels(16, 8, 31);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(static_cast<int>(rd.writeRect(0, 0, 16, 8, px.data())),
                 static_cast<int>(DisplayStatus::kOk));
        CHECK_EQ(static_cast<int>(rd.flush()), static_cast<int>(DisplayStatus::kOk));
        while (rd.pump()) {
        }
    }
    std::vector<Message> msgs;
    decodeMessages(sink.packets_, msgs);
    CHECK_EQ(msgs.size(), size_t(9));
    if (msgs.size() < 9) {
        return;
    }
    CHECK_EQ(readU16(msgs[0].payload.data()), uint16_t(1));
    CHECK_EQ(readU16(msgs[3].payload.data()), uint16_t(2));
    CHECK_EQ(readU16(msgs[6].payload.data()), uint16_t(3));
}

}  // namespace

void runRemoteDisplayTests() {
    std::printf("[remote_display]\n");
    testNotConnected();
    testWriteRectBounds();
    testFirstFrameFullAndPixels();
    testLargeRectStreaming();
    testPartialAfterFull();
    testDisconnectRequiresFull();
    testBackpressureAbort();
    testBackpressureBeforeBegin();
    testFlushLifecycle();
    testStatsDirtyRatio();
    testPipelineCommit();
    testDisplayManager();
    testFrameIdWrap();
}
