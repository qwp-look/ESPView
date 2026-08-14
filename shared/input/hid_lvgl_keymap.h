// ESPView M5-B — USB HID usage → LVGL key code 映射（纯 C++17，不依赖 LVGL 头文件）。
//
// 规范来源：M5-B 任务书 §10/§11（HID→LVGL mapping）+ LVGL v8.4 `lv_group.h`
// LV_KEY_* 常量。LVGL 键码与 USB HID usage 不是同一套值，因此在 shared/input
// 建立独立映射器，**不污染** shared/input/KeyboardMapper（那是 HostKey → HID
// usage 的 PC 侧映射）。
//
// LVGL v8.4 实际键码值（lv_group.h，v8.3/v8.4 相同）：
//   UP=17 DOWN=18 RIGHT=19 LEFT=20 ESC=27 DEL=127 BACKSPACE=8 ENTER=10('\n')
//   NEXT=9('\t') PREV=11 HOME=2 END=3；字母/数字直接使用 ASCII。
//
// 覆盖策略：
//   - A-Z / 0-9 / 空格 / 标点 → 对应 ASCII；
//   - Enter/Esc/Backspace/Tab/方向键/Home/End/Delete → LV_KEY_*；
//   - Keypad 数字与运算符 → 对应 ASCII（wire 允许 0x54..0x63）；
//   - 修饰键 0xE0..0xE7 → 不映射（修饰状态已在 modifiers 掩码中表达，LVGL 侧
//     不合成修饰键事件）；
//   - F1..F12 / PageUp / PageDown / Insert / CapsLock / NumLock 等 → 不映射
//     （LVGL v8 无对应概念；计数 unmapped，不 crash）。

#pragma once

#include <cstdint>

namespace espview {
namespace input {

// ---- LVGL v8.4 LV_KEY_* 值（plain 常量，避免 host 测试引入 lvgl.h）----
inline constexpr uint32_t kLvglKeyUp = 17;
inline constexpr uint32_t kLvglKeyDown = 18;
inline constexpr uint32_t kLvglKeyRight = 19;
inline constexpr uint32_t kLvglKeyLeft = 20;
inline constexpr uint32_t kLvglKeyEsc = 27;
inline constexpr uint32_t kLvglKeyDel = 127;
inline constexpr uint32_t kLvglKeyBackspace = 8;
inline constexpr uint32_t kLvglKeyEnter = 10;  // '\n'
inline constexpr uint32_t kLvglKeyNext = 9;    // '\t'（Tab → focus next）
inline constexpr uint32_t kLvglKeyPrev = 11;
inline constexpr uint32_t kLvglKeyHome = 2;
inline constexpr uint32_t kLvglKeyEnd = 3;

class HidToLvglKeyMapper {
public:
    // USB HID keyboard usage → LVGL key code。返回 false = 无映射
    // （修饰键 / F 键 / PageUp 等 LVGL v8 不表达的能力）。
    static bool mapKey(uint32_t hidUsage, uint32_t& lvglKey);

    // LVGL key code → 可读名（demo "Keyboard:" 标签；≤6 字符，未知名回退 0x%02X）。
    static const char* keyName(uint32_t lvglKey);
};

}  // namespace input
}  // namespace espview
