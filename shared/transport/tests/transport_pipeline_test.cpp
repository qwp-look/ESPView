// ESPView M6-C — Transport Pipeline Test（TransportManager + ProtocolEndpoint 集成）。
//
// 覆盖 M6-C 任务书：
//   §二十八.7  session reset（切换后 A 会话重置：seq/decoder/frame 清零 → 重握手）
//   §二十八.8  FULL resync（切换后新 FULL 帧正常提交，无 stale 基准）
//   §二十八.9  input state reset（切换后 InputManager 清 pressed 状态）
//   §二十八.15 control traffic during display load（大帧背压 → 整帧丢弃，不提交半帧）
//   §二十一    runtime switch 语义（Disconnected → Connected → HELLO → FULL）
//
// 原则：所有 wire bytes 由真实 MessageEncoder 产生、真实 StreamDecoder 消费；
//   A = ESP32 侧（TransportManager + FakeTransport + TransportSink + ProtocolEndpoint），
//   B = PC 侧（ProtocolEndpoint + in-memory 双向管道）。
// 纯 C++17，零平台依赖。

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "input_event.h"
#include "input_manager.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"
#include "protocol_endpoint.h"
#include "../transport/transport_manager.h"
#include "../transport/transport_sink.h"
#include "in_memory_byte_pipe.h"
#include "test_util.h"
#include "../transport/tests/transport_test_util.h"

namespace {

using espview::proto::CommittedFrame;
using espview::proto::EndpointConfig;
using espview::proto::FrameDiscardReason;
using espview::proto::FrameType;
using espview::proto::IMessagePayloadSource;
using espview::proto::Message;
using espview::proto::MessageHeader;
using espview::proto::MessageType;
using espview::proto::PixelFormat;
using espview::proto::ProtocolEndpoint;
using espview::proto::SendResult;
using espview::proto::SendStatus;
using espview::proto::SessionState;

using espview::transport::ITransport;
using espview::transport::TransportManager;
using espview::transport::TransportSink;
using espview::transport::TransportType;
using espview::transport::test::FakeTransport;
using espview::transport::test::tcpCaps;
using espview::transport::test::uartCaps;

namespace test = espview::proto::test;

// ---- 流式 payload source（FRAME_RECT：8B 头 + 320x240 RGB565 像素，按需产生）----
class RectSource : public IMessagePayloadSource {
public:
    RectSource(uint16_t w, uint16_t h) : length_(8u + static_cast<size_t>(w) * h * 2u) {}

    size_t read(uint8_t* dst, size_t maxBytes) override {
        size_t n = 0;
        while (n < maxBytes && pos_ < length_) {
            if (pos_ < 8) {
                // x, y, w, h 各 2B LE（x=0,y=0）
                static const uint8_t kHdr[8] = {0, 0, 0, 0, 0x40, 0x01, 0xF0, 0x00};
                dst[n++] = kHdr[pos_];
            } else {
                dst[n++] = static_cast<uint8_t>((pos_ * 13u + 5u) & 0xFFu);
            }
            ++pos_;
        }
        return n;
    }

private:
    size_t length_ = 0;
    size_t pos_ = 0;
};

// ---- 双端 harness ----
struct Harness {
    std::vector<std::shared_ptr<FakeTransport>> owned;
    bool uartOpenOk = true;
    bool tcpOpenOk = true;
    uint64_t now = 0;

    // A 侧（manager + endpoint）
    TransportManager mgr;
    TransportSink sink;
    std::vector<SessionState> aStates;
    espview::input::InputManager input{320, 240};
    std::unique_ptr<ProtocolEndpoint> a;
    std::vector<std::vector<uint8_t>> sinkSleeps;  // unused

    // B 侧（PC peer）
    std::vector<SessionState> bStates;
    std::vector<CommittedFrame> bCommits;
    std::vector<FrameDiscardReason> bDiscards;
    std::unique_ptr<ProtocolEndpoint> b;
    espview::proto::test::InMemoryBytePipe pipeBA;

    size_t aTxPos = 0;  // A 当前 fake txData 已排空位置
    size_t bPos = 0;    // pipeBA 已排空位置

    explicit Harness(TransportType initial)
        : mgr(
              [this](TransportType t) -> std::shared_ptr<espview::transport::ITransport> {
                  auto f = std::make_shared<FakeTransport>(t, t == TransportType::kUart ? uartCaps() : tcpCaps());
                  f->setOpenResult(t == TransportType::kUart ? this->uartOpenOk : this->tcpOpenOk);
                  owned.emplace_back(f);
                  return f;  // shared_ptr: manager holds a reference
              },
              initial),
          sink(mgr, [this]() { return a && a->state() != SessionState::kDisconnected; },
               [this]() { return now; }, [this](uint32_t) {}) {
        EndpointConfig cfg;
        cfg.protocol_version = espview::proto::kProtocolVersion;
        cfg.width = 320;
        cfg.height = 240;
        cfg.pixel_format = PixelFormat::kRgb565;
        cfg.mode_mask = 0b1111;  // M7-C2：WINDOW|DEVICE|MIRROR|SPLIT
        cfg.device_name = "espview-a";

        ProtocolEndpoint::Callbacks acb;
        acb.onSessionState = [this](SessionState s) {
            aStates.push_back(s);
            if (s == SessionState::kDisconnected) {
                input.resetState();  // 断线/切换：本地安全恢复（与 main.cpp 一致）
            }
        };
        auto mapSend = [](espview::transport::SendStatus r) -> SendStatus {
            switch (r) {
                case espview::transport::SendStatus::kOk: return SendStatus::kOk;
                case espview::transport::SendStatus::kBackpressure: return SendStatus::kBackpressure;
                case espview::transport::SendStatus::kError: return SendStatus::kError;
                case espview::transport::SendStatus::kNotConnected: return SendStatus::kNotConnected;
            }
            return SendStatus::kError;
        };
        a = std::make_unique<ProtocolEndpoint>(
            cfg,
            [this, mapSend](const uint8_t* d, size_t n) { return mapSend(sink.send(d, n)); },
            [this, mapSend](const uint8_t* d, size_t n) { return mapSend(sink.trySend(d, n)); },
            std::move(acb), [this]() { return now; });

        mgr.setDataCallback([this](const uint8_t* d, size_t n) { a->onTransportData(d, n); });
        mgr.setStateCallback([this](ITransport::State s) {
            if (s == ITransport::State::kConnected) {
                a->onTransportConnected();
            } else if (s == ITransport::State::kDisconnected || s == ITransport::State::kError) {
                a->onTransportDisconnected();
            }
        });

        // B（PC 侧）
        EndpointConfig bcfg = cfg;
        bcfg.device_name = "espview-b";
        ProtocolEndpoint::Callbacks bcb;
        bcb.onSessionState = [this](SessionState s) { bStates.push_back(s); };
        bcb.onFrameCommit = [this](const CommittedFrame& f) { bCommits.push_back(f); };
        bcb.onFrameDiscard = [this](FrameDiscardReason r) { bDiscards.push_back(r); };
        b = std::make_unique<ProtocolEndpoint>(
            bcfg,
            [this](const uint8_t* d, size_t n) {
                pipeBA.write(d, n);
                return SendStatus::kOk;
            },
            [this](const uint8_t* d, size_t n) {
                pipeBA.write(d, n);
                return SendStatus::kOk;
            },
            std::move(bcb), [this]() { return now; });
    }

    FakeTransport* curFake() {
        // 反向取最近创建的、与当前类型匹配的 fake：切换后旧实例已 close，
        // 正向遍历会命中被摘除的旧 Transport（其回调已 detach，数据无法送达）。
        const TransportType t = mgr.current();
        for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
            if ((*it)->type() == t && (*it)->openCount() > 0) {
                return it->get();
            }
        }
        return owned.empty() ? nullptr : owned.back().get();
    }

    void drainA() {  // A 已发字节 → B
        FakeTransport* f = curFake();
        if (f == nullptr) {
            return;
        }
        const auto& data = f->txData();
        if (data.size() > aTxPos) {
            b->onTransportData(data.data() + aTxPos, data.size() - aTxPos);
            aTxPos = data.size();
        }
    }

    void drainB() {  // B 已发字节 → A
        FakeTransport* f = curFake();
        if (f == nullptr) {
            return;
        }
        const auto& v = pipeBA.bytes();
        if (v.size() > bPos) {
            f->deliverRx(v.data() + bPos, v.size() - bPos);
            bPos = v.size();
        }
    }

    // 握手直到双方 CONNECTED（最多 n 轮）。
    bool handshake(int rounds = 8) {
        for (int i = 0; i < rounds; ++i) {
            drainA();
            drainB();
            now += 100;
            a->tick();
            b->tick();
            if (a->state() == SessionState::kConnected &&
                b->state() == SessionState::kConnected) {
                return true;
            }
        }
        return false;
    }

    // 切换：管理器切换 + 断线语义（B 侧模拟 PC 断线重连）。
    bool doSwitch(TransportType t) {
        if (!mgr.switchTo(t)) {
            return false;
        }
        // 模拟 PC 侧：旧会话断开 → 新会话接受（HELLO 重新互换）。
        // 先清管道再重连：B 重连时 HELLO 写入 pipeBA，不得被 clear 抹掉。
        aTxPos = 0;
        bPos = 0;
        pipeBA.clear();
        b->onTransportDisconnected();
        b->onTransportConnected();
        return true;
    }

    // A 发送一帧 FULL（BEGIN + 单 RECT 流式 + END）。
    SendResult sendFullFrame(uint16_t frameId) {
        const auto begin = espview::proto::makeFrameBegin(frameId, FrameType::kFull,
                                                          PixelFormat::kRgb565, 320, 240, 153600);
        if (!begin.has_value()) {
            return SendResult::kInvalidMessage;
        }
        SendResult r = a->sendMessage(*begin);
        if (r != SendResult::kOk) {
            return r;
        }
        drainA();
        const MessageHeader hdr{static_cast<uint8_t>(MessageType::kFrameRect), 0};
        RectSource src(320, 240);
        r = a->sendMessageStreaming(hdr, src);
        if (r != SendResult::kOk) {
            return r;
        }
        drainA();
        const auto end = espview::proto::makeFrameEnd(frameId, 1, 153600, false);
        r = a->sendMessage(end);
        if (r != SendResult::kOk) {
            return r;
        }
        drainA();
        return SendResult::kOk;
    }
};

}  // namespace

void runTransportPipelineTests() {

    std::printf("[transport_pipeline]\n");

    // ---- A. handshake → FULL commit → switch → session/input reset → FULL resync ----
    {
        Harness h(TransportType::kUart);
        CHECK(h.mgr.open());
        CHECK(h.handshake());
        CHECK_EQ(h.aStates.back(), SessionState::kConnected);
        CHECK_EQ(h.bStates.back(), SessionState::kConnected);

        // FULL 帧 1 提交
        CHECK_EQ(h.sendFullFrame(1), SendResult::kOk);
        CHECK_EQ(h.bCommits.size(), 1u);
        CHECK_EQ(h.bCommits[0].frameId, 1u);
        CHECK_EQ(h.bCommits[0].byteCount, 153600u);
        CHECK_EQ(h.bCommits[0].rectCount, 1u);

        // 输入按下 → 切换 → resetState 清 pressed
        h.input.feed(espview::input::makeKeyEvent(espview::input::InputType::kKeyDown, 0x04, 0, 0));
        CHECK_EQ(h.input.stats().pressedKeys, 1u);

        // runtime switch：UART → TCP（§五 流程：停止 → 清 input → 会话重置 → 新 HELLO → FULL）
        CHECK(h.doSwitch(TransportType::kTcp));
        CHECK_EQ(h.mgr.current(), TransportType::kTcp);
        CHECK_EQ(h.input.stats().pressedKeys, 0u);  // input state reset（§二十八.9）
        CHECK(h.input.stats().resetCount >= 1u);
        // A 会话重置 + 重新握手
        CHECK(h.handshake());
        CHECK_EQ(h.aStates.back(), SessionState::kConnected);
        CHECK_EQ(h.bStates.back(), SessionState::kConnected);
        // 无 decoder/seq 错误（会话从 0 重新开始）
        CHECK_EQ(h.b->stats().decoderErrors, 0u);
        CHECK_EQ(h.b->stats().seqGaps, 0u);

        // FULL resync：切换后新 FULL 正常提交（§二十八.8）
        CHECK_EQ(h.sendFullFrame(2), SendResult::kOk);
        CHECK_EQ(h.bCommits.size(), 2u);
        CHECK_EQ(h.bCommits[1].frameId, 2u);
        CHECK_EQ(h.bCommits[1].byteCount, 153600u);
        CHECK_EQ(h.bCommits[1].frameType, static_cast<uint8_t>(FrameType::kFull));

        // 反向：TCP → UART（§二十八.4）
        CHECK(h.doSwitch(TransportType::kUart));
        CHECK(h.handshake());
        CHECK_EQ(h.aStates.back(), SessionState::kConnected);
        CHECK_EQ(h.sendFullFrame(3), SendResult::kOk);
        CHECK_EQ(h.bCommits.size(), 3u);
        CHECK_EQ(h.bCommits[2].frameId, 3u);
        h.mgr.close();
    }

    // ---- B. 大帧背压（TCP 单次尝试）→ 整帧丢弃，PC 不提交半帧（§二十八.13/14）----
    {
        Harness h(TransportType::kTcp);
        CHECK(h.mgr.open());
        CHECK(h.handshake());
        // 第 3 个包起背压（BEGIN ok、RECT 前 2 包 ok、之后背压 → 帧作废）
        std::vector<espview::transport::SendStatus> seq(40, espview::transport::SendStatus::kOk);
        seq[2] = espview::transport::SendStatus::kBackpressure;
        h.curFake()->setSendSequence(seq);
        const SendResult r = h.sendFullFrame(7);
        CHECK_EQ(r, SendResult::kBackpressure);  // 整帧失败信号
        // PC 侧没有收到 FRAME_END → 不提交半帧
        CHECK_EQ(h.bCommits.size(), 0u);
        // 下一次 FULL（背压解除）可恢复
        h.curFake()->setSendSequence({});
        h.now += 3000;
        h.a->tick();
        h.b->tick();
        CHECK_EQ(h.sendFullFrame(8), SendResult::kOk);
        CHECK_EQ(h.bCommits.size(), 1u);
        CHECK_EQ(h.bCommits[0].frameId, 8u);
        h.mgr.close();
    }

    std::printf("[transport_pipeline] done\n");
}
