// ESPView — DisplayRouter Host Tests（M7-C1）
//
// 规范来源：M7-C 任务书 §34（10 项：VirtualOnly / PhysicalOnly / Mirror /
// Split / sink unavailable / sink degraded / transport independent /
// mode switch / FULL resync / stale frame clearing）+ §33 降级语义。
// 纯 host，零平台依赖；沿用 remote_display_test / test_util.h 风格
// （CHECK / CHECK_EQ + 全局计数器）。
//
// 测试链路：
//   DisplayRouter → IDisplaySink（RecordingSink，记录 enable/present/flush/
//   可用性）→ 断言路由扇出与状态机。不触碰协议 wire（Router 与传输无关，
//   即「transport independent」由两个互不共享状态的 sink 实例验证）。

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "display.h"
#include "display_capabilities.h"
#include "display_router.h"
#include "display_sink.h"
#include "test_util.h"

namespace {

using espview::display::DisplayCapabilities;
using espview::display::DisplayRouteMode;
using espview::display::DisplayRouter;
using espview::display::DisplaySinkKind;
using espview::display::DisplayStatus;
using espview::display::IDisplaySink;
using espview::display::PhysicalScene;
using espview::display::Rect;
using espview::display::RouterState;
using espview::proto::PixelFormat;

// ---- 记录型 sink（复用 remote_display_test 的 RecordingSink 风格）----
class RecordingSink : public IDisplaySink {
public:
    explicit RecordingSink(DisplaySinkKind kind, int w = 128, int h = 64,
                           bool available = true)
        : available_(available) {
        caps_.width = w;
        caps_.height = h;
        caps_.format = PixelFormat::kRgb565;
        caps_.color = (kind == DisplaySinkKind::kPhysical) ? 1 : 16;
        caps_.mono = (kind == DisplaySinkKind::kPhysical);
        caps_.canReadback = (kind == DisplaySinkKind::kVirtual);
        caps_.sinkKind = kind;
    }

    // 记录 enable/disable 事件到共享日志（切换顺序断言用）。
    void attachLog(std::vector<std::string>* log, const char* name) {
        log_ = log;
        name_ = name;
    }

    DisplayStatus init(const DisplayCapabilities& caps) override {
        ++initCalls;
        initCaps_ = caps;
        return DisplayStatus::kOk;
    }
    const DisplayCapabilities& capabilities() const override { return caps_; }
    DisplayStatus present(const Rect& rect, const uint8_t* pixels) override {
        presents_.emplace_back(rect, pixels ? pixels[0] : 0);
        lastStatus_ = presentResult_;
        return presentResult_;
    }
    DisplayStatus flush() override {
        ++flushCalls;
        lastStatus_ = DisplayStatus::kOk;
        return DisplayStatus::kOk;
    }
    DisplayStatus setEnabled(bool enabled) override {
        enabled_ = enabled;
        if (log_) {
            log_->push_back(std::string(name_) + (enabled ? ":enable" : ":disable"));
        }
        return DisplayStatus::kOk;
    }
    bool isAvailable() const override { return available_; }
    DisplayStatus status() const override { return lastStatus_; }

    void setAvailable(bool a) { available_ = a; }
    void setPresentResult(DisplayStatus s) { presentResult_ = s; }

    int initCalls = 0;
    DisplayCapabilities initCaps_;
    std::vector<std::pair<Rect, uint8_t>> presents_;
    int flushCalls = 0;
    bool enabled_ = false;

private:
    DisplayCapabilities caps_;
    bool available_ = true;
    std::vector<std::string>* log_ = nullptr;
    std::string name_;
    DisplayStatus lastStatus_ = DisplayStatus::kOk;
    DisplayStatus presentResult_ = DisplayStatus::kOk;
};

// ---- 工具 ----
bool sameRect(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

std::shared_ptr<RecordingSink> makeVirtual(bool available = true) {
    return std::make_shared<RecordingSink>(DisplaySinkKind::kVirtual, 320, 240, available);
}
std::shared_ptr<RecordingSink> makePhysical(bool available = true) {
    return std::make_shared<RecordingSink>(DisplaySinkKind::kPhysical, 128, 64, available);
}

// 两个 sink + 共享切换日志 + 状态捕获（mode switch 顺序断言用）。
struct Harness {
    DisplayRouter router;
    std::shared_ptr<RecordingSink> virt = makeVirtual();
    std::shared_ptr<RecordingSink> phys = makePhysical();
    std::vector<std::string> log;
    std::vector<int> staleEnabledCount;   // stale-clear 触发时已 enable 的 sink 数（恒 0 = 切换窗口）
    std::vector<int> resyncEnabledCount;  // full-resync 触发时已 enable 的 sink 数（= 目标模式 sink 数）

    Harness() {
        virt->attachLog(&log, "v");
        phys->attachLog(&log, "p");
        router.attachVirtual(virt);
        router.attachPhysical(phys);
        // 钩子内不调用 Router 锁方法（避免死锁）；用 sink 的 enable 状态
        // 行为性验证切换窗口：stale-clear 时全部 disable，full-resync 时已 enable。
        router.setStaleClearCallback([this] {
            log.push_back("stale-clear");
            staleEnabledCount.push_back((virt->enabled_ ? 1 : 0) + (phys->enabled_ ? 1 : 0));
        });
        router.setFullResyncCallback([this] {
            log.push_back("full-resync");
            resyncEnabledCount.push_back((virt->enabled_ ? 1 : 0) + (phys->enabled_ ? 1 : 0));
        });
    }
};

// ---- 用例 ----

// 1. VirtualOnly：应用帧只走 virtual；physical 不启用。
void testVirtualOnly() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kIdle));
    // 未选择模式：写路径 kNotEnabled（kIdle 门；kSwitching 走同一分支）
    const uint8_t px0[2] = {0x11, 0x22};
    CHECK_EQ(static_cast<int>(h.router.writeRect(Rect{0, 0, 2, 1}, px0)),
             static_cast<int>(DisplayStatus::kNotEnabled));
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kNotEnabled));

    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.mode()),
             static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK(h.virt->enabled_);
    CHECK(!h.phys->enabled_);

    const Rect r{10, 20, 8, 4};
    const uint8_t px[8] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01, 0x02};
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(0));
    if (h.virt->presents_.size() == 1) {
        CHECK(sameRect(h.virt->presents_[0].first, r));
        CHECK_EQ(h.virt->presents_[0].second, 0xAA);
    }
    CHECK_EQ(h.virt->flushCalls, 1);
    CHECK_EQ(h.phys->flushCalls, 0);
}

// 2. PhysicalOnly：应用帧只走 physical；virtual 不启用。
void testPhysicalOnly() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.mode()),
             static_cast<int>(DisplayRouteMode::kPhysicalOnly));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK(!h.virt->enabled_);
    CHECK(h.phys->enabled_);

    const Rect r{0, 0, 128, 64};
    const uint8_t px[4] = {0x81, 0x82, 0x83, 0x84};
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(0));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    CHECK_EQ(h.virt->flushCalls, 0);
    CHECK_EQ(h.phys->flushCalls, 1);
}

// 3. Mirror：同一份 writeRect 扇出到两个 sink。
void testMirrorFanout() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK(h.virt->enabled_);
    CHECK(h.phys->enabled_);

    const Rect r{4, 4, 16, 8};
    const uint8_t px[8] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    if (h.virt->presents_.size() == 1 && h.phys->presents_.size() == 1) {
        CHECK(sameRect(h.virt->presents_[0].first, r));
        CHECK(sameRect(h.phys->presents_[0].first, r));
        CHECK_EQ(h.virt->presents_[0].second, 0x10);
        CHECK_EQ(h.phys->presents_[0].second, 0x10);
    }
    CHECK_EQ(h.virt->flushCalls, 1);
    CHECK_EQ(h.phys->flushCalls, 1);
}

// 4. Split：virtual 收应用帧（writeRect），physical 只经 presentScene 收独立
//    场景帧；两个 sink 内容可不同。
void testSplitIndependent() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kSplit)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK(h.virt->enabled_);
    CHECK(h.phys->enabled_);

    const Rect app{0, 0, 320, 240};
    const Rect diag{0, 0, 128, 64};
    const uint8_t appPx[4] = {0x31, 0x32, 0x33, 0x34};
    const uint8_t diagPx[4] = {0x91, 0x92, 0x93, 0x94};

    // 应用帧只走 virtual
    CHECK_EQ(static_cast<int>(h.router.writeRect(app, appPx)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(0));

    // 物理侧独立场景帧经 presentScene（kDiagnostics / kApplication）
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kDiagnostics, diag, diagPx)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kApplication, diag, diagPx)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));   // 场景帧不进 virtual
    CHECK_EQ(h.phys->presents_.size(), size_t(2));
    if (h.phys->presents_.size() == 2) {
        CHECK(sameRect(h.phys->presents_[0].first, diag));
        CHECK_EQ(h.phys->presents_[0].second, 0x91);
    }

    // flush：Split 只刷新 virtual（物理场景帧的 flush 由 C2 场景提交方负责）
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->flushCalls, 1);
    CHECK_EQ(h.phys->flushCalls, 0);

    // 场景概念仅属 Split：Mirror 下 presentScene → kNotSupported
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kDiagnostics, diag, diagPx)),
             static_cast<int>(DisplayStatus::kNotSupported));
    // 非法 scene 值 → kInvalidParam（模式无关）
    CHECK_EQ(static_cast<int>(h.router.presentScene(static_cast<PhysicalScene>(9), diag, diagPx)),
             static_cast<int>(DisplayStatus::kInvalidParam));
}

// 5. sink unavailable：attach 但 isAvailable()==false → DEGRADED，写路径 kNotConnected。
void testSinkUnavailable() {
    DisplayRouter router;
    auto virt = makeVirtual(false);   // virtual transport 未连接
    auto phys = makePhysical(true);
    router.attachVirtual(virt);
    router.attachPhysical(phys);
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kDegraded));
    const uint8_t px[2] = {0x41, 0x42};
    CHECK_EQ(static_cast<int>(router.writeRect(Rect{0, 0, 2, 1}, px)),
             static_cast<int>(DisplayStatus::kNotConnected));
    CHECK_EQ(virt->presents_.size(), size_t(0));
    CHECK_EQ(static_cast<int>(router.flush()), static_cast<int>(DisplayStatus::kNotConnected));

    // 恢复可用 → CONNECTED，写路径恢复
    virt->setAvailable(true);
    router.refreshState();
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kConnected));
    CHECK_EQ(static_cast<int>(router.writeRect(Rect{0, 0, 2, 1}, px)),
             static_cast<int>(DisplayStatus::kOk));

    // physical 侧同理
    DisplayRouter router2;
    auto virt2 = makeVirtual(true);
    auto phys2 = makePhysical(false);
    router2.attachVirtual(virt2);
    router2.attachPhysical(phys2);
    CHECK_EQ(static_cast<int>(router2.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(router2.state()), static_cast<int>(RouterState::kDegraded));
    CHECK_EQ(static_cast<int>(router2.writeRect(Rect{0, 0, 2, 1}, px)),
             static_cast<int>(DisplayStatus::kNotConnected));
}

// 6. sink degraded（任务 §33）：Mirror 中 physical 运行期不可用 →
//    DEGRADED，但 virtual 继续收帧；恢复后回到 CONNECTED。
void testSinkDegradedMirror() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));

    const Rect r{1, 1, 16, 8};
    const uint8_t px[8] = {0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58};
    h.phys->setAvailable(false);   // I2C/物理链路掉线
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kDegraded));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));   // virtual 继续
    CHECK_EQ(h.phys->presents_.size(), size_t(0));   // physical 被跳过
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->flushCalls, 1);

    // 恢复 → 自动回到 CONNECTED（下一次写路径重评估）
    h.phys->setAvailable(true);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
}

// 7. transport independent：两个 sink 的可用性/状态互不影响（各自由自己的
//    transport 决定），Router 逐个独立评估并扇出。
void testTransportIndependent() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    const Rect r{2, 2, 8, 4};
    const uint8_t px[4] = {0x61, 0x62, 0x63, 0x64};

    // 两者都可用 → 都收到
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));

    // 只有 virtual 的 transport 掉线 → physical 独立继续
    h.virt->setAvailable(false);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kDegraded));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));   // virtual 被跳过
    CHECK_EQ(h.phys->presents_.size(), size_t(2));   // physical 继续

    // 反过来：virtual 恢复、physical 掉线 → virtual 继续
    h.virt->setAvailable(true);
    h.phys->setAvailable(false);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(2));
    CHECK_EQ(h.phys->presents_.size(), size_t(2));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kDegraded));

    // 都恢复 → CONNECTED，双向扇出恢复
    h.phys->setAvailable(true);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK_EQ(h.virt->presents_.size(), size_t(3));
    CHECK_EQ(h.phys->presents_.size(), size_t(3));
}

// 8. mode switch：4 模式全切换 + 切换序列顺序（disable 所有 → stale clear →
//    enable 按模式 → FULL resync → CONNECTED/DEGRADED；stale/resync 钩子在
//    SWITCHING 窗口内触发）。
void testModeSwitch() {
    Harness h;
    // kMirror：两个 sink 都 disable → stale-clear → 两个都 enable → full-resync
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    {
        const std::vector<std::string> expected = {
            "v:disable", "p:disable", "stale-clear", "v:enable", "p:enable", "full-resync"};
        CHECK(h.log == expected);
    }
    CHECK_EQ(h.staleEnabledCount.size(), size_t(1));
    CHECK_EQ(h.resyncEnabledCount.size(), size_t(1));
    if (!h.staleEnabledCount.empty()) {
        CHECK_EQ(h.staleEnabledCount[0], 0);   // stale-clear 时 sink 已全部 disable
    }
    if (!h.resyncEnabledCount.empty()) {
        CHECK_EQ(h.resyncEnabledCount[0], 2);  // Mirror：full-resync 时两个 sink 已 enable
    }
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));

    // → kVirtualOnly：只 enable virtual
    h.log.clear();
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kOk));
    {
        const std::vector<std::string> expected = {
            "v:disable", "p:disable", "stale-clear", "v:enable", "full-resync"};
        CHECK(h.log == expected);
    }
    CHECK_EQ(static_cast<int>(h.router.mode()), static_cast<int>(DisplayRouteMode::kVirtualOnly));
    CHECK(h.virt->enabled_);
    CHECK(!h.phys->enabled_);

    // → kPhysicalOnly：只 enable physical
    h.log.clear();
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kOk));
    {
        const std::vector<std::string> expected = {
            "v:disable", "p:disable", "stale-clear", "p:enable", "full-resync"};
        CHECK(h.log == expected);
    }
    CHECK(!h.virt->enabled_);
    CHECK(h.phys->enabled_);

    // → kSplit：两个都 enable
    h.log.clear();
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kSplit)),
             static_cast<int>(DisplayStatus::kOk));
    {
        const std::vector<std::string> expected = {
            "v:disable", "p:disable", "stale-clear", "v:enable", "p:enable", "full-resync"};
        CHECK(h.log == expected);
    }
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK_EQ(h.staleEnabledCount.size(), size_t(4));   // 每次切换各触发一次
    CHECK_EQ(h.resyncEnabledCount.size(), size_t(4));
    CHECK_EQ(h.staleEnabledCount[3], 0);
    CHECK_EQ(h.resyncEnabledCount[3], 2);              // Split：两个都 enable
}

// 9. FULL resync：每次成功 setMode 后请求一次 FULL resync（生产者置 needFull）。
void testFullResyncRequested() {
    Harness h;
    CHECK_EQ(h.resyncEnabledCount.size(), size_t(0));
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    // 同模式重复切换同样触发（幂等但显式重建基准）
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.resyncEnabledCount.size(), size_t(3));
    CHECK_EQ(h.resyncEnabledCount[0], 1);   // VirtualOnly
    CHECK_EQ(h.resyncEnabledCount[1], 2);   // Mirror
    CHECK_EQ(h.resyncEnabledCount[2], 2);   // Mirror 重复
    // 失败路径不触发：非法模式
    CHECK_EQ(static_cast<int>(h.router.setMode(static_cast<DisplayRouteMode>(9))),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(h.resyncEnabledCount.size(), size_t(3));
}

// 10. stale frame clearing：切换窗口内丢弃旧帧基准（stale-clear 在 disable
//     之后、enable 之前触发；每次切换恰好一次）。
void testStaleFrameClearing() {
    Harness h;
    // 首切：stale-clear 与 full-resync 都在 SWITCHING 窗口内
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.log.size(), size_t(6));
    // 再切：重复触发（stale 帧在两次切换间产生的也必须被清掉）
    h.log.clear();
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kSplit)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.log.size(), size_t(6));
    CHECK(h.log[0] == "v:disable");
    CHECK(h.log[1] == "p:disable");
    CHECK(h.log[2] == "stale-clear");
    CHECK(h.log[3] == "v:enable");
    CHECK(h.log[4] == "p:enable");
    CHECK(h.log[5] == "full-resync");
    CHECK_EQ(h.staleEnabledCount.size(), size_t(2));
    CHECK_EQ(h.staleEnabledCount[0], 0);
    CHECK_EQ(h.staleEnabledCount[1], 0);
}

// 11. setMode 校验：非法模式 / 必需 sink 缺失 → kInvalidParam，状态不变。
void testSetModeValidation() {
    DisplayRouter router;
    // 无任何 sink：任何模式都失败（必需 sink 缺失）
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kSplit)),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kIdle));

    // 非法模式值（>3）→ kInvalidParam
    CHECK_EQ(static_cast<int>(router.setMode(static_cast<DisplayRouteMode>(4))),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kIdle));

    // 只 attach virtual：kPhysicalOnly / kMirror / kSplit 缺 physical
    auto virt = makeVirtual();
    router.attachVirtual(virt);
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kInvalidParam));
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kConnected));
    // attach 不改变当前模式/状态；重新 setMode 使新 sink 生效
    auto phys = makePhysical();
    router.attachPhysical(phys);
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kConnected));
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kConnected));
}

// 12. present 错误聚合：任一 sink 成功 → kOk（不阻塞 UI，DESIGN.md F 节）；
//     全部失败 → 传播首个错误。
void testPresentErrorAggregation() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    const Rect r{0, 0, 8, 4};
    const uint8_t px[4] = {0x71, 0x72, 0x73, 0x74};
    // virtual 背压（kQueueFull）+ physical 成功 → kOk（不放大背压）
    h.virt->setPresentResult(DisplayStatus::kQueueFull);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    // 两个都失败 → 传播首个错误（virtual 的 kQueueFull）
    h.phys->setPresentResult(DisplayStatus::kInternal);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)),
             static_cast<int>(DisplayStatus::kQueueFull));
    // 恢复
    h.virt->setPresentResult(DisplayStatus::kOk);
    h.phys->setPresentResult(DisplayStatus::kOk);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
}

// 13. 接口契约：init(caps) 落定 capabilities；status() 反映最近一次操作；
//     Router 访问器返回同一 sink 实例。
void testSinkInterface() {
    Harness h;
    const DisplayCapabilities req;
    CHECK_EQ(static_cast<int>(h.virt->init(req)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->initCalls, 1);
    CHECK_EQ(h.virt->initCaps_.width, req.width);
    const DisplayCapabilities& caps = h.virt->capabilities();
    CHECK_EQ(caps.width, 320);
    CHECK_EQ(caps.height, 240);
    CHECK_EQ(caps.color, 16);
    CHECK(!caps.mono);
    CHECK(caps.canReadback);
    CHECK_EQ(static_cast<int>(caps.sinkKind), static_cast<int>(DisplaySinkKind::kVirtual));
    CHECK_EQ(static_cast<int>(h.virt->status()), static_cast<int>(DisplayStatus::kOk));

    // Router 访问器
    CHECK(h.router.virtualSink() == h.virt);
    CHECK(h.router.physicalSink() == h.phys);

    // presentScene 在 kIdle（未选择模式）→ kNotSupported（模式检查先于状态检查）
    const Rect r{0, 0, 2, 2};
    const uint8_t px[4] = {1, 2, 3, 4};
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kDiagnostics, r, px)),
             static_cast<int>(DisplayStatus::kNotSupported));
}


// ---- M7-C2 追加用例（任务 §21 13-19：PhysicalOnly 路由 / Mirror 扇出 /
// Split 场景 / physical unavailable / physical degraded / physical failure
// 不阻塞 virtual / physical enable-disable / scene 切换）----

// 14. PhysicalOnly 路由：应用帧只进 physical；virtual 不启用、不收帧。
void testC2PhysicalOnlyRoutes() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK(!h.virt->enabled_);
    CHECK(h.phys->enabled_);

    const Rect r{0, 0, 64, 32};
    const uint8_t px[4] = {0xA1, 0xA2, 0xA3, 0xA4};
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(0));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    if (h.phys->presents_.size() == 1) {
        CHECK(sameRect(h.phys->presents_[0].first, r));
    }
    CHECK_EQ(h.virt->flushCalls, 0);
    CHECK_EQ(h.phys->flushCalls, 1);
}

// 15. Mirror 双 sink 扇出：virtual + physical 都收到同一 rect（同帧同内容）。
void testC2MirrorFanout() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    const Rect r{8, 8, 16, 8};
    const uint8_t px[8] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8};
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    if (h.virt->presents_.size() == 1 && h.phys->presents_.size() == 1) {
        CHECK(sameRect(h.virt->presents_[0].first, r));
        CHECK(sameRect(h.phys->presents_[0].first, r));
        CHECK_EQ(h.virt->presents_[0].second, 0xB1);
        CHECK_EQ(h.phys->presents_[0].second, 0xB1);
    }
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->flushCalls, 1);
    CHECK_EQ(h.phys->flushCalls, 1);
}

// 16. Split 场景：writeRect 只进 virtual；presentScene（Diagnostics/Application）
//     只进 physical；不同场景的 rect 都能送达 physical。
void testC2SplitSceneRouting() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kSplit)),
             static_cast<int>(DisplayStatus::kOk));
    const Rect app{0, 0, 20, 10};
    const uint8_t appPx[4] = {0xC1, 0xC2, 0xC3, 0xC4};
    CHECK_EQ(static_cast<int>(h.router.writeRect(app, appPx)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(0));   // 应用帧不进 physical（Split）

    const Rect diag{10, 10, 8, 4};
    const uint8_t diagPx[4] = {0xD1, 0xD2, 0xD3, 0xD4};
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kDiagnostics, diag, diagPx)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));   // 场景帧不进 virtual
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    if (h.phys->presents_.size() == 1) {
        CHECK(sameRect(h.phys->presents_[0].first, diag));
        CHECK_EQ(h.phys->presents_[0].second, 0xD1);
    }
    // 另一场景（Application）同样只进 physical。
    const Rect app2{30, 30, 4, 4};
    const uint8_t app2Px[4] = {0xE1, 0xE2, 0xE3, 0xE4};
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kApplication, app2, app2Px)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.phys->presents_.size(), size_t(2));
    if (h.phys->presents_.size() == 2) {
        CHECK(sameRect(h.phys->presents_[1].first, app2));
    }
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
}

// 17. physical unavailable（PhysicalOnly）：attach 但 isAvailable()==false →
//     DEGRADED，写路径 kNotConnected；恢复后 CONNECTED。
void testC2PhysicalUnavailable() {
    DisplayRouter router;
    auto virt = makeVirtual(true);
    auto phys = makePhysical(false);   // I2C/物理链路未就绪
    router.attachVirtual(virt);
    router.attachPhysical(phys);
    CHECK_EQ(static_cast<int>(router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kDegraded));
    const uint8_t px[2] = {0xF1, 0xF2};
    CHECK_EQ(static_cast<int>(router.writeRect(Rect{0, 0, 2, 1}, px)),
             static_cast<int>(DisplayStatus::kNotConnected));
    CHECK_EQ(phys->presents_.size(), size_t(0));

    phys->setAvailable(true);
    router.refreshState();
    CHECK_EQ(static_cast<int>(router.state()), static_cast<int>(RouterState::kConnected));
    CHECK_EQ(static_cast<int>(router.writeRect(Rect{0, 0, 2, 1}, px)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(phys->presents_.size(), size_t(1));
}

// 18. physical degraded（Mirror）：physical 运行期不可用 → DEGRADED，
//     virtual 继续收帧；恢复后回 CONNECTED。
void testC2PhysicalDegradedMirror() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    const Rect r{1, 1, 12, 6};
    const uint8_t px[4] = {0x11, 0x22, 0x33, 0x44};
    h.phys->setAvailable(false);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kDegraded));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(0));
    h.phys->setAvailable(true);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.state()), static_cast<int>(RouterState::kConnected));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
}

// 19. physical failure 不阻塞 virtual：Mirror 下 physical present/flush 返回
//     错误 → Router 仍 kOk（virtual 成功即不阻塞 UI，DESIGN.md F 节）；
//     全部失败才传播首个错误。
void testC2PhysicalFailureDoesNotBlockVirtual() {
    Harness h;
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    const Rect r{2, 2, 8, 4};
    const uint8_t px[4] = {0x71, 0x72, 0x73, 0x74};
    h.phys->setPresentResult(DisplayStatus::kInternal);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));   // 仍被调用（present 错误由 sink 自身承载）
    // physical flush 错误不影响 virtual flush。
    h.phys->setPresentResult(DisplayStatus::kOk);
    class FlushFailSink : public RecordingSink {
    public:
        explicit FlushFailSink(DisplaySinkKind kind) : RecordingSink(kind, 128, 64, true) {}
        DisplayStatus flush() override { return DisplayStatus::kInternal; }
    };
    auto ff = std::make_shared<FlushFailSink>(DisplaySinkKind::kPhysical);
    h.router.attachPhysical(ff);
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.flush()), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->flushCalls, 1);
    // 全部失败：virtual kQueueFull + physical 失败 → 传播首个错误（不静默吞掉）。
    h.virt->setPresentResult(DisplayStatus::kQueueFull);
    ff->setPresentResult(DisplayStatus::kInternal);
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)),
             static_cast<int>(DisplayStatus::kQueueFull));
    h.virt->setPresentResult(DisplayStatus::kOk);
    ff->setPresentResult(DisplayStatus::kOk);
}

// 20. physical enable-disable：Router setMode 统一 enable/disable 两个 sink；
//     被 disable 的 physical 不接收应用帧（setEnabled 控制应用帧接收）。
void testC2PhysicalEnableDisable() {
    Harness h;
    // PhysicalOnly：physical 启用。
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK(h.phys->enabled_);
    CHECK(!h.virt->enabled_);
    // 切回 VirtualOnly：physical 被 disable（应用帧禁用 → OLED 诊断页继续）。
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK(!h.phys->enabled_);
    CHECK(h.virt->enabled_);
    const uint8_t px[2] = {0x81, 0x82};
    CHECK_EQ(static_cast<int>(h.router.writeRect(Rect{0, 0, 2, 1}, px)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.phys->presents_.size(), size_t(0));   // 未启用：不收应用帧
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    // 再切 Mirror：两个都启用。
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK(h.virt->enabled_);
    CHECK(h.phys->enabled_);
    CHECK_EQ(static_cast<int>(h.router.writeRect(Rect{0, 0, 2, 1}, px)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
}

// 21. scene 切换：模式循环 VirtualOnly→Mirror→Split→PhysicalOnly 下，写路径与
//     presentScene 的目标集始终匹配模式（场景概念仅属 Split）。
void testC2SceneSwitch() {
    Harness h;
    const uint8_t px[2] = {0x91, 0x92};
    const Rect r{0, 0, 4, 2};
    // VirtualOnly：只 virtual。
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kVirtualOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(1));
    CHECK_EQ(h.phys->presents_.size(), size_t(0));
    // Mirror：双 sink。
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kMirror)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(2));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    // Split：应用帧只 virtual；presentScene 只 physical（场景概念仅属 Split）。
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kSplit)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(3));
    CHECK_EQ(h.phys->presents_.size(), size_t(1));
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kDiagnostics, r, px)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.phys->presents_.size(), size_t(2));
    // PhysicalOnly：只 physical。
    CHECK_EQ(static_cast<int>(h.router.setMode(DisplayRouteMode::kPhysicalOnly)),
             static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(static_cast<int>(h.router.writeRect(r, px)), static_cast<int>(DisplayStatus::kOk));
    CHECK_EQ(h.virt->presents_.size(), size_t(3));
    CHECK_EQ(h.phys->presents_.size(), size_t(3));
    // Split 外 presentScene → kNotSupported（场景概念仅属 Split）。
    CHECK_EQ(static_cast<int>(h.router.presentScene(PhysicalScene::kApplication, r, px)),
             static_cast<int>(DisplayStatus::kNotSupported));
}
}  // namespace

void runDisplayRouterTests() {
    std::printf("[display_router] tests\n");
    testVirtualOnly();
    testPhysicalOnly();
    testMirrorFanout();
    testSplitIndependent();
    testSinkUnavailable();
    testSinkDegradedMirror();
    testTransportIndependent();
    testModeSwitch();
    testFullResyncRequested();
    testStaleFrameClearing();
    testSetModeValidation();
    testPresentErrorAggregation();
    testSinkInterface();
    // M7-C2 追加
    testC2PhysicalOnlyRoutes();
    testC2MirrorFanout();
    testC2SplitSceneRouting();
    testC2PhysicalUnavailable();
    testC2PhysicalDegradedMirror();
    testC2PhysicalFailureDoesNotBlockVirtual();
    testC2PhysicalEnableDisable();
    testC2SceneSwitch();
    std::printf("[display_router] done\n");
}