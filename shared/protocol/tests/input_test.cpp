// ESPView M3 — Input 层宿主测试（input_codec / input_manager / keyboard_mapper /
// coordinate_mapper / input_policy）。
//
// 规范来源：docs/DESIGN.md E 节 INPUT_KEY / INPUT_MOUSE + B.4 InputManager +
// spec §6/§7/§11/§12/§14/§15/§18/§20/§21/§22。
//
// 关键原则（spec §22）：协议 round-trip 使用真实 MessageEncoder + StreamDecoder，
// 不手工构造 wire bytes；「逐字段一致」按 wire 语义验证：
//   - INPUT_KEY：type（down 位）+ keycode + modifiers 完全一致；
//   - INPUT_MOUSE：wire 无事件类型，buttons/x/y/wheel 逐字段一致；MouseDown/Up
//     语义由 InputManager 按按钮掩码变化推导（本文件单独验证 Manager 序列）。

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "decoder.h"
#include "encoder.h"
#include "message.h"
#include "protocol.h"
#include "test_util.h"

#include "coordinate_mapper.h"
#include "input_codec.h"
#include "input_event.h"
#include "input_manager.h"
#include "input_policy.h"
#include "keyboard_mapper.h"

namespace {

using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::SequenceCounter;
using espview::proto::StreamDecoder;
using espview::proto::makeMessage;

using espview::input::CoordinateMapper;
using espview::input::HostKey;
using espview::input::InputEvent;
using espview::input::InputManager;
using espview::input::InputPolicy;
using espview::input::InputStats;
using espview::input::InputType;
using espview::input::KeyMapResult;
using espview::input::KeyboardMapper;
using espview::input::MouseMoveThrottle;
using espview::input::decodeInputMessage;
using espview::input::encodeInputEvent;
using espview::input::kHidKeyboardFirst;
using espview::input::kHidModifierLast;
using espview::input::kModAlt;
using espview::input::kModCtrl;
using espview::input::kModGui;
using espview::input::kModShift;
using espview::input::kMouseFlagAbs;
using espview::input::kMouseLeft;
using espview::input::kMouseMiddle;
using espview::input::kMouseRight;
using espview::input::makeKeyEvent;
using espview::input::makeMouseButton;
using espview::input::makeMouseMove;
using espview::input::makeMouseWheel;

constexpr uint16_t kDisplayW = 320;
constexpr uint16_t kDisplayH = 240;
constexpr uint16_t kMaxX = kDisplayW - 1;
constexpr uint16_t kMaxY = kDisplayH - 1;

// ---- LE helpers（构造非法 INPUT_* payload 用）----
void putU16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(static_cast<uint8_t>(val & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
}
void putU32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(static_cast<uint8_t>(val & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 8) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 16) & 0xFFu));
    v.push_back(static_cast<uint8_t>((val >> 24) & 0xFFu));
}

// ---- 真实 Message → Packet bytes → StreamDecoder → Message ----
// 返回解码后的 Message；decodeError 记录解码错误（无错误则为 kNeedMoreData 之外值）。
struct RoundTripResult {
    std::optional<Message> decoded;
    std::vector<espview::proto::DecoderError> errors;
};

RoundTripResult wireRoundTrip(const Message& msg) {
    RoundTripResult r;
    SequenceCounter seq;
    MessageEncoder encoder(seq);
    std::vector<std::vector<uint8_t>> packets;
    const PacketError pe = encoder.encode(msg, packets);
    CHECK_MSG(pe == PacketError::kNone,
              std::string("encode ") + espview::proto::toString(pe));
    if (pe != PacketError::kNone) {
        return r;
    }

    StreamDecoder decoder(
        [&r](const Message& m) { r.decoded = m; },
        {}, [&r](espview::proto::DecoderError e) { r.errors.push_back(e); });
    for (const auto& pkt : packets) {
        decoder.feed(pkt.data(), pkt.size());
    }
    return r;
}

// ---- 键盘映射表断言 ----
void expectMap(HostKey key, uint32_t usage, uint16_t modifierBit) {
    KeyMapResult r;
    CHECK_MSG(KeyboardMapper::mapKey(key, r), "mapKey should support");
    if (r.supported) {
        CHECK_EQ(r.hidUsage, usage);
        CHECK_EQ(r.modifierBit, modifierBit);
    }
}

void testKeyboardMapper() {
    std::printf("  keyboard mapper\n");
    expectMap(HostKey::kA, 0x04, 0);
    expectMap(HostKey::kB, 0x05, 0);
    expectMap(HostKey::kZ, 0x1D, 0);
    expectMap(HostKey::k0, 0x27, 0);
    expectMap(HostKey::k1, 0x1E, 0);
    expectMap(HostKey::k9, 0x26, 0);
    expectMap(HostKey::kF1, 0x3A, 0);
    expectMap(HostKey::kF12, 0x45, 0);
    expectMap(HostKey::kEnter, 0x28, 0);
    expectMap(HostKey::kEscape, 0x29, 0);
    expectMap(HostKey::kBackspace, 0x2A, 0);
    expectMap(HostKey::kTab, 0x2B, 0);
    expectMap(HostKey::kSpace, 0x2C, 0);
    expectMap(HostKey::kLeft, 0x50, 0);
    expectMap(HostKey::kRight, 0x4F, 0);
    expectMap(HostKey::kUp, 0x52, 0);
    expectMap(HostKey::kDown, 0x51, 0);
    expectMap(HostKey::kHome, 0x4A, 0);
    expectMap(HostKey::kEnd, 0x4D, 0);
    expectMap(HostKey::kPageUp, 0x4B, 0);
    expectMap(HostKey::kPageDown, 0x4E, 0);
    expectMap(HostKey::kInsert, 0x49, 0);
    expectMap(HostKey::kDelete, 0x4C, 0);
    expectMap(HostKey::kCapsLock, 0x39, 0);
    expectMap(HostKey::kNumLock, 0x53, 0);
    expectMap(HostKey::kScrollLock, 0x47, 0);
    // 修饰键：usage 0xE0..0xE7 + 对应修饰位
    expectMap(HostKey::kLeftCtrl, 0xE0, kModCtrl);
    expectMap(HostKey::kRightCtrl, 0xE4, kModCtrl);
    expectMap(HostKey::kLeftShift, 0xE1, kModShift);
    expectMap(HostKey::kRightShift, 0xE5, kModShift);
    expectMap(HostKey::kLeftAlt, 0xE2, kModAlt);
    expectMap(HostKey::kRightAlt, 0xE6, kModAlt);
    expectMap(HostKey::kLeftGui, 0xE3, kModGui);
    expectMap(HostKey::kRightGui, 0xE7, kModGui);
    // 不支持
    KeyMapResult r;
    CHECK_MSG(!KeyboardMapper::mapKey(HostKey::kUnknown, r), "kUnknown unsupported");
}

void testCoordinateMapper() {
    std::printf("  coordinate mapper\n");
    int ox = -1, oy = -1;
    // 320x240 widget = 1:1
    CHECK(CoordinateMapper::mapPoint(0, 0, 320, 240, 320, 240, ox, oy));
    CHECK_EQ(ox, 0);
    CHECK_EQ(oy, 0);
    CHECK(CoordinateMapper::mapPoint(319, 239, 320, 240, 320, 240, ox, oy));
    CHECK_EQ(ox, 319);
    CHECK_EQ(oy, 239);
    // 640x480 widget = 2x 缩放：中心点
    CHECK(CoordinateMapper::mapPoint(320, 240, 640, 480, 320, 240, ox, oy));
    CHECK_EQ(ox, 160);
    CHECK_EQ(oy, 120);
    // 边缘 0
    CHECK(CoordinateMapper::mapPoint(0, 0, 640, 480, 320, 240, ox, oy));
    CHECK_EQ(ox, 0);
    CHECK_EQ(oy, 0);
    // 边缘 max
    CHECK(CoordinateMapper::mapPoint(639, 479, 640, 480, 320, 240, ox, oy));
    CHECK_EQ(ox, 319);
    CHECK_EQ(oy, 239);
    // 960x720（3x）：spec §12 例子 (480,360) → (160,120)
    CHECK(CoordinateMapper::mapPoint(480, 360, 960, 720, 320, 240, ox, oy));
    CHECK_EQ(ox, 160);
    CHECK_EQ(oy, 120);
    // letterbox：窗口 800x600（等比 320x240 → 800x600 无黑边，中心 400,300 → 160,120）
    CHECK(CoordinateMapper::mapPoint(400, 300, 800, 600, 320, 240, ox, oy));
    CHECK_EQ(ox, 160);
    CHECK_EQ(oy, 120);
    // letterbox 黑边：窗口 800x500（显示区 800x600 被高度限制 → 666x500，左右黑边）
    //   左边黑边 x=0..66，右边 x=734..799；中心 (400,250) 应在区内
    CHECK(CoordinateMapper::mapPoint(400, 250, 800, 500, 320, 240, ox, oy));
    CHECK_EQ(ox, 160);
    CHECK_EQ(oy, 120);
    // 黑边内：x=0（左边黑边）→ false
    CHECK_MSG(!CoordinateMapper::mapPoint(0, 250, 800, 500, 320, 240, ox, oy),
              "left letterbox outside");
    // 黑边内：y=0（上方？此处无上下黑边；改窗口 500x800 → 上下黑边）
    CHECK_MSG(!CoordinateMapper::mapPoint(250, 0, 500, 800, 320, 240, ox, oy),
              "top letterbox outside");
    CHECK(CoordinateMapper::mapPoint(250, 400, 500, 800, 320, 240, ox, oy));
    CHECK_EQ(ox, 160);
    CHECK_EQ(oy, 119);  // 中心对齐：dstY=213 → (400-213)*240/375 = 119
    // resize：任意窗口尺寸（100x100 < 逻辑屏）——Qt 会缩小显示，仍应可映射
    CHECK(CoordinateMapper::mapPoint(50, 50, 100, 100, 320, 240, ox, oy));
    CHECK_EQ(ox, 160);
    CHECK_EQ(oy, 118);  // 中心对齐：dstY=13 → (50-13)*240/75 = 118
    // 非法输入
    CHECK(!CoordinateMapper::mapPoint(0, 0, 0, 0, 320, 240, ox, oy));
    CHECK(!CoordinateMapper::mapPoint(0, 0, 100, 100, 0, 240, ox, oy));
}

void testWheelNormalization() {
    std::printf("  wheel normalization\n");
    // spec §15：angleDelta().y()/120 → ±1；不要直接 cast 120 到 int8
    CHECK_EQ(static_cast<int>(espview::input::normalizeWheelDelta(120)), 1);
    CHECK_EQ(static_cast<int>(espview::input::normalizeWheelDelta(-120)), -1);
    CHECK_EQ(static_cast<int>(espview::input::normalizeWheelDelta(240)), 2);
    CHECK_EQ(static_cast<int>(espview::input::normalizeWheelDelta(-240)), -2);
    CHECK_EQ(static_cast<int>(espview::input::normalizeWheelDelta(0)), 0);
    // clamp 到 int8 范围
    CHECK_EQ(static_cast<int>(espview::input::normalizeWheelDelta(120 * 1000)), 127);
    CHECK_EQ(static_cast<int>(espview::input::normalizeWheelDelta(-120 * 1000)), -128);
}

void testMouseMoveThrottle() {
    std::printf("  mouse move throttle\n");
    MouseMoveThrottle t(16);
    uint16_t x = 0, y = 0;
    uint8_t b = 0;
    // 首笔立即发送
    CHECK(t.acceptMove(1000, 10, 10, 0, x, y, b));
    CHECK_EQ(x, 10);
    CHECK_EQ(y, 10);
    // 窗口内（<16ms）→ coalesce，不发送
    CHECK(!t.acceptMove(1008, 20, 20, 0, x, y, b));
    CHECK(!t.acceptMove(1010, 30, 30, 1, x, y, b));
    // 窗口外的新事件 → 立即发送最新坐标
    CHECK(t.acceptMove(1020, 40, 40, 1, x, y, b));
    CHECK_EQ(x, 40);
    CHECK_EQ(y, 40);
    CHECK_EQ(b, 1);
    // 移动后停止：flushPending 补发最后一笔
    CHECK(!t.acceptMove(1030, 50, 50, 1, x, y, b));
    CHECK(t.flushPending(1040, x, y, b));
    CHECK_EQ(x, 50);
    CHECK_EQ(y, 50);
    CHECK(!t.flushPending(1050, x, y, b));  // 已清空
    t.reset();
    CHECK(t.acceptMove(2000, 1, 1, 0, x, y, b));
}

// ---- 收集 listener ----
class CollectListener : public espview::input::IInputListener {
public:
    void onInputEvent(const InputEvent& e) override { events.push_back(e); }
    std::vector<InputEvent> events;
};

void testInputManager() {
    std::printf("  input manager\n");
    InputManager mgr(kDisplayW, kDisplayH);
    CollectListener listener;
    mgr.registerListener(&listener);

    // 键盘按下/抬起 + 状态
    mgr.feed(makeKeyEvent(InputType::kKeyDown, 0x04, kModCtrl, 0));
    mgr.feed(makeKeyEvent(InputType::kKeyUp, 0x04, kModCtrl, 0));
    CHECK_EQ(listener.events.size(), 2u);
    CHECK_EQ(static_cast<unsigned>(listener.events[0].type),
             static_cast<unsigned>(InputType::kKeyDown));
    CHECK_EQ(listener.events[0].keycode, 0x04u);
    CHECK_EQ(listener.events[0].modifiers, kModCtrl);
    CHECK_EQ(static_cast<unsigned>(listener.events[1].type),
             static_cast<unsigned>(InputType::kKeyUp));
    CHECK_EQ(mgr.stats().pressedKeys, 0u);
    CHECK_EQ(mgr.stats().validEvents, 2u);

    // 鼠标序列：Move(0) → Move(1)=Down → Move(1)=Move → Move(0)=Up
    listener.events.clear();
    mgr.feed(makeMouseMove(100, 80, 0, 0));
    mgr.feed(makeMouseMove(110, 85, kMouseLeft, 0));
    mgr.feed(makeMouseMove(120, 90, kMouseLeft, 0));
    mgr.feed(makeMouseMove(130, 95, 0, 0));
    CHECK_EQ(listener.events.size(), 4u);
    CHECK_EQ(static_cast<unsigned>(listener.events[0].type),
             static_cast<unsigned>(InputType::kMouseMove));
    CHECK_EQ(static_cast<unsigned>(listener.events[1].type),
             static_cast<unsigned>(InputType::kMouseDown));
    CHECK_EQ(listener.events[1].buttons, kMouseLeft);
    CHECK_EQ(static_cast<unsigned>(listener.events[2].type),
             static_cast<unsigned>(InputType::kMouseMove));
    CHECK_EQ(listener.events[2].buttons, kMouseLeft);
    CHECK_EQ(static_cast<unsigned>(listener.events[3].type),
             static_cast<unsigned>(InputType::kMouseUp));
    CHECK_EQ(listener.events[3].buttons, 0u);
    CHECK_EQ(mgr.stats().pressedButtons, 0u);

    // 多键同时按下 → 每个变化位一个 Down；依次抬起 → 每个一个 Up
    listener.events.clear();
    mgr.feed(makeMouseMove(10, 10, 0, 0));
    mgr.feed(makeMouseMove(10, 10, static_cast<uint8_t>(kMouseLeft | kMouseRight), 0));
    CHECK_EQ(listener.events.size(), 3u);
    CHECK_EQ(static_cast<unsigned>(listener.events[1].type),
             static_cast<unsigned>(InputType::kMouseDown));
    CHECK_EQ(listener.events[1].buttons, static_cast<uint8_t>(kMouseLeft | kMouseRight));
    CHECK_EQ(static_cast<unsigned>(listener.events[2].type),
             static_cast<unsigned>(InputType::kMouseDown));
    mgr.feed(makeMouseMove(10, 10, kMouseLeft, 0));
    CHECK_EQ(listener.events.size(), 4u);
    CHECK_EQ(static_cast<unsigned>(listener.events[3].type),
             static_cast<unsigned>(InputType::kMouseUp));
    mgr.feed(makeMouseMove(10, 10, 0, 0));
    CHECK_EQ(listener.events.size(), 5u);
    CHECK_EQ(static_cast<unsigned>(listener.events[4].type),
             static_cast<unsigned>(InputType::kMouseUp));

    // Wheel：直接转发（按钮掩码同步）
    listener.events.clear();
    mgr.feed(makeMouseWheel(160, 120, 1, 0, 0));
    mgr.feed(makeMouseWheel(160, 120, -1, 0, 0));
    CHECK_EQ(listener.events.size(), 2u);
    CHECK_EQ(static_cast<unsigned>(listener.events[0].type),
             static_cast<unsigned>(InputType::kMouseWheel));
    CHECK_EQ(listener.events[0].wheelDelta, 1);
    CHECK_EQ(listener.events[1].wheelDelta, -1);

    // 非法输入拒绝（先清空 listener，确认非法事件一律不转发）
    listener.events.clear();
    const uint64_t before = mgr.stats().invalidEvents;
    mgr.feed(makeKeyEvent(InputType::kKeyDown, 0x0000, 0, 0));       // keycode 非法
    mgr.feed(makeKeyEvent(InputType::kKeyDown, 0xE8, 0, 0));         // 超出修饰范围
    mgr.feed(makeKeyEvent(InputType::kKeyDown, 0x04, 0x0010, 0));    // modifier 越界
    mgr.feed(makeMouseMove(kDisplayW, 0, 0, 0));                     // x 越界
    mgr.feed(makeMouseMove(0, kDisplayH, 0, 0));                     // y 越界
    mgr.feed(makeMouseMove(10, 10, 0x08, 0));                        // buttons 越界
    CHECK_EQ(mgr.stats().invalidEvents - before, 6u);
    CHECK_EQ(listener.events.size(), 0u);

    // 保留类型 → unsupported
    InputEvent touch;
    touch.type = InputType::kTouchDown;
    mgr.feed(touch);
    CHECK_EQ(mgr.stats().unsupportedEvents, 1u);

    // resetState：按下 Ctrl + 左键 → 断线恢复
    mgr.feed(makeKeyEvent(InputType::kKeyDown, 0xE0, kModCtrl, 0));
    mgr.feed(makeMouseMove(200, 150, kMouseLeft, 0));
    CHECK_EQ(mgr.stats().pressedKeys, 1u);
    CHECK_EQ(mgr.stats().pressedButtons, static_cast<uint8_t>(kMouseLeft));
    listener.events.clear();
    mgr.resetState();
    CHECK_EQ(mgr.stats().resetCount, 1u);
    CHECK_EQ(mgr.stats().stuckKeysReleased, 1u);
    CHECK_EQ(mgr.stats().stuckButtonsReleased, 1u);
    CHECK_EQ(mgr.stats().pressedKeys, 0u);
    CHECK_EQ(mgr.stats().pressedButtons, 0u);
    // 本地补发的 KeyUp + MouseUp（不回 PC —— 本层无发送面）
    CHECK_EQ(listener.events.size(), 2u);
    CHECK_EQ(static_cast<unsigned>(listener.events[0].type),
             static_cast<unsigned>(InputType::kKeyUp));
    CHECK_EQ(listener.events[0].keycode, 0xE0u);
    CHECK_EQ(static_cast<unsigned>(listener.events[1].type),
             static_cast<unsigned>(InputType::kMouseUp));
    CHECK_EQ(listener.events[1].buttons, 0u);
}

// ---- codec 正向/非法 ----
void testCodecEncodeDecode() {
    std::printf("  codec encode/decode\n");
    // 编码：合法
    const auto kd = encodeInputEvent(makeKeyEvent(InputType::kKeyDown, 0x04, kModCtrl, 123), kMaxX, kMaxY);
    CHECK(kd.has_value());
    CHECK_EQ(kd->type, static_cast<unsigned>(MessageType::kInputKey));
    CHECK_EQ(kd->payload.size(), 8u);
    const auto ku = encodeInputEvent(makeKeyEvent(InputType::kKeyUp, 0x04, kModCtrl, 0), kMaxX, kMaxY);
    CHECK(ku.has_value());
    const auto mm = encodeInputEvent(makeMouseMove(160, 120, kMouseLeft, 0), kMaxX, kMaxY);
    CHECK(mm.has_value());
    CHECK_EQ(mm->type, static_cast<unsigned>(MessageType::kInputMouse));
    const auto mw = encodeInputEvent(makeMouseWheel(160, 120, 1, 0, 0), kMaxX, kMaxY);
    CHECK(mw.has_value());

    // 编码：非法
    CHECK(!encodeInputEvent(makeKeyEvent(InputType::kKeyDown, 0x03, 0, 0), kMaxX, kMaxY).has_value());
    CHECK(!encodeInputEvent(makeKeyEvent(InputType::kKeyDown, 0x04, 0x10, 0), kMaxX, kMaxY).has_value());
    CHECK(!encodeInputEvent(makeMouseMove(320, 0, 0, 0), kMaxX, kMaxY).has_value());
    CHECK(!encodeInputEvent(makeMouseMove(0, 0, 0x08, 0), kMaxX, kMaxY).has_value());
    InputEvent touch;
    touch.type = InputType::kTouchDown;
    CHECK(!encodeInputEvent(touch, kMaxX, kMaxY).has_value());

    // 解码：合法（payload 由真实 builder 产生，保证与 Encoder 逐字段一致）
    const auto d1 = decodeInputMessage(*kd, kMaxX, kMaxY);
    CHECK(d1.has_value());
    CHECK_EQ(static_cast<unsigned>(d1->type), static_cast<unsigned>(InputType::kKeyDown));
    CHECK_EQ(d1->keycode, 0x04u);
    CHECK_EQ(d1->modifiers, kModCtrl);
    const auto d2 = decodeInputMessage(*mm, kMaxX, kMaxY);
    CHECK(d2.has_value());
    CHECK_EQ(d2->x, 160u);
    CHECK_EQ(d2->y, 120u);
    CHECK_EQ(d2->buttons, kMouseLeft);
    CHECK_EQ(d2->wheelDelta, 0);
    CHECK_EQ(static_cast<unsigned>(d2->type), static_cast<unsigned>(InputType::kMouseMove));
    const auto d3 = decodeInputMessage(*mw, kMaxX, kMaxY);
    CHECK(d3.has_value());
    CHECK_EQ(static_cast<unsigned>(d3->type), static_cast<unsigned>(InputType::kMouseWheel));
    CHECK_EQ(d3->wheelDelta, 1);

    // 解码：非法 payload（手工构造坏 wire，仍经真实 decoder 通道）
    auto badPayload = [](uint8_t type, std::vector<uint8_t> p) {
        return makeMessage(type, 0, std::move(p));
    };
    // 长度错误
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputKey),
                                         std::vector<uint8_t>(7, 0)),
                              kMaxX, kMaxY)
               .has_value());
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputMouse),
                                         std::vector<uint8_t>(9, 0)),
                              kMaxX, kMaxY)
               .has_value());
    // keycode 非法
    std::vector<uint8_t> p(8, 0);
    putU32(p, 0x03);
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputKey), p), kMaxX, kMaxY).has_value());
    // modifiers 非法
    p = std::vector<uint8_t>(8, 0);
    putU32(p, 0x04);
    putU16(p, 0x10);
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputKey), p), kMaxX, kMaxY).has_value());
    // down 非法
    p = std::vector<uint8_t>(8, 0);
    putU32(p, 0x04);
    p[6] = 2;
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputKey), p), kMaxX, kMaxY).has_value());
    // rsvd 非法
    p = std::vector<uint8_t>(8, 0);
    putU32(p, 0x04);
    p[7] = 1;
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputKey), p), kMaxX, kMaxY).has_value());
    // buttons 非法
    p = std::vector<uint8_t>(8, 0);
    p[0] = 0x08;
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputMouse), p), kMaxX, kMaxY).has_value());
    // 坐标越界（x = 320 = width → 拒绝）
    p = std::vector<uint8_t>(8, 0);
    putU16(p, kDisplayW);
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputMouse), p), kMaxX, kMaxY).has_value());
    // flags != ABS
    p = std::vector<uint8_t>(8, 0);
    p[6] = 0;
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputMouse), p), kMaxX, kMaxY).has_value());
    p[6] = 2;
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kInputMouse), p), kMaxX, kMaxY).has_value());
    // 非输入类型
    CHECK(!decodeInputMessage(badPayload(static_cast<uint8_t>(MessageType::kHello), std::vector<uint8_t>(8, 0)),
                              kMaxX, kMaxY)
               .has_value());
}

// ---- 真实协议 round-trip（spec §22）----
void testProtocolRoundTrip() {
    std::printf("  protocol round-trip (Encoder → Decoder)\n");

    // 键盘：类型 + 字段完整一致
    const InputEvent keyEvs[] = {
        makeKeyEvent(InputType::kKeyDown, 0x04, 0, 0),
        makeKeyEvent(InputType::kKeyUp, 0x04, 0, 0),
        makeKeyEvent(InputType::kKeyDown, 0x1D, static_cast<uint16_t>(kModCtrl | kModShift), 0),
        makeKeyEvent(InputType::kKeyDown, 0xE0, kModCtrl, 0),
        makeKeyEvent(InputType::kKeyUp, 0xE7, 0, 0),
    };
    for (const InputEvent& ev : keyEvs) {
        const auto msg = encodeInputEvent(ev, kMaxX, kMaxY);
        CHECK(msg.has_value());
        const RoundTripResult rt = wireRoundTrip(*msg);
        CHECK(rt.decoded.has_value());
        if (rt.decoded.has_value()) {
            const auto back = decodeInputMessage(*rt.decoded, kMaxX, kMaxY);
            CHECK(back.has_value());
            if (back.has_value()) {
                CHECK_EQ(static_cast<unsigned>(back->type), static_cast<unsigned>(ev.type));
                CHECK_EQ(back->keycode, ev.keycode);
                CHECK_EQ(back->modifiers, ev.modifiers);
            }
        }
    }

    // 鼠标：wire 字段逐位一致；Move/Wheel 类型可还原
    const InputEvent mouseEvs[] = {
        makeMouseMove(0, 0, 0, 0),
        makeMouseMove(319, 239, kMouseLeft, 0),
        makeMouseMove(160, 120, static_cast<uint8_t>(kMouseLeft | kMouseRight), 0),
        makeMouseWheel(160, 120, 1, 0, 0),
        makeMouseWheel(160, 120, -1, kMouseLeft, 0),
    };
    for (const InputEvent& ev : mouseEvs) {
        const auto msg = encodeInputEvent(ev, kMaxX, kMaxY);
        CHECK(msg.has_value());
        const RoundTripResult rt = wireRoundTrip(*msg);
        CHECK(rt.decoded.has_value());
        if (rt.decoded.has_value()) {
            const auto back = decodeInputMessage(*rt.decoded, kMaxX, kMaxY);
            CHECK(back.has_value());
            if (back.has_value()) {
                CHECK_EQ(back->x, ev.x);
                CHECK_EQ(back->y, ev.y);
                CHECK_EQ(back->buttons, ev.buttons);
                CHECK_EQ(back->wheelDelta, ev.wheelDelta);
                const unsigned expectType =
                    ev.wheelDelta != 0 ? static_cast<unsigned>(InputType::kMouseWheel)
                                       : static_cast<unsigned>(InputType::kMouseMove);
                CHECK_EQ(static_cast<unsigned>(back->type), expectType);
            }
        }
    }

    // 语义 round-trip：真实 wire 解码 → InputManager 推导 Down/Move/Up/Wheel
    InputManager mgr(kDisplayW, kDisplayH);
    CollectListener listener;
    mgr.registerListener(&listener);
    const InputEvent seq[] = {
        makeMouseMove(10, 10, 0, 0),
        makeMouseMove(10, 10, kMouseLeft, 0),      // → Down
        makeMouseMove(20, 20, kMouseLeft, 0),      // → Move
        makeMouseMove(20, 20, 0, 0),               // → Up
        makeMouseWheel(20, 20, 1, 0, 0),           // → Wheel
    };
    for (const InputEvent& ev : seq) {
        const auto msg = encodeInputEvent(ev, kMaxX, kMaxY);
        CHECK(msg.has_value());
        const RoundTripResult rt = wireRoundTrip(*msg);
        CHECK(rt.decoded.has_value());
        if (rt.decoded.has_value()) {
            const auto back = decodeInputMessage(*rt.decoded, kMaxX, kMaxY);
            CHECK(back.has_value());
            if (back.has_value()) {
                mgr.feed(*back);
            }
        }
    }
    CHECK_EQ(listener.events.size(), 5u);
    CHECK_EQ(static_cast<unsigned>(listener.events[0].type),
             static_cast<unsigned>(InputType::kMouseMove));
    CHECK_EQ(static_cast<unsigned>(listener.events[1].type),
             static_cast<unsigned>(InputType::kMouseDown));
    CHECK_EQ(listener.events[1].buttons, kMouseLeft);
    CHECK_EQ(static_cast<unsigned>(listener.events[2].type),
             static_cast<unsigned>(InputType::kMouseMove));
    CHECK_EQ(static_cast<unsigned>(listener.events[3].type),
             static_cast<unsigned>(InputType::kMouseUp));
    CHECK_EQ(static_cast<unsigned>(listener.events[4].type),
             static_cast<unsigned>(InputType::kMouseWheel));
    CHECK_EQ(listener.events[4].wheelDelta, 1);
}

// autoRepeat 策略（spec §9）
void testAutoRepeatPolicy() {
    std::printf("  autorepeat policy\n");
    // KeyDown + autoRepeat → 忽略；KeyUp 始终发送；普通 KeyDown 发送
    CHECK(!InputPolicy::acceptKey(true, true));
    CHECK(InputPolicy::acceptKey(true, false));
    CHECK(InputPolicy::acceptKey(false, true));
    CHECK(InputPolicy::acceptKey(false, false));
}

}  // namespace

void runInputTests() {
    std::printf("[input]\n");
    testKeyboardMapper();
    testCoordinateMapper();
    testWheelNormalization();
    testMouseMoveThrottle();
    testInputManager();
    testCodecEncodeDecode();
    testProtocolRoundTrip();
    testAutoRepeatPolicy();
}
