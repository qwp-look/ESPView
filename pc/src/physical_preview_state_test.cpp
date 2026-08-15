// ESPView M7-D2 — PhysicalPreviewState host tests（纯 C++17，零 Qt / 零协议
// wire 依赖）。
//
// 规范来源：docs/DESIGN.md AE 节（M7-D2 PHYSICAL_PREVIEW 冻结）：
//   - setFrame 更新快照 + 时间戳；
//   - 帧去重/过期：`(int16_t)(frameId - lastFrameId) > 0` 才接受
//     （回绕安全：65535 → 0 接受；0 → 65535 拒绝）；
//   - 重连后首帧无条件接受（握手清 lastFrameId）；
//   - onDisconnected 清空像素/时间戳 → unavailable；
//   - stale 判定：lastUpdate 距今 > 阈值（默认 1000ms，>1s 才 stale），
//     模拟时间注入，不依赖真实时钟；
//   - previewEnabled QSettings 往返：toSettingsMap 恰 {ui/previewEnabled}，
//     fromSettingsMap 只读该键、未知键忽略、缺键保持当前值；
//   - 防御：非法几何/像素不足拒绝。
//
// 使用 shared/protocol/tests/test_util.h 的 CHECK / CHECK_EQ 框架，经
// test_main.cpp 登记进协议套件二进制（与 display/oled/pc 纯模型测试一致）。

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "physical_preview_state.h"
#include "test_util.h"

namespace {

using espview::pc::kDefaultPreviewStaleMs;
using espview::pc::kPixelFormatMono1;
using espview::pc::kPreviewEnabledSettingsKey;
using espview::pc::PhysicalPreviewState;

constexpr uint16_t kWidth = 128;
constexpr uint16_t kHeight = 64;
constexpr size_t kPixelsBytes = 1024;  // 128x64 1bpp 页式

// 构造 128x64 页式 1bpp 像素（页式布局：fb[page*width + x]，bit0=页顶行）。
std::vector<uint8_t> makePixels(uint8_t fill = 0) {
    return std::vector<uint8_t>(kPixelsBytes, fill);
}

void testSetFrameUpdates() {
    std::printf("[physical_preview_state] setFrame updates\n");
    PhysicalPreviewState s;
    CHECK(!s.isAvailable());  // 初始：无像素 / 未连接

    // 未连接也可写入快照，但可用性仍被 sessionConnected 门控。
    const auto pixels = makePixels(0xAA);
    CHECK(s.setFrame(1, kWidth, kHeight, kPixelFormatMono1, pixels, 1234));
    CHECK_EQ(s.frameId(), 1);
    CHECK_EQ(s.width(), kWidth);
    CHECK_EQ(s.height(), kHeight);
    CHECK_EQ(s.pixelFormat(), kPixelFormatMono1);
    CHECK_EQ(s.lastUpdateMs(), 1234ULL);
    CHECK(s.pixels() == pixels);
    CHECK(!s.isAvailable());  // 未连接

    // 连接后可用。
    s.setSessionConnected(true);
    CHECK(s.isAvailable());
    CHECK(s.pixels() == pixels);

    // 新帧覆盖快照 + 时间戳。
    const auto pixels2 = makePixels(0x55);
    CHECK(s.setFrame(2, kWidth, kHeight, kPixelFormatMono1, pixels2, 2345));
    CHECK_EQ(s.frameId(), 2);
    CHECK_EQ(s.lastUpdateMs(), 2345ULL);
    CHECK(s.pixels() == pixels2);
    CHECK(s.isAvailable());
}

void testFrameDedupAndWrap() {
    std::printf("[physical_preview_state] frame dedup / out-of-order / wrap\n");
    PhysicalPreviewState s;
    s.setSessionConnected(true);
    const auto pixels = makePixels();

    // 首帧无条件接受（去重基未建立）。
    CHECK(s.setFrame(100, kWidth, kHeight, kPixelFormatMono1, pixels, 10));
    // 相等 id 丢弃。
    CHECK(!s.setFrame(100, kWidth, kHeight, kPixelFormatMono1, pixels, 20));
    CHECK_EQ(s.lastUpdateMs(), 10ULL);  // 状态不变
    // 过期/乱序 id 丢弃（100 → 50）。
    CHECK(!s.setFrame(50, kWidth, kHeight, kPixelFormatMono1, pixels, 30));
    CHECK_EQ(s.lastUpdateMs(), 10ULL);
    // 更小 id（乱序）也丢弃（100 → 99）。
    CHECK(!s.setFrame(99, kWidth, kHeight, kPixelFormatMono1, pixels, 40));
    // 新 id 接受。
    CHECK(s.setFrame(101, kWidth, kHeight, kPixelFormatMono1, pixels, 50));

    // 回绕安全：会话首帧 65535 无条件接受 → 下一帧 0 接受
    //（(int16_t)(0-65535)=1 > 0）。
    {
        PhysicalPreviewState w;
        w.setSessionConnected(true);
        CHECK(w.setFrame(65535, kWidth, kHeight, kPixelFormatMono1, pixels, 60));
        CHECK(w.setFrame(0, kWidth, kHeight, kPixelFormatMono1, pixels, 70));
        CHECK_EQ(w.frameId(), 0);
        // 回绕后 0 → 65535 拒绝（(int16_t)(65535-0)=-1 < 0）。
        CHECK(!w.setFrame(65535, kWidth, kHeight, kPixelFormatMono1, pixels, 80));
        // 0 → 1 接受。
        CHECK(w.setFrame(1, kWidth, kHeight, kPixelFormatMono1, pixels, 90));
        CHECK_EQ(w.frameId(), 1);
    }
}

void testReconnectFirstFrameAccepted() {
    std::printf("[physical_preview_state] reconnect first frame accepted\n");
    PhysicalPreviewState s;
    s.setSessionConnected(true);
    const auto pixels = makePixels();
    CHECK(s.setFrame(7, kWidth, kHeight, kPixelFormatMono1, pixels, 10));

    // 断线清空 + 重置去重基。
    s.onDisconnected();
    CHECK(!s.isAvailable());
    CHECK(s.pixels().empty());
    CHECK_EQ(s.lastUpdateMs(), 0ULL);

    // 重连：即使旧 id（≤ 断线前）也无条件接受（AE.3：清 lastFrameId）。
    s.setSessionConnected(true);
    CHECK(s.setFrame(3, kWidth, kHeight, kPixelFormatMono1, pixels, 20));
    CHECK_EQ(s.frameId(), 3);
    CHECK(s.isAvailable());
}

void testStaleThreshold() {
    std::printf("[physical_preview_state] stale (>1s, simulated time)\n");
    PhysicalPreviewState s;
    s.setSessionConnected(true);
    const auto pixels = makePixels();
    CHECK_EQ(kDefaultPreviewStaleMs, 1000ULL);  // 任务书：stale >1s

    s.setFrame(1, kWidth, kHeight, kPixelFormatMono1, pixels, 1000);
    // 距 1000ms：>1s 才 stale → 恰好 1000ms 不 stale。
    CHECK(!s.isStale(2000));
    // 1001ms → stale。
    CHECK(s.isStale(2001));
    // 时钟回退防御：不 stale。
    CHECK(!s.isStale(999));

    // 未连接 / 无数据：永不 stale。
    s.onDisconnected();
    CHECK(!s.isStale(5000));

    // 自定义阈值：0 = 任意间隔都 stale（仅测试注入）。
    s.setSessionConnected(true);
    s.setFrame(2, kWidth, kHeight, kPixelFormatMono1, pixels, 3000);
    s.setStaleThresholdMs(0);
    CHECK(s.isStale(3001));
    // 恢复默认阈值。
    s.setStaleThresholdMs(kDefaultPreviewStaleMs);
    CHECK(!s.isStale(3999));
    CHECK(s.isStale(4001));
}

void testRejectInvalidInput() {
    std::printf("[physical_preview_state] invalid geometry / short pixels\n");
    PhysicalPreviewState s;
    s.setSessionConnected(true);
    const auto pixels = makePixels();
    // 几何非法：width/height = 0。
    CHECK(!s.setFrame(1, 0, kHeight, kPixelFormatMono1, pixels, 10));
    CHECK(!s.setFrame(1, kWidth, 0, kPixelFormatMono1, pixels, 10));
    // 像素不足（1023B < 1024B）。
    const std::vector<uint8_t> shortPixels(1023, 0);
    CHECK(!s.setFrame(1, kWidth, kHeight, kPixelFormatMono1, shortPixels, 10));
    // 非法输入不污染快照。
    CHECK(!s.isAvailable());
    CHECK(s.pixels().empty());
    // 合法帧仍可接受。
    CHECK(s.setFrame(1, kWidth, kHeight, kPixelFormatMono1, pixels, 10));
}

void testPreviewEnabledSettingsRoundTrip() {
    std::printf("[physical_preview_state] previewEnabled QSettings map (single key)\n");
    PhysicalPreviewState s;
    CHECK(s.previewEnabled());  // 默认启用（AE.5：PC 侧 UI 控制）

    // toSettingsMap：恰一个键，值 "1"/"0"。
    std::map<std::string, std::string> m = s.toSettingsMap();
    CHECK_EQ(static_cast<long long>(m.size()), 1LL);
    CHECK(m.count(kPreviewEnabledSettingsKey) == 1);
    CHECK(m.at(kPreviewEnabledSettingsKey) == "1");

    // 往返：写 false → 导出 "0" → 读回 false。
    s.setPreviewEnabled(false);
    m = s.toSettingsMap();
    CHECK_EQ(static_cast<long long>(m.size()), 1LL);
    CHECK(m.at(kPreviewEnabledSettingsKey) == "0");
    PhysicalPreviewState restored;
    restored.fromSettingsMap(m);
    CHECK(!restored.previewEnabled());

    // "true"/"false"（QSettings ini 风格）也接受，大小写不敏感。
    {
        std::map<std::string, std::string> map;
        map[kPreviewEnabledSettingsKey] = "TRUE";
        PhysicalPreviewState r;
        r.fromSettingsMap(map);
        CHECK(r.previewEnabled());
    }

    // 缺键 → 保持当前值。
    {
        PhysicalPreviewState r;
        r.setPreviewEnabled(false);
        std::map<std::string, std::string> empty;
        r.fromSettingsMap(empty);
        CHECK(!r.previewEnabled());
    }

    // 未知键一律忽略（只认 ui/previewEnabled）。
    {
        std::map<std::string, std::string> map;
        map["ui/other"] = "1";
        map["somethingElse"] = "0";
        PhysicalPreviewState r;
        r.fromSettingsMap(map);
        CHECK(r.previewEnabled());  // 未被未知键污染
    }

    // 未知值 → 忽略并保持当前值。
    {
        std::map<std::string, std::string> map;
        map[kPreviewEnabledSettingsKey] = "maybe";
        PhysicalPreviewState r;
        r.fromSettingsMap(map);
        CHECK(r.previewEnabled());
    }
}

}  // namespace

// 由 shared/protocol/tests/test_main.cpp 登记调用（协议套件二进制）。
void runPhysicalPreviewStateTests() {
    std::printf("[physical_preview_state]\n");
    testSetFrameUpdates();
    testFrameDedupAndWrap();
    testReconnectFirstFrameAccepted();
    testStaleThreshold();
    testRejectInvalidInput();
    testPreviewEnabledSettingsRoundTrip();
    std::printf("[physical_preview_state] done\n");
}
