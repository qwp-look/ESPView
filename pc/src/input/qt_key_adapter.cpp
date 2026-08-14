// ESPView M3 — qt_key_adapter 实现（见 qt_key_adapter.h）。
// Qt::Key 数值来自 Qt 公开稳定枚举（Qt::Key_* 定义于 QtCore/qnamespace.h）。

#include "qt_key_adapter.h"

#include <QtCore/qnamespace.h>  // Qt::Key / Qt::KeyboardModifiers / Qt::MouseButtons

namespace espview {
namespace input {

HostKey toHostKey(int qtKey) {
    // 字母（Qt::Key_A=0x41 .. Key_Z=0x5A）
    if (qtKey >= 'A' && qtKey <= 'Z') {
        return static_cast<HostKey>(static_cast<int>(HostKey::kA) + (qtKey - 'A'));
    }
    // 数字（Qt::Key_0=0x30 .. Key_9=0x39）
    if (qtKey >= '0' && qtKey <= '9') {
        return static_cast<HostKey>(static_cast<int>(HostKey::k0) + (qtKey - '0'));
    }

    switch (qtKey) {
        case Qt::Key_F1: return HostKey::kF1;
        case Qt::Key_F2: return HostKey::kF2;
        case Qt::Key_F3: return HostKey::kF3;
        case Qt::Key_F4: return HostKey::kF4;
        case Qt::Key_F5: return HostKey::kF5;
        case Qt::Key_F6: return HostKey::kF6;
        case Qt::Key_F7: return HostKey::kF7;
        case Qt::Key_F8: return HostKey::kF8;
        case Qt::Key_F9: return HostKey::kF9;
        case Qt::Key_F10: return HostKey::kF10;
        case Qt::Key_F11: return HostKey::kF11;
        case Qt::Key_F12: return HostKey::kF12;
        case Qt::Key_Escape: return HostKey::kEscape;
        case Qt::Key_Return: return HostKey::kEnter;   // 主键盘 Enter
        case Qt::Key_Enter: return HostKey::kEnter;    // 数字小键盘 Enter
        case Qt::Key_Tab: return HostKey::kTab;
        case Qt::Key_Backspace: return HostKey::kBackspace;
        case Qt::Key_Space: return HostKey::kSpace;
        case Qt::Key_Left: return HostKey::kLeft;
        case Qt::Key_Right: return HostKey::kRight;
        case Qt::Key_Up: return HostKey::kUp;
        case Qt::Key_Down: return HostKey::kDown;
        case Qt::Key_Home: return HostKey::kHome;
        case Qt::Key_End: return HostKey::kEnd;
        case Qt::Key_PageUp: return HostKey::kPageUp;
        case Qt::Key_PageDown: return HostKey::kPageDown;
        case Qt::Key_Insert: return HostKey::kInsert;
        case Qt::Key_Delete: return HostKey::kDelete;
        // 修饰键：Windows 语义明确（左/右由 nativeScanCode 区分，v0.1 统一映射
        // 为左侧用法；AltGr 为右侧 Alt）。
        case Qt::Key_Control: return HostKey::kLeftCtrl;
        case Qt::Key_Shift: return HostKey::kLeftShift;
        case Qt::Key_Alt: return HostKey::kLeftAlt;
        case Qt::Key_AltGr: return HostKey::kRightAlt;
        case Qt::Key_Meta: return HostKey::kLeftGui;   // Windows 键
        case Qt::Key_CapsLock: return HostKey::kCapsLock;
        case Qt::Key_NumLock: return HostKey::kNumLock;
        case Qt::Key_ScrollLock: return HostKey::kScrollLock;
        default:
            return HostKey::kUnknown;  // 语义不明确 / 未覆盖 → unsupported
    }
}

uint16_t modifiersFromQt(Qt::KeyboardModifiers mods) {
    uint16_t m = 0;
    if (mods.testFlag(Qt::ControlModifier)) {
        m |= kModCtrl;
    }
    if (mods.testFlag(Qt::ShiftModifier)) {
        m |= kModShift;
    }
    if (mods.testFlag(Qt::AltModifier)) {
        m |= kModAlt;
    }
    if (mods.testFlag(Qt::MetaModifier)) {
        m |= kModGui;
    }
    return m;
}

uint8_t buttonsFromQt(Qt::MouseButtons buttons) {
    uint8_t b = 0;
    if (buttons.testFlag(Qt::LeftButton)) {
        b |= kMouseLeft;
    }
    if (buttons.testFlag(Qt::RightButton)) {
        b |= kMouseRight;
    }
    if (buttons.testFlag(Qt::MiddleButton)) {
        b |= kMouseMiddle;
    }
    return b;
}

uint8_t buttonBitFromQt(Qt::MouseButton button) {
    switch (button) {
        case Qt::LeftButton: return kMouseLeft;
        case Qt::RightButton: return kMouseRight;
        case Qt::MiddleButton: return kMouseMiddle;
        default: return 0;  // 仅支持三键（spec §14）
    }
}

}  // namespace input
}  // namespace espview
