// ESPView M7-C4 — PhysicalCapabilitySnapshot 派生/撤销/健康分离宿主测试。
//
// 覆盖：
//   1. 初始/空快照（provenance=kNotSeen、capabilityKnown=false）；
//   2. oled 遥测学习（ok=1 → capabilityKnown + healthy + 几何/控制器/地址）；
//   3. 健康降级（ok=0 → healthy=false，capabilityKnown 保持——门控不随健康抖动）；
//   4. 跨会话撤销（reset → 全部清空，修复一次性闩锁跨会话残留）；
//   5. 控制器 → 分辨率推断（SSD1306/SH1106 → 128x64；未知 → 0x0，不伪造）；
//   6. scene 派生（mod 行 physicalScene 0/1；0xFF → Diagnostics 兜底）；
//   7. 学习结果只置不清（prev.capabilityKnown=true + 新行 ok=0 → 保持）。
// 纯 C++17 零平台依赖；不触碰协议 wire。

#include <cstdint>
#include <string_view>

#include "physical_capability_snapshot.h"
#include "physical_status.h"
#include "test_util.h"

namespace {

using espview::display::makePhysicalCapabilitySnapshot;
using espview::display::OledControllerCode;
using espview::display::parsePhysicalStatusLine;
using espview::display::PhysicalCapabilityProvenance;
using espview::display::PhysicalCapabilitySnapshot;
using espview::display::PhysicalScene;
using espview::display::PhysicalStatus;
using espview::display::resetPhysicalCapability;

// 从遥测行文本解析出 PhysicalStatus（测试助手；bad 行则返回 false）。
bool parseLine(const char* line, PhysicalStatus& out) {
    return parsePhysicalStatusLine(std::string_view(line), out);
}

void testInitial() {
    const PhysicalCapabilitySnapshot s;
    CHECK_EQ(static_cast<unsigned>(s.provenance),
             static_cast<unsigned>(PhysicalCapabilityProvenance::kNotSeen));
    CHECK(!s.capabilityKnown);
    CHECK(!s.healthy);
    CHECK(!s.telemetryFresh);
    CHECK_EQ(s.width, 0);
    CHECK_EQ(s.height, 0);
    CHECK(!s.canReadback);
    CHECK_EQ(s.controller, OledControllerCode::kUnknown);
    CHECK_EQ(s.scene, PhysicalScene::kDiagnostics);

    // reset 等价于初始（跨会话撤销后不残留）。
    const PhysicalCapabilitySnapshot r = resetPhysicalCapability();
    CHECK(!r.capabilityKnown);
    CHECK(!r.healthy);
    CHECK_EQ(static_cast<unsigned>(r.provenance),
             static_cast<unsigned>(PhysicalCapabilityProvenance::kNotSeen));
}

void testLearnFromOledOk() {
    PhysicalStatus st;
    CHECK(parseLine("oled a=0x3C c=SSD1306 err=0 ok=1", st));
    const PhysicalCapabilitySnapshot s =
        makePhysicalCapabilitySnapshot(st, PhysicalCapabilitySnapshot{});
    CHECK_EQ(static_cast<unsigned>(s.provenance),
             static_cast<unsigned>(PhysicalCapabilityProvenance::kOledTelemetry));
    CHECK(s.telemetryFresh);
    CHECK(s.capabilityKnown);
    CHECK(s.healthy);
    CHECK_EQ(s.controller, OledControllerCode::kSsd1306);
    CHECK_EQ(s.address, 0x3C);
    CHECK_EQ(s.width, 128);   // SSD1306 → 128x64（与 ESP32 侧 init 落定值一致）
    CHECK_EQ(s.height, 64);
    CHECK(s.mono);
    CHECK(!s.canReadback);
}

void testHealthDegradedKeepsCapability() {
    // 先学习（ok=1），再健康降级（ok=0）——门控必须保持，健康必须跟随。
    PhysicalStatus ok;
    CHECK(parseLine("oled a=0x3C c=SSD1306 err=0 ok=1", ok));
    const PhysicalCapabilitySnapshot learned =
        makePhysicalCapabilitySnapshot(ok, PhysicalCapabilitySnapshot{});
    CHECK(learned.capabilityKnown);
    CHECK(learned.healthy);

    PhysicalStatus bad;
    CHECK(parseLine("oled a=0x3C c=SSD1306 err=2 ok=0", bad));
    const PhysicalCapabilitySnapshot s = makePhysicalCapabilitySnapshot(bad, learned);
    CHECK(s.capabilityKnown);   // 学习结果不清除（门控不随健康抖动）
    CHECK(!s.healthy);          // 健康跟随最近遥测
    CHECK(s.telemetryFresh);
}

void testSh1106Geometry() {
    PhysicalStatus st;
    CHECK(parseLine("oled a=0x3C c=SH1106 err=0 ok=1", st));
    const PhysicalCapabilitySnapshot s =
        makePhysicalCapabilitySnapshot(st, PhysicalCapabilitySnapshot{});
    CHECK_EQ(s.controller, OledControllerCode::kSh1106);
    CHECK_EQ(s.width, 128);
    CHECK_EQ(s.height, 64);
}

void testUnknownControllerNoFakeGeometry() {
    PhysicalStatus st;
    CHECK(parseLine("oled a=0x00 c=AUTO err=0 ok=1", st));
    const PhysicalCapabilitySnapshot s =
        makePhysicalCapabilitySnapshot(st, PhysicalCapabilitySnapshot{});
    CHECK(s.capabilityKnown);
    CHECK_EQ(s.controller, OledControllerCode::kAuto);
    CHECK_EQ(s.width, 0);   // 未知控制器 → 不伪造分辨率
    CHECK_EQ(s.height, 0);
}

void testResetClearsCrossSessionLatch() {
    PhysicalStatus st;
    CHECK(parseLine("oled a=0x3C c=SSD1306 err=0 ok=1", st));
    const PhysicalCapabilitySnapshot learned =
        makePhysicalCapabilitySnapshot(st, PhysicalCapabilitySnapshot{});
    CHECK(learned.capabilityKnown);
    // 重连到无 OLED 设备：无 oled 行 → reset 撤销学习结果。
    const PhysicalCapabilitySnapshot r = resetPhysicalCapability();
    CHECK(!r.capabilityKnown);
    CHECK(!r.healthy);
    CHECK(!r.telemetryFresh);
    CHECK_EQ(static_cast<unsigned>(r.provenance),
             static_cast<unsigned>(PhysicalCapabilityProvenance::kNotSeen));
}

void testSceneDerivation() {
    // mod 行 scene=1 → Application
    PhysicalStatus st1;
    CHECK(parseLine("mod sw=3 st=2 scene=1", st1));
    const PhysicalCapabilitySnapshot s1 =
        makePhysicalCapabilitySnapshot(st1, PhysicalCapabilitySnapshot{});
    CHECK_EQ(s1.scene, PhysicalScene::kApplication);
    // 越界 scene=0xFF → Diagnostics 兜底（不展示非法状态）
    PhysicalStatus st2;
    CHECK(parseLine("mod sw=3 st=2 scene=255", st2));
    const PhysicalCapabilitySnapshot s2 =
        makePhysicalCapabilitySnapshot(st2, PhysicalCapabilitySnapshot{});
    CHECK_EQ(s2.scene, PhysicalScene::kDiagnostics);
}

void testLearnIsMonotonic() {
    // 学习结果只置不清：prev 已知 + 后续无 oled 行 → 保持不变。
    PhysicalStatus st;
    CHECK(parseLine("oled a=0x3C c=SSD1306 err=0 ok=1", st));
    const PhysicalCapabilitySnapshot learned =
        makePhysicalCapabilitySnapshot(st, PhysicalCapabilitySnapshot{});
    PhysicalStatus sessOnly;
    CHECK(parseLine("sess st=3 h=1/1 p=9/10", sessOnly));
    const PhysicalCapabilitySnapshot s = makePhysicalCapabilitySnapshot(sessOnly, learned);
    CHECK(s.capabilityKnown);
    CHECK_EQ(s.controller, OledControllerCode::kSsd1306);
    CHECK_EQ(s.width, 128);
    CHECK_EQ(s.height, 64);
}

}  // namespace

void runPhysicalCapabilitySnapshotTests() {
    std::printf("[physical_capability_snapshot]\n");
    testInitial();
    testLearnFromOledOk();
    testHealthDegradedKeepsCapability();
    testSh1106Geometry();
    testUnknownControllerNoFakeGeometry();
    testResetClearsCrossSessionLatch();
    testSceneDerivation();
    testLearnIsMonotonic();
}