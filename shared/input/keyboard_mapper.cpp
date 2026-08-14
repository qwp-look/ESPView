// ESPView M3 — KeyboardMapper 实现（见 keyboard_mapper.h）。
// 映射表：USB HID Usage Tables for USB HID Devices（Keyboard/Keypad Page 0x07）。

#include "keyboard_mapper.h"

namespace espview {
namespace input {

namespace {

struct Entry {
    HostKey key;
    uint32_t usage;
    uint16_t modifierBit;  // 0 = 非修饰键
};

// 映射表（spec §7 要求的全部键；Q 行与 W 行标准 usage）：
//   A-Z = 0x04..0x1D；0-9 = 0x1E..0x27；F1-F12 = 0x3A..0x45；
//   修饰键 = 0xE0..0xE7。
constexpr Entry kTable[] = {
    {HostKey::kA, 0x04, 0}, {HostKey::kB, 0x05, 0}, {HostKey::kC, 0x06, 0},
    {HostKey::kD, 0x07, 0}, {HostKey::kE, 0x08, 0}, {HostKey::kF, 0x09, 0},
    {HostKey::kG, 0x0A, 0}, {HostKey::kH, 0x0B, 0}, {HostKey::kI, 0x0C, 0},
    {HostKey::kJ, 0x0D, 0}, {HostKey::kK, 0x0E, 0}, {HostKey::kL, 0x0F, 0},
    {HostKey::kM, 0x10, 0}, {HostKey::kN, 0x11, 0}, {HostKey::kO, 0x12, 0},
    {HostKey::kP, 0x13, 0}, {HostKey::kQ, 0x14, 0}, {HostKey::kR, 0x15, 0},
    {HostKey::kS, 0x16, 0}, {HostKey::kT, 0x17, 0}, {HostKey::kU, 0x18, 0},
    {HostKey::kV, 0x19, 0}, {HostKey::kW, 0x1A, 0}, {HostKey::kX, 0x1B, 0},
    {HostKey::kY, 0x1C, 0}, {HostKey::kZ, 0x1D, 0},
    {HostKey::k0, 0x27, 0}, {HostKey::k1, 0x1E, 0}, {HostKey::k2, 0x1F, 0},
    {HostKey::k3, 0x20, 0}, {HostKey::k4, 0x21, 0}, {HostKey::k5, 0x22, 0},
    {HostKey::k6, 0x23, 0}, {HostKey::k7, 0x24, 0}, {HostKey::k8, 0x25, 0},
    {HostKey::k9, 0x26, 0},
    {HostKey::kF1, 0x3A, 0},  {HostKey::kF2, 0x3B, 0},  {HostKey::kF3, 0x3C, 0},
    {HostKey::kF4, 0x3D, 0},  {HostKey::kF5, 0x3E, 0},  {HostKey::kF6, 0x3F, 0},
    {HostKey::kF7, 0x40, 0},  {HostKey::kF8, 0x41, 0},  {HostKey::kF9, 0x42, 0},
    {HostKey::kF10, 0x43, 0}, {HostKey::kF11, 0x44, 0}, {HostKey::kF12, 0x45, 0},
    {HostKey::kEscape, 0x29, 0}, {HostKey::kEnter, 0x28, 0},
    {HostKey::kTab, 0x2B, 0}, {HostKey::kBackspace, 0x2A, 0}, {HostKey::kSpace, 0x2C, 0},
    {HostKey::kLeft, 0x50, 0}, {HostKey::kRight, 0x4F, 0},
    {HostKey::kUp, 0x52, 0}, {HostKey::kDown, 0x51, 0},
    {HostKey::kHome, 0x4A, 0}, {HostKey::kEnd, 0x4D, 0},
    {HostKey::kPageUp, 0x4B, 0}, {HostKey::kPageDown, 0x4E, 0},
    {HostKey::kInsert, 0x49, 0}, {HostKey::kDelete, 0x4C, 0},
    {HostKey::kLeftCtrl, 0xE0, kModCtrl},   {HostKey::kRightCtrl, 0xE4, kModCtrl},
    {HostKey::kLeftShift, 0xE1, kModShift}, {HostKey::kRightShift, 0xE5, kModShift},
    {HostKey::kLeftAlt, 0xE2, kModAlt},     {HostKey::kRightAlt, 0xE6, kModAlt},
    {HostKey::kLeftGui, 0xE3, kModGui},     {HostKey::kRightGui, 0xE7, kModGui},
    {HostKey::kCapsLock, 0x39, 0}, {HostKey::kNumLock, 0x53, 0},
    {HostKey::kScrollLock, 0x47, 0},
};

}  // namespace

bool KeyboardMapper::mapKey(HostKey key, KeyMapResult& out) {
    for (const Entry& e : kTable) {
        if (e.key == key) {
            out.supported = true;
            out.hidUsage = e.usage;
            out.modifierBit = e.modifierBit;
            return true;
        }
    }
    out.supported = false;
    out.hidUsage = 0;
    out.modifierBit = 0;
    return false;
}

}  // namespace input
}  // namespace espview
