// ESPView M7-C3 — PhysicalStatus 解析/合并宿主测试。
//
// 覆盖（任务书：每种行的解析、混合行合并、坏行忽略、clamp）：
//   1. oled / trx / mem / disp / sess / mod 六类行的字段解析；
//   2. 混合行合并到同一快照（mergePhysicalStatus + 逐行 parse 累积）；
//   3. 坏行忽略（首 token 不匹配 → false，out 不变）；
//   4. 数字 clamp（无符号溢出 / 有符号 / 窄化 / fps 定点 / 语义枚举越界）。
// 纯 C++17 零平台依赖；不触碰协议 wire / WorkerStats。
//
// 注册：由主代理在 test_main.cpp 声明 runPhysicalStatusTests() 并调用
// （本文件不编辑 test_main / CMakeLists）。

#include <cstdint>
#include <limits>
#include <string_view>

#include "physical_status.h"
#include "test_util.h"

namespace {

using espview::display::controllerCodeName;
using espview::display::mergePhysicalStatus;
using espview::display::OledControllerCode;
using espview::display::parsePhysicalStatusLine;
using espview::display::PhysicalStatus;

void testOledLine() {
    // 真实行（M7-B/C2 实机）：SSD1306、ok=1。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("oled a=0x3C c=SSD1306 err=0 ok=1", s));
        CHECK(s.oledValid);
        CHECK_EQ(s.oledAddress, 0x3C);
        CHECK_EQ(s.oledController, OledControllerCode::kSsd1306);
        CHECK_EQ(s.oledErrCount, 0u);
        CHECK(s.oledOk);
    }
    // SH1106、ok=0；地址小写 hex、无 0x 前缀也接受。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("oled a=0X3d c=SH1106 err=42 ok=0", s));
        CHECK_EQ(s.oledAddress, 0x3D);
        CHECK_EQ(s.oledController, OledControllerCode::kSh1106);
        CHECK_EQ(s.oledErrCount, 42u);
        CHECK(!s.oledOk);
    }
    // 未知控制器 → kUnknown；AUTO 映射。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("oled a=0x3C c=UNKNOWN_CTRL err=1 ok=1", s));
        CHECK_EQ(s.oledController, OledControllerCode::kUnknown);
        PhysicalStatus s2;
        CHECK(parsePhysicalStatusLine("oled a=0x3C c=AUTO err=0 ok=0", s2));
        CHECK_EQ(s2.oledController, OledControllerCode::kAuto);
    }
    // 仅前缀也匹配（容错：字段缺失不报错）。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("oled", s));
        CHECK(s.oledValid);
        CHECK(!s.oledOk);
    }
    CHECK_EQ(static_cast<int>(controllerCodeName(OledControllerCode::kSsd1306)[0]),
             static_cast<int>('S'));
}

void testTrxLine() {
    // 真实行：UART、rssi=-24 ch=6；只取 rssi/ch，其余键忽略。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine(
            "trx tr=0 st=2 sw=0 rc=0 tx=0 rx=0 rssi=-24 ch=6", s));
        CHECK(s.transportValid);
        CHECK_EQ(s.rssiDbm, -24);
        CHECK_EQ(s.channel, 6u);
        CHECK(!s.oledValid);  // 其它组不受影响
        CHECK(!s.sessionValid);
    }
    // 无信号：rssi=-128 ch=0。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("trx tr=1 st=0 sw=0 rc=0 tx=0 rx=0 rssi=-128 ch=0", s));
        CHECK_EQ(s.rssiDbm, -128);
        CHECK_EQ(s.channel, 0u);
    }
    // rssi 带 '+' 号。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("trx rssi=+5 ch=11", s));
        CHECK_EQ(s.rssiDbm, 5);
        CHECK_EQ(s.channel, 11u);
    }
}

void testMemLine() {
    PhysicalStatus s;
    CHECK(parsePhysicalStatusLine("mem h=167008 lg=110592 mn=155160", s));
    CHECK(s.memValid);
    CHECK_EQ(s.heapFree, 167008u);
    CHECK_EQ(s.heapLargest, 110592u);
    CHECK_EQ(s.heapMinFree, 155160u);
}

void testDispLine() {
    // 真实行：f=0.00 → 0 百分位。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine(
            "disp id=4 t=0 r=2 b=30720 e=2405 f=0.00 d=1 q=334", s));
        CHECK(s.displayValid);
        CHECK_EQ(s.lastFrameId, 4u);
        CHECK_EQ(s.lastFrameType, 0u);
        CHECK_EQ(s.lastRectCount, 2u);
        CHECK_EQ(s.lastFrameBytes, 30720u);
        CHECK_EQ(s.lastFrameElapsedMs, 2405u);
        CHECK_EQ(s.fpsHundredths, 0u);
        CHECK_EQ(s.framesDropped, 1u);
        CHECK_EQ(s.queueFullEvents, 334u);
    }
    // PARTIAL 帧 + fps 定点。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine(
            "disp id=65530 t=1 r=9 b=1536 e=800 f=1.25 d=0 q=0", s));
        CHECK_EQ(s.lastFrameId, 65530u);
        CHECK_EQ(s.lastFrameType, 1u);
        CHECK_EQ(s.fpsHundredths, 125u);
    }
    // 1 位小数 / 无小数。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("disp f=0.5", s));
        CHECK_EQ(s.fpsHundredths, 50u);
        PhysicalStatus s2;
        CHECK(parsePhysicalStatusLine("disp f=12", s2));
        CHECK_EQ(s2.fpsHundredths, 1200u);
    }
}

void testSessLine() {
    // 真实行：st=3 h=1/1 p=9/10 → hello/ping 均 OK。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("sess st=3 h=1/1 p=9/10", s));
        CHECK(s.sessionValid);
        CHECK_EQ(s.sessionState, 3u);
        CHECK(s.helloOk);
        CHECK(s.pingOk);
    }
    // 未交换：h=0/0 p=0/0。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("sess st=0 h=0/0 p=0/0", s));
        CHECK_EQ(s.sessionState, 0u);
        CHECK(!s.helloOk);
        CHECK(!s.pingOk);
    }
    // 半握手：txHello=1 rxHello=0 → helloOk=false；收到对端 PING → pingOk=true。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("sess st=2 h=1/0 p=3/5", s));
        CHECK(!s.helloOk);
        CHECK(s.pingOk);
    }
    // h 缺 '/' 的坏值不更新 helloOk。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("sess st=3 h=1 p=0/0", s));
        CHECK(!s.helloOk);
        CHECK(!s.pingOk);
    }
}

void testModLine() {
    // 真实行（M7-C2 F11 调试钩子）：sw=3 st=3 scene=0。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("mod sw=3 st=3 scene=0", s));
        CHECK(s.modeValid);
        CHECK_EQ(s.mode, 3u);
        CHECK_EQ(s.routerState, 3u);
        CHECK_EQ(s.physicalScene, 0u);
    }
    // Application 场景 + Connected 路由。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("mod sw=2 st=2 scene=1", s));
        CHECK_EQ(s.mode, 2u);
        CHECK_EQ(s.routerState, 2u);
        CHECK_EQ(s.physicalScene, 1u);
    }
    // 无 router：st=-1 → 0xFF；scene=-1 → 0xFF。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("mod sw=0 st=-1 scene=-1", s));
        CHECK_EQ(s.mode, 0u);
        CHECK_EQ(s.routerState, 0xFFu);
        CHECK_EQ(s.physicalScene, 0xFFu);
    }
}

void testBadLines() {
    // 首 token 不匹配 → false 且 out 不变。
    PhysicalStatus s;
    s.oledAddress = 0x3C;
    s.oledValid = true;
    s.heapFree = 100;
    s.memValid = true;
    const PhysicalStatus before = s;

    CHECK(!parsePhysicalStatusLine("", s));
    CHECK(!parsePhysicalStatusLine("   ", s));
    CHECK(!parsePhysicalStatusLine("hello world", s));
    CHECK(!parsePhysicalStatusLine("oledx a=0x3C", s));       // 前缀近似不算
    CHECK(!parsePhysicalStatusLine("sess2 q=1/2 e=3 c=4 s=5", s));  // 未授权行
    CHECK(!parsePhysicalStatusLine("bogus key=value", s));

    CHECK_EQ(s.oledAddress, before.oledAddress);
    CHECK_EQ(s.oledValid, before.oledValid);
    CHECK_EQ(s.heapFree, before.heapFree);
    CHECK_EQ(s.memValid, before.memValid);
    CHECK_EQ(s.sessionValid, before.sessionValid);

    // 空 PhysicalStatus 上坏行不产生任何 valid 标志。
    PhysicalStatus fresh;
    CHECK(!parsePhysicalStatusLine("garbage", fresh));
    CHECK(!fresh.anyValid());
}

void testMerge() {
    // 逐行 parse 到独立快照，再合并到同一累积快照。
    PhysicalStatus acc;
    PhysicalStatus t;
    CHECK(parsePhysicalStatusLine("oled a=0x3C c=SSD1306 err=3 ok=1", t));
    mergePhysicalStatus(t, acc);
    CHECK(parsePhysicalStatusLine("trx tr=1 st=2 sw=0 rc=0 tx=0 rx=0 rssi=-60 ch=6", t));
    mergePhysicalStatus(t, acc);
    CHECK(parsePhysicalStatusLine("mem h=100000 lg=90000 mn=80000", t));
    mergePhysicalStatus(t, acc);
    CHECK(parsePhysicalStatusLine("disp id=9 t=1 r=3 b=5120 e=900 f=2.10 d=2 q=1", t));
    mergePhysicalStatus(t, acc);
    CHECK(parsePhysicalStatusLine("sess st=3 h=1/1 p=4/4", t));
    mergePhysicalStatus(t, acc);
    CHECK(parsePhysicalStatusLine("mod sw=3 st=3 scene=0", t));
    mergePhysicalStatus(t, acc);

    CHECK(acc.anyValid());
    CHECK(acc.oledValid);
    CHECK_EQ(acc.oledAddress, 0x3C);
    CHECK_EQ(acc.oledErrCount, 3u);
    CHECK(acc.oledOk);
    CHECK(acc.transportValid);
    CHECK_EQ(acc.rssiDbm, -60);
    CHECK_EQ(acc.channel, 6u);
    CHECK(acc.memValid);
    CHECK_EQ(acc.heapFree, 100000u);
    CHECK_EQ(acc.heapLargest, 90000u);
    CHECK_EQ(acc.heapMinFree, 80000u);
    CHECK(acc.displayValid);
    CHECK_EQ(acc.lastFrameId, 9u);
    CHECK_EQ(acc.fpsHundredths, 210u);
    CHECK(acc.sessionValid);
    CHECK_EQ(acc.sessionState, 3u);
    CHECK(acc.helloOk);
    CHECK(acc.pingOk);
    CHECK(acc.modeValid);
    CHECK_EQ(acc.mode, 3u);
    CHECK_EQ(acc.routerState, 3u);

    // 合并只覆盖 src 有效的组：再次只发 oled 行，其余组保持。
    PhysicalStatus oledOnly;
    CHECK(parsePhysicalStatusLine("oled a=0x3D c=SH1106 err=7 ok=0", oledOnly));
    mergePhysicalStatus(oledOnly, acc);
    CHECK_EQ(acc.oledAddress, 0x3D);
    CHECK_EQ(acc.rssiDbm, -60);  // 未被覆盖
    CHECK_EQ(acc.heapFree, 100000u);
    CHECK(acc.oledValid);
    CHECK(acc.transportValid);
    CHECK(acc.memValid);
}

void testClamp() {
    // 无符号溢出 → UINT64_MAX（mem 行）。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine(
            "mem h=99999999999999999999999999 lg=99999999999999999999 mn=99999999999999999999999",
            s));
        CHECK(s.heapFree == std::numeric_limits<uint64_t>::max());
        CHECK(s.heapLargest == std::numeric_limits<uint64_t>::max());
        CHECK(s.heapMinFree == std::numeric_limits<uint64_t>::max());
    }
    // oled：地址窄化 clamp 到 255；err 溢出 clamp。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine(
            "oled a=0xFFFF c=SSD1306 err=999999999999999999999999999 ok=1", s));
        CHECK_EQ(s.oledAddress, 0xFFu);
        CHECK(s.oledErrCount == std::numeric_limits<uint64_t>::max());
    }
    // trx：rssi 负向 clamp 到 -128；ch 窄化 clamp 到 255。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine(
            "trx tr=0 st=0 sw=0 rc=0 tx=0 rx=0 rssi=-999 ch=999", s));
        CHECK_EQ(s.rssiDbm, -128);
        CHECK_EQ(s.channel, 0xFFu);
        PhysicalStatus s2;
        CHECK(parsePhysicalStatusLine(
            "trx tr=0 st=0 sw=0 rc=0 tx=0 rx=0 rssi=999 ch=999", s2));
        CHECK_EQ(s2.rssiDbm, 127);
    }
    // disp：id/rects/bytes/fps/dropped 溢出窄化 clamp。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine(
            "disp id=999999999999999999999999 t=999 r=99999999999999999 "
            "b=99999999999999999 e=99999999999999999999999 f=99999999999999999.99 "
            "d=99999999999999999999999999 q=99999999999999999999999999",
            s));
        CHECK_EQ(s.lastFrameId, 0xFFFFu);
        CHECK_EQ(s.lastFrameType, 0xFFu);
        CHECK(s.lastRectCount == std::numeric_limits<uint32_t>::max());
        CHECK(s.lastFrameBytes == std::numeric_limits<uint32_t>::max());
        CHECK(s.fpsHundredths == std::numeric_limits<uint32_t>::max());
        CHECK(s.framesDropped == std::numeric_limits<uint64_t>::max());
        CHECK(s.queueFullEvents == std::numeric_limits<uint64_t>::max());
    }
    // fps 定点溢出：整数部分乘 100 超过 uint32。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("disp f=42949673.00", s));
        CHECK(s.fpsHundredths == std::numeric_limits<uint32_t>::max());
    }
    // sess：sessionState 窄化 clamp 到 255。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("sess st=999 h=0/0 p=0/0", s));
        CHECK_EQ(s.sessionState, 0xFFu);
    }
    // mod：mode 越界 clamp 到 3；st/scene 越界 → 0xFF（未知）。
    {
        PhysicalStatus s;
        CHECK(parsePhysicalStatusLine("mod sw=99 st=7 scene=5", s));
        CHECK_EQ(s.mode, 3u);
        CHECK_EQ(s.routerState, 0xFFu);
        CHECK_EQ(s.physicalScene, 0xFFu);
    }
}

}  // namespace

void runPhysicalStatusTests() {
    std::printf("[physical_status]\n");
    testOledLine();
    testTrxLine();
    testMemLine();
    testDispLine();
    testSessLine();
    testModLine();
    testBadLines();
    testMerge();
    testClamp();
}
