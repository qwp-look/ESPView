// ESPView M3 — InputEvent ⇄ Message 编解码（纯 C++17，两端共用）。
//
// 规范来源：docs/DESIGN.md E 节 INPUT_KEY / INPUT_MOUSE Layout + B.4（InputEvent）。
// 职责：在 InputEvent（内部统一模型）与 INPUT_* 协议 Message 之间做确定性转换，
//       并执行 wire 级校验（拒绝非法 payload / 坐标 / buttons / modifiers /
//       keycode / flags / rsvd）。本层不接触 Packet/CRC（那是 MessageEncoder 的事），
//       只调用 message.h 已有的 makeInputKey / makeInputMouse builder。
//
// 重要：INPUT_MOUSE wire 上只携带「当前指针状态」（buttons 掩码 / x / y / wheel /
//   flags），没有事件类型字段（DESIGN.md INPUT_MOUSE 表）。因此：
//   - encode：MouseMove / MouseDown / MouseUp / MouseWheel 都编码成同一 8B 布局，
//     buttons = 当前按下状态掩码（spec §14 状态语义，由发送端 InputController 维护）；
//   - decode：INPUT_MOUSE 解出的 InputEvent.type 按 wheel 区分：wheel != 0 →
//     kMouseWheel，否则 kMouseMove；MouseDown / MouseUp 由接收端 InputManager
//     按 buttons 掩码相对上一状态的「变化位」推导（见 input_manager.h 文档）。
//
// 校验规则（spec §20 非法输入，与 InputManager 一致）：
//   - INPUT_KEY：payload 恰 8B；keycode ∈ {0x04..0x65} ∪ {0xE0..0xE7}；
//     modifiers ≤ 0x0F；down ∈ {0,1}；rsvd == 0。
//   - INPUT_MOUSE：payload 恰 8B；buttons ≤ 0x07；x ≤ maxX、y ≤ maxY；
//     flags == kMouseFlagAbs(0x01)；rsvd == 0。
//   - 其它 Message 类型 / 未知类型 → nullopt。
//
// 时间戳：INPUT_* 不上 wire（DESIGN.md 未定义），encode 忽略 InputEvent.timestampMs，
//   decode 得到 timestampMs == 0，由接收方（InputManager）本地赋值。

#pragma once

#include <cstdint>
#include <optional>

#include "input_event.h"
#include "message.h"

namespace espview {
namespace input {

// INPUT_MOUSE.flags：ABS = 1（v0.1 恒为 1，DESIGN.md INPUT_MOUSE 表）。
inline constexpr uint8_t kMouseFlagAbs = 0x01;

// InputEvent → INPUT_KEY / INPUT_MOUSE Message（先校验，非法返回 nullopt）。
// maxX / maxY = 显示分辨率减 1（鼠标事件坐标上界；键事件不校验坐标）。
std::optional<proto::Message> encodeInputEvent(const InputEvent& e, uint16_t maxX, uint16_t maxY);

// Message → InputEvent（先校验，非法返回 nullopt）。maxX / maxY 同上。
// INPUT_MOUSE 返回类型：wheel != 0 → kMouseWheel，否则 kMouseMove
// （Down/Up 需由 InputManager 按按钮掩码变化推导）。
std::optional<InputEvent> decodeInputMessage(const proto::Message& msg, uint16_t maxX,
                                             uint16_t maxY);

}  // namespace input
}  // namespace espview
