// ESPView M5-B — HID→LVGL key 映射实现（见 hid_lvgl_keymap.h）。

#include "hid_lvgl_keymap.h"

#include <cstdio>
#include <cstring>

namespace espview {
namespace input {

namespace {

// 局部命名空间：线性表比 switch 更易维护，32 条以内直接线性查找。
struct HidKeyEntry {
    uint32_t hid;
    uint32_t lvgl;
};

constexpr HidKeyEntry kEntries[] = {
    // 字母 A..Z → ASCII 'A'..'Z'（0x04..0x1D）
    {0x04, 'A'}, {0x05, 'B'}, {0x06, 'C'}, {0x07, 'D'}, {0x08, 'E'},
    {0x09, 'F'}, {0x0A, 'G'}, {0x0B, 'H'}, {0x0C, 'I'}, {0x0D, 'J'},
    {0x0E, 'K'}, {0x0F, 'L'}, {0x10, 'M'}, {0x11, 'N'}, {0x12, 'O'},
    {0x13, 'P'}, {0x14, 'Q'}, {0x15, 'R'}, {0x16, 'S'}, {0x17, 'T'},
    {0x18, 'U'}, {0x19, 'V'}, {0x1A, 'W'}, {0x1B, 'X'}, {0x1C, 'Y'},
    {0x1D, 'Z'},
    // 数字行 1..0 → ASCII '1'..'0'（0x1E..0x27）
    {0x1E, '1'}, {0x1F, '2'}, {0x20, '3'}, {0x21, '4'}, {0x22, '5'},
    {0x23, '6'}, {0x24, '7'}, {0x25, '8'}, {0x26, '9'}, {0x27, '0'},
    // 控制键
    {0x28, kLvglKeyEnter},      // Enter
    {0x29, kLvglKeyEsc},        // Escape
    {0x2A, kLvglKeyBackspace},  // Backspace
    {0x2B, kLvglKeyNext},       // Tab → LVGL focus next
    {0x2C, ' '},                // Space
    // 标点（HID 标准顺序）
    {0x2D, '-'}, {0x2E, '='}, {0x2F, '['}, {0x30, ']'}, {0x31, '\\'},
    {0x33, ';'}, {0x34, '\''}, {0x35, '`'}, {0x36, ','}, {0x37, '.'},
    {0x38, '/'},
    // 导航/编辑键
    {0x4A, kLvglKeyHome},       // Home
    {0x4C, kLvglKeyDel},        // Delete
    {0x4D, kLvglKeyEnd},        // End
    {0x4F, kLvglKeyRight},      // Right（HID 0x4F）
    {0x50, kLvglKeyLeft},       // Left（HID 0x50）
    {0x51, kLvglKeyDown},       // Down（HID 0x51）
    {0x52, kLvglKeyUp},         // Up（HID 0x52）
    // Keypad（wire 允许 0x54..0x63）
    {0x54, '/'}, {0x55, '*'}, {0x56, '-'}, {0x57, '+'}, {0x58, kLvglKeyEnter},
    {0x59, '1'}, {0x5A, '2'}, {0x5B, '3'}, {0x5C, '4'}, {0x5D, '5'},
    {0x5E, '6'}, {0x5F, '7'}, {0x60, '8'}, {0x61, '9'}, {0x62, '0'},
    {0x63, '.'},
};

}  // namespace

bool isSupportedLvglKey(uint32_t hidUsage) {
    for (const HidKeyEntry& e : kEntries) {
        if (e.hid == hidUsage) {
            return true;
        }
    }
    return false;
}

bool HidToLvglKeyMapper::mapKey(uint32_t hidUsage, uint32_t& lvglKey) {
    for (const HidKeyEntry& e : kEntries) {
        if (e.hid == hidUsage) {
            lvglKey = e.lvgl;
            return true;
        }
    }
    return false;  // 修饰键 0xE0..0xE7、F1..F12、PageUp/Down、Insert、CapsLock 等
}

const char* HidToLvglKeyMapper::keyName(uint32_t lvglKey) {
    switch (lvglKey) {
        case kLvglKeyUp: return "UP";
        case kLvglKeyDown: return "DOWN";
        case kLvglKeyRight: return "RIGHT";
        case kLvglKeyLeft: return "LEFT";
        case kLvglKeyEsc: return "ESC";
        case kLvglKeyDel: return "DEL";
        case kLvglKeyBackspace: return "BKSP";
        case kLvglKeyEnter: return "ENTER";
        case kLvglKeyNext: return "TAB";
        case kLvglKeyPrev: return "PREV";
        case kLvglKeyHome: return "HOME";
        case kLvglKeyEnd: return "END";
        default:
            if (lvglKey >= 32 && lvglKey < 127) {
                static char s[2] = {0, 0};
                s[0] = static_cast<char>(lvglKey);
                return s;
            }
            static char hex[8];
            std::snprintf(hex, sizeof(hex), "0x%02X", static_cast<unsigned>(lvglKey));
            return hex;
    }
}

}  // namespace input
}  // namespace espview
