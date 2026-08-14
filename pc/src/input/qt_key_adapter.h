// ESPView M3 — Qt 输入适配（Qt::Key / Qt 修饰键 / Qt 鼠标键 → 平台无关表示）。
//
// 规范来源：spec §4/§5/§6（InputEvent 唯一内部模型；不在业务层传播 Qt::Key /
//   Windows VK_*）。本文件是「Qt 事件源 → 共享输入层」的唯一 Qt 依赖边界：
//   - Qt::Key → HostKey（逻辑键；Qt::Key 数值为稳定公开枚举，经 switch 显式映射）；
//   - Qt::KeyboardModifiers → modifiers 位掩码（bit0=Ctrl,1=Shift,2=Alt,3=GUI）；
//   - Qt::MouseButtons / Qt::MouseButton → buttons 位掩码
//     （bit0=LEFT, bit1=RIGHT, bit2=MIDDLE）。
// 语义不明确的键 → HostKey::kUnknown（unsupported，不发送，spec §7）。

#pragma once

#include <QtCore/qnamespace.h>  // Qt::KeyboardModifiers / Qt::MouseButtons / Qt::MouseButton

#include "input_event.h"
#include "keyboard_mapper.h"  // HostKey

namespace espview {
namespace input {

// Qt::Key 数值 → HostKey。返回 kUnknown = 不支持（不发送）。
HostKey toHostKey(int qtKey);

// Qt 修饰键状态 → 协议修饰位掩码（kModCtrl/kModShift/kModAlt/kModGui）。
uint16_t modifiersFromQt(Qt::KeyboardModifiers mods);

// Qt 当前按住按钮状态（Qt::MouseButtons）→ 协议按钮掩码。
uint8_t buttonsFromQt(Qt::MouseButtons buttons);

// Qt 单个事件按钮（Qt::MouseButton）→ 协议按钮位；非三键返回 0。
uint8_t buttonBitFromQt(Qt::MouseButton button);

}  // namespace input
}  // namespace espview
