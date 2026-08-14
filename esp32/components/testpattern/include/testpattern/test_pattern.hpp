// ESPView M1-3C — TestPattern（ESP32 侧确定性帧发送脚本，test-only）。
//
// 规范来源：docs/DESIGN.md E 节（帧消息 Layout / PARTIAL 提交语义）。
// 目的：产生确定性 Frame Message 流，供真实硬件验收（M1-3B / M1-3C）。
// 每次会话 CONNECTED 后自动从头运行一轮脚本；DISCONNECTED 停止并等待下次连接。
//
// 脚本（每轮 cycle base = cycle_*100）：
//   PARTIAL base+1 / base+2      （无已提交基准 → 接收端拒绝；重连后自动验证）
//   FULL small  base+10          （4×16x16 rect，建立基准）
//   PARTIAL base+11              （有基准 → 提交）
//   FULL large  base+20          （单 RECT 320x240，153608B Message，Streaming 发送）
//   FULL small  base+30          （坏帧后的重同步恢复目标）
//   PARTIAL base+31
//
// 像素公式（与 PC 侧 pc/src/com3_frame_test.cpp 完全一致）：
//   RGB565: lo=(frameId+rectId+x)&0xFF, hi=(frameId+y+1)&0xFF，小端字节对。
//
// 内存模型（M1-3C）：不持有 permanent framebuffer；153608B 大 RECT 通过
//   RectPatternSource（IMessagePayloadSource）按需产生字节，经
//   MessageEncoder::encodeStreaming 逐包编码发送，峰值额外内存 ≈ 8.2KB
//   （4096B staging + 4116B 单包缓冲），不要求整段 payload 驻留内存。

#pragma once

#include <cstdint>
#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "encoder.h"  // IMessagePayloadSource（StreamingSender 签名）
#include "message.h"
#include "protocol.h"
#include "protocol_endpoint.h"  // SendResult

namespace espview {

class TestPattern {
public:
    using Sender = std::function<proto::SendResult(const proto::Message&)>;
    // M1-3C：流式发送（MessageHeader + IMessagePayloadSource → SendResult）。
    // 由 main.cpp 接到 ProtocolEndpoint::sendMessageStreaming。
    using StreamingSender = std::function<proto::SendResult(
        const proto::MessageHeader&, proto::IMessagePayloadSource&)>;

    explicit TestPattern(Sender sender, StreamingSender streamingSender);
    ~TestPattern();

    void start();  // 创建任务（app_main 调用一次）
    void onSessionState(proto::SessionState s);

private:
    static void taskEntry(void* arg);
    void taskLoop();
    void runScript();
    bool send(const proto::Message& msg);
    bool sendSmallFull(uint16_t frameId);
    bool sendLargeFull(uint16_t frameId);
    bool sendPartial(uint16_t frameId, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    Sender sender_;
    StreamingSender streamingSender_;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t startSem_ = nullptr;
    volatile bool running_ = false;
    volatile bool connected_ = false;
    uint16_t cycle_ = 0;
};

}  // namespace espview
