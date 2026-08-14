// ESPView M3 — host 侧输入映射测试（Qt::Key → HostKey → HID usage 表 + Qt 掩码转换）。
//
// 规范来源：spec §6/§7/§21（KeyboardMapper 单元测试表 / Qt 键语义明确性）。
// 链接 Qt6::Core（仅取 Qt::Key / Qt::KeyboardModifiers 枚举常量），不需要 GUI。
// 覆盖：Qt::Key → HostKey → KeyboardMapper(HID usage)；未覆盖键 → kUnknown
// （unsupported，不发送）；修饰键/鼠标键掩码转换；以及共享层
// KeyboardMapper / CoordinateMapper / InputPolicy 的关键路径（完整表已在
// shared/protocol/tests/input_test.cpp 覆盖，这里做交叉验证）。

#include <cstdint>
#include <cstdio>
#include <string>

#include <QtCore/qnamespace.h>

#include "coordinate_mapper.h"
#include "input_event.h"
#include "input_policy.h"
#include "keyboard_mapper.h"
#include "qt_key_adapter.h"

namespace {

using espview::input::CoordinateMapper;
using espview::input::HostKey;
using espview::input::KeyMapResult;
using espview::input::KeyboardMapper;
using espview::input::buttonBitFromQt;
using espview::input::buttonsFromQt;
using espview::input::kModAlt;
using espview::input::kModCtrl;
using espview::input::kModGui;
using espview::input::kModShift;
using espview::input::kMouseLeft;
using espview::input::kMouseMiddle;
using espview::input::kMouseRight;
using espview::input::modifiersFromQt;
using espview::input::toHostKey;

int gChecks = 0;
int gFailures = 0;

void fail(int line, const std::string& msg) {
    ++gFailures;
    std::printf("  FAIL %s:%d %s\n", __FILE__, line, msg.c_str());
}

#define EXPECT(cond, msg)                          \
    do {                                           \
        ++gChecks;                                 \
        if (!(cond)) {                             \
            fail(__LINE__, std::string(msg));      \
        }                                          \
    } while (0)

#define EXPECT_EQ(a, b, msg) EXPECT((a) == (b), msg)

// Qt::Key → HostKey → HID usage 全表
void checkKey(int qtKey, HostKey expectHost, uint32_t expectUsage, const char* label) {
    const HostKey h = toHostKey(qtKey);
    EXPECT_EQ(static_cast<int>(h), static_cast<int>(expectHost), std::string(label) + ": toHostKey");
    KeyMapResult r;
    if (!KeyboardMapper::mapKey(h, r)) {
        EXPECT(false, std::string(label) + ": mapKey unsupported");
        return;
    }
    EXPECT_EQ(r.hidUsage, expectUsage, std::string(label) + ": hidUsage");
}

void testQtKeyTable() {
    std::printf("  qt key -> host -> hid table\n");
    checkKey(Qt::Key_A, HostKey::kA, 0x04, "A");
    checkKey(Qt::Key_B, HostKey::kB, 0x05, "B");
    checkKey(Qt::Key_Z, HostKey::kZ, 0x1D, "Z");
    checkKey(Qt::Key_0, HostKey::k0, 0x27, "0");
    checkKey(Qt::Key_1, HostKey::k1, 0x1E, "1");
    checkKey(Qt::Key_9, HostKey::k9, 0x26, "9");
    checkKey(Qt::Key_F1, HostKey::kF1, 0x3A, "F1");
    checkKey(Qt::Key_F12, HostKey::kF12, 0x45, "F12");
    checkKey(Qt::Key_Return, HostKey::kEnter, 0x28, "Return");
    checkKey(Qt::Key_Enter, HostKey::kEnter, 0x28, "Enter");
    checkKey(Qt::Key_Escape, HostKey::kEscape, 0x29, "Escape");
    checkKey(Qt::Key_Tab, HostKey::kTab, 0x2B, "Tab");
    checkKey(Qt::Key_Backspace, HostKey::kBackspace, 0x2A, "Backspace");
    checkKey(Qt::Key_Space, HostKey::kSpace, 0x2C, "Space");
    checkKey(Qt::Key_Left, HostKey::kLeft, 0x50, "Left");
    checkKey(Qt::Key_Right, HostKey::kRight, 0x4F, "Right");
    checkKey(Qt::Key_Up, HostKey::kUp, 0x52, "Up");
    checkKey(Qt::Key_Down, HostKey::kDown, 0x51, "Down");
    checkKey(Qt::Key_Home, HostKey::kHome, 0x4A, "Home");
    checkKey(Qt::Key_End, HostKey::kEnd, 0x4D, "End");
    checkKey(Qt::Key_PageUp, HostKey::kPageUp, 0x4B, "PageUp");
    checkKey(Qt::Key_PageDown, HostKey::kPageDown, 0x4E, "PageDown");
    checkKey(Qt::Key_Insert, HostKey::kInsert, 0x49, "Insert");
    checkKey(Qt::Key_Delete, HostKey::kDelete, 0x4C, "Delete");
    checkKey(Qt::Key_Control, HostKey::kLeftCtrl, 0xE0, "Ctrl");
    checkKey(Qt::Key_Shift, HostKey::kLeftShift, 0xE1, "Shift");
    checkKey(Qt::Key_Alt, HostKey::kLeftAlt, 0xE2, "Alt");
    checkKey(Qt::Key_AltGr, HostKey::kRightAlt, 0xE6, "AltGr");
    checkKey(Qt::Key_Meta, HostKey::kLeftGui, 0xE3, "Meta/GUI");
    checkKey(Qt::Key_CapsLock, HostKey::kCapsLock, 0x39, "CapsLock");
    checkKey(Qt::Key_NumLock, HostKey::kNumLock, 0x53, "NumLock");
    checkKey(Qt::Key_ScrollLock, HostKey::kScrollLock, 0x47, "ScrollLock");

    // 修饰键位
    KeyMapResult mr;
    KeyboardMapper::mapKey(toHostKey(Qt::Key_Control), mr);
    EXPECT_EQ(mr.modifierBit, kModCtrl, "ctrl modifier bit");
    KeyboardMapper::mapKey(toHostKey(Qt::Key_Shift), mr);
    EXPECT_EQ(mr.modifierBit, kModShift, "shift modifier bit");
    KeyboardMapper::mapKey(toHostKey(Qt::Key_Alt), mr);
    EXPECT_EQ(mr.modifierBit, kModAlt, "alt modifier bit");
    KeyboardMapper::mapKey(toHostKey(Qt::Key_Meta), mr);
    EXPECT_EQ(mr.modifierBit, kModGui, "gui modifier bit");

    // 未覆盖键 → kUnknown（unsupported，不发送）
    EXPECT_EQ(static_cast<int>(toHostKey(Qt::Key_F13)), static_cast<int>(HostKey::kUnknown),
              "F13 unsupported");
    EXPECT_EQ(static_cast<int>(toHostKey(Qt::Key_unknown)), static_cast<int>(HostKey::kUnknown),
              "unknown unsupported");
    EXPECT_EQ(static_cast<int>(toHostKey(Qt::Key_Menu)), static_cast<int>(HostKey::kUnknown),
              "Menu unsupported");
    EXPECT_EQ(static_cast<int>(toHostKey(Qt::Key_MediaPlay)), static_cast<int>(HostKey::kUnknown),
              "MediaPlay unsupported");
}

void testQtMasks() {
    std::printf("  qt modifier/button masks\n");
    EXPECT_EQ(modifiersFromQt(Qt::NoModifier), 0u, "no modifiers");
    EXPECT_EQ(modifiersFromQt(Qt::ControlModifier), kModCtrl, "ctrl");
    EXPECT_EQ(modifiersFromQt(Qt::ShiftModifier), kModShift, "shift");
    EXPECT_EQ(modifiersFromQt(Qt::AltModifier), kModAlt, "alt");
    EXPECT_EQ(modifiersFromQt(Qt::MetaModifier), kModGui, "gui");
    EXPECT_EQ(modifiersFromQt(Qt::ControlModifier | Qt::ShiftModifier),
              static_cast<uint16_t>(kModCtrl | kModShift), "ctrl+shift");

    EXPECT_EQ(buttonsFromQt(Qt::NoButton), 0u, "no buttons");
    EXPECT_EQ(buttonsFromQt(Qt::LeftButton), kMouseLeft, "left");
    EXPECT_EQ(buttonsFromQt(Qt::RightButton), kMouseRight, "right");
    EXPECT_EQ(buttonsFromQt(Qt::MiddleButton), kMouseMiddle, "middle");
    EXPECT_EQ(buttonsFromQt(Qt::LeftButton | Qt::RightButton),
              static_cast<uint8_t>(kMouseLeft | kMouseRight), "left+right");

    EXPECT_EQ(buttonBitFromQt(Qt::LeftButton), kMouseLeft, "bit left");
    EXPECT_EQ(buttonBitFromQt(Qt::RightButton), kMouseRight, "bit right");
    EXPECT_EQ(buttonBitFromQt(Qt::MiddleButton), kMouseMiddle, "bit middle");
    EXPECT_EQ(buttonBitFromQt(Qt::BackButton), 0u, "bit back -> 0 (unsupported)");
}

void testSharedCrossCheck() {
    std::printf("  shared mapper cross-check\n");
    int ox = -1, oy = -1;
    EXPECT(CoordinateMapper::mapPoint(480, 360, 960, 720, 320, 240, ox, oy), "960x720 center");
    EXPECT_EQ(ox, 160, "ox");
    EXPECT_EQ(oy, 120, "oy");
    // 800x500（非 4:3）→ 左右 letterbox；x=0 在黑边内 → 不发送
    EXPECT(!CoordinateMapper::mapPoint(0, 250, 800, 500, 320, 240, ox, oy), "left bar");
    EXPECT(CoordinateMapper::mapPoint(400, 250, 800, 500, 320, 240, ox, oy), "inside 800x500");
    EXPECT_EQ(ox, 160, "ox inside");
    EXPECT_EQ(oy, 120, "oy inside");
    EXPECT_EQ(static_cast<int>(espview::input::normalizeWheelDelta(240)), 2, "wheel 240");
    EXPECT_EQ(static_cast<int>(espview::input::normalizeWheelDelta(-120)), -1, "wheel -120");
    EXPECT(!espview::input::InputPolicy::acceptKey(true, true), "autorepeat ignored");
    EXPECT(espview::input::InputPolicy::acceptKey(false, true), "keyup always");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView M3 input_mapper_test ==\n");
    testQtKeyTable();
    testQtMasks();
    testSharedCrossCheck();
    std::printf("----\nchecks: %d, failures: %d\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
