// ESPView M7-C3 — SplitState Host Tests（任务书 §二十二 15–18）。
//
// 15. drawer open/close：默认收起；open/close/toggle/setDrawerVisible；
// 16. resize：setDrawerWidth/resize 夹取 [200, 560]；
// 17. settings persistence：toSettingsMap/fromSettingsMap 键值精确往返；
// 18. 组合语义：宽度跨开合保留；部分 map / 空 map / 未知键 / 非法值行为。
//
// 纯 host、零平台依赖（同 display_router_test 风格：CHECK/CHECK_EQ +
// 全局计数器 test_util.h）。注册由主代理在 test_main/CMakeLists 完成。

#include <string>
#include <utility>
#include <vector>

#include "split_state.h"
#include "test_util.h"

namespace {

using espview::display::SplitState;
using SettingsMap = SplitState::SettingsMap;

// §22.15 drawer open/close。
void testOpenClose() {
    SplitState s;
    // 默认：收起 + 默认宽度（保守默认，进入 Split 由主窗口显式 open()）。
    CHECK(!s.drawerVisible());
    CHECK_EQ(s.drawerWidth(), SplitState::kDefaultDrawerWidth);

    s.open();
    CHECK(s.drawerVisible());
    s.close();
    CHECK(!s.drawerVisible());

    s.setDrawerVisible(true);
    CHECK(s.drawerVisible());
    s.toggle();
    CHECK(!s.drawerVisible());
    s.toggle();
    CHECK(s.drawerVisible());

    // 显式赋值幂等。
    s.setDrawerVisible(true);
    CHECK(s.drawerVisible());
    s.setDrawerVisible(false);
    CHECK(!s.drawerVisible());
}

// §22.16 resize：范围夹取。
void testResize() {
    SplitState s;
    CHECK_EQ(s.drawerWidth(), SplitState::kDefaultDrawerWidth);

    s.setDrawerWidth(400);
    CHECK_EQ(s.drawerWidth(), 400);

    // 低于下限 -> 夹到下限。
    s.setDrawerWidth(SplitState::kMinDrawerWidth - 1);
    CHECK_EQ(s.drawerWidth(), SplitState::kMinDrawerWidth);
    s.setDrawerWidth(1);
    CHECK_EQ(s.drawerWidth(), SplitState::kMinDrawerWidth);
    s.setDrawerWidth(-100);
    CHECK_EQ(s.drawerWidth(), SplitState::kMinDrawerWidth);

    // 高于上限 -> 夹到上限。
    s.setDrawerWidth(SplitState::kMaxDrawerWidth + 1);
    CHECK_EQ(s.drawerWidth(), SplitState::kMaxDrawerWidth);
    s.setDrawerWidth(2000);
    CHECK_EQ(s.drawerWidth(), SplitState::kMaxDrawerWidth);

    // 边界合法值原样保留。
    s.setDrawerWidth(SplitState::kMinDrawerWidth);
    CHECK_EQ(s.drawerWidth(), SplitState::kMinDrawerWidth);
    s.setDrawerWidth(SplitState::kMaxDrawerWidth);
    CHECK_EQ(s.drawerWidth(), SplitState::kMaxDrawerWidth);

    // resize() 是 setDrawerWidth 的别名（同样夹取）。
    s.resize(500);
    CHECK_EQ(s.drawerWidth(), 500);
    s.resize(9999);
    CHECK_EQ(s.drawerWidth(), SplitState::kMaxDrawerWidth);
    s.resize(0);
    CHECK_EQ(s.drawerWidth(), SplitState::kMinDrawerWidth);

    // clampWidth 静态助手与成员夹取一致。
    CHECK_EQ(SplitState::clampWidth(0), SplitState::kMinDrawerWidth);
    CHECK_EQ(SplitState::clampWidth(320), 320);
    CHECK_EQ(SplitState::clampWidth(1000), SplitState::kMaxDrawerWidth);
}

// §22.17 settings persistence：toSettingsMap / fromSettingsMap。
void testSettingsPersistence() {
    SplitState s;
    s.open();
    s.setDrawerWidth(450);

    const SettingsMap map = s.toSettingsMap();
    // 恰好两个键，键名固定（与 Qt 层 QSettings 键一致）。
    CHECK_EQ(map.size(), static_cast<std::size_t>(2));
    CHECK(map[0].first == SplitState::kKeyDrawerVisible);
    CHECK(map[0].second == "1");
    CHECK(map[1].first == SplitState::kKeyDrawerWidth);
    CHECK(map[1].second == "450");

    // 往返：恢复出的状态与序列化前一致。
    SplitState restored;
    CHECK(!restored.drawerVisible());
    CHECK(restored.fromSettingsMap(map));
    CHECK(restored.drawerVisible());
    CHECK_EQ(restored.drawerWidth(), 450);

    // 收起状态的序列化值。
    SplitState closed;
    closed.setDrawerWidth(SplitState::kMinDrawerWidth);
    const SettingsMap closedMap = closed.toSettingsMap();
    CHECK(closedMap[0].second == "0");
    CHECK(closedMap[1].second == std::to_string(SplitState::kMinDrawerWidth));

    // 反序列化对宽度值做夹取（可解析但越界 -> 夹取，视为应用）。
    SettingsMap wide = {
        {SplitState::kKeyDrawerWidth, "10000"},
    };
    CHECK(restored.fromSettingsMap(wide));
    CHECK_EQ(restored.drawerWidth(), SplitState::kMaxDrawerWidth);
    SettingsMap narrow = {
        {SplitState::kKeyDrawerWidth, "-5"},
    };
    CHECK(restored.fromSettingsMap(narrow));
    CHECK_EQ(restored.drawerWidth(), SplitState::kMinDrawerWidth);
}

// §22.18 组合语义：部分 map / 空 map / 未知键 / 非法值。
void testPersistenceCombinations() {
    SplitState s;
    s.open();
    s.setDrawerWidth(480);

    // 部分 map（只给宽度）：可见性保持，宽度更新。
    SettingsMap widthOnly = {
        {SplitState::kKeyDrawerWidth, "360"},
    };
    CHECK(s.fromSettingsMap(widthOnly));
    CHECK(s.drawerVisible());
    CHECK_EQ(s.drawerWidth(), 360);

    // 部分 map（只给可见性）：宽度保持。
    SettingsMap visibleOnly = {
        {SplitState::kKeyDrawerVisible, "false"},
    };
    CHECK(s.fromSettingsMap(visibleOnly));
    CHECK(!s.drawerVisible());
    CHECK_EQ(s.drawerWidth(), 360);

    // 宽松布尔值。
    SettingsMap boolVariants = {
        {SplitState::kKeyDrawerVisible, "TRUE"},
        {SplitState::kKeyDrawerVisible, "yes"},
        {SplitState::kKeyDrawerVisible, "on"},
        {SplitState::kKeyDrawerVisible, "1"},
    };
    for (const auto& kv : boolVariants) {
        SplitState t;
        SettingsMap one = {kv};
        CHECK(t.fromSettingsMap(one));
        CHECK(t.drawerVisible());
    }

    // 空 map / 全未知键：不应用任何状态，返回 false。
    const SettingsMap empty;
    CHECK(!s.fromSettingsMap(empty));
    CHECK(!s.drawerVisible());
    CHECK_EQ(s.drawerWidth(), 360);

    SettingsMap unknown = {
        {"split/other", "x"},
        {"window/size", "800x600"},
    };
    CHECK(!s.fromSettingsMap(unknown));
    CHECK(!s.drawerVisible());
    CHECK_EQ(s.drawerWidth(), 360);

    // 非法值（不可解析）：忽略，保持当前状态。
    SettingsMap bad = {
        {SplitState::kKeyDrawerVisible, "maybe"},
        {SplitState::kKeyDrawerWidth, "12ab"},
        {SplitState::kKeyDrawerWidth, ""},
        {SplitState::kKeyDrawerWidth, "99999999999999999999"},
    };
    CHECK(!s.fromSettingsMap(bad));
    CHECK(!s.drawerVisible());
    CHECK_EQ(s.drawerWidth(), 360);

    // 合法与非法混合：合法键生效（返回 true）。
    SettingsMap mixed = {
        {SplitState::kKeyDrawerVisible, "maybe"},  // 非法，忽略
        {SplitState::kKeyDrawerWidth, "440"},      // 合法，应用
    };
    CHECK(s.fromSettingsMap(mixed));
    CHECK(!s.drawerVisible());
    CHECK_EQ(s.drawerWidth(), 440);

    // 宽度跨 open/close 保留（§22.15/18 组合）。
    s.close();
    s.open();
    CHECK_EQ(s.drawerWidth(), 440);
    s.resize(510);
    s.close();
    CHECK_EQ(s.drawerWidth(), 510);
    s.open();
    CHECK_EQ(s.drawerWidth(), 510);
}

}  // namespace

void runSplitStateTests() {
    std::printf("[split_state] tests\n");
    testOpenClose();
    testResize();
    testSettingsPersistence();
    testPersistenceCombinations();
    std::printf("[split_state] done\n");
}
