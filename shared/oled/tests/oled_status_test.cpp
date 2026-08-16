// ESPView M7-B — OLED 配置校验 / 恢复策略状态机 / 状态渲染 Host Tests。
//
// 覆盖：
//   1. validateOledConfig：默认合法、逐字段非法拒绝、maxTaskPriority 参数、
//      合法边界（Kconfig range 端点）接受；
//   2. ReinitPolicy：初始放行、指数退避（500→…→30000 封顶）、冷却、
//      成功复位、1000 次失败有界（防重置风暴）；
//   3. renderStatus：8 行文本逐字符逐列位模式精确匹配 + clamp（FRM 6 位 /
//      HEAP 8 位 / UP 小时 99）+ 回退显示（'?' / "--"）；
//   4. uptime 渲染与小时 clamp。
// 纯 C++17，零平台依赖；并入 shared/protocol host 套件。

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "oled_config.h"
#include "oled_fb.h"
#include "oled_recovery.h"
#include "oled_status.h"
#include "test_util.h"

namespace {

using espview::oled::ControllerType;
using espview::oled::OledConfig;
using espview::oled::OledFb;
using espview::oled::ReinitPolicy;
using espview::oled::StatusSnapshot;
using espview::oled::renderStatus;
using espview::oled::validateOledConfig;

// ---- 1. 配置校验 ----

void configValidDefault() {
    OledConfig cfg;
    CHECK(validateOledConfig(cfg));
    CHECK_EQ(cfg.refreshMs, uint32_t(500));
    CHECK_EQ(cfg.taskPriority, uint32_t(2));
    CHECK_EQ(cfg.maxReinit, uint32_t(3));
    CHECK_EQ(cfg.clkHz, uint32_t(400000));
    CHECK_EQ(cfg.controller, ControllerType::kAuto);
}

void configRejectsEachInvalid() {
    OledConfig base;
    CHECK(validateOledConfig(base));

    // 合法边界（Kconfig range 端点）全部接受。
    OledConfig lo = base;
    lo.sdaGpio = 0;
    lo.sclGpio = 0;
    lo.clkHz = 100000;
    lo.refreshMs = 100;
    lo.taskStack = 2048;
    lo.taskPriority = 1;
    lo.i2cTimeoutMs = 5;
    lo.maxReinit = 1;
    CHECK(validateOledConfig(lo));

    OledConfig hi = base;
    hi.clkHz = 1000000;
    hi.refreshMs = 60000;
    hi.taskStack = 16384;
    hi.taskPriority = 2;
    hi.i2cTimeoutMs = 1000;
    hi.maxReinit = 100;
    CHECK(validateOledConfig(hi));

    // 逐字段非法 → 拒绝。
    OledConfig c = base; c.sdaGpio = -1;     CHECK(!validateOledConfig(c));
    c = base; c.sclGpio = -1;                CHECK(!validateOledConfig(c));
    c = base; c.clkHz = 99999;               CHECK(!validateOledConfig(c));
    c = base; c.clkHz = 1000001;             CHECK(!validateOledConfig(c));
    c = base; c.refreshMs = 99;              CHECK(!validateOledConfig(c));
    c = base; c.refreshMs = 60001;           CHECK(!validateOledConfig(c));
    c = base; c.taskStack = 2047;            CHECK(!validateOledConfig(c));
    c = base; c.taskPriority = 0;            CHECK(!validateOledConfig(c));
    c = base; c.taskPriority = 3;            CHECK(!validateOledConfig(c));
    c = base; c.i2cTimeoutMs = 4;            CHECK(!validateOledConfig(c));
    c = base; c.i2cTimeoutMs = 1001;         CHECK(!validateOledConfig(c));
    c = base; c.maxReinit = 0;               CHECK(!validateOledConfig(c));
    c = base; c.maxReinit = 101;             CHECK(!validateOledConfig(c));

    // maxTaskPriority 参数生效：上限可放宽/收紧。
    c = base;
    c.taskPriority = 3;
    CHECK(!validateOledConfig(c, 2));
    CHECK(validateOledConfig(c, 3));
    CHECK(validateOledConfig(c, 100));
    c.taskPriority = 2;
    CHECK(!validateOledConfig(c, 1));
    CHECK(validateOledConfig(c, 2));
    CHECK(!validateOledConfig(c, 0));
}

// ---- 2. 恢复策略状态机 ----

void recoveryInitialAllowed() {
    ReinitPolicy p;
    CHECK_EQ(p.backoffMs(), uint64_t(500));
    CHECK_EQ(p.nextAttemptMs(), uint64_t(0));
    CHECK_EQ(p.cooldownUntilMs(), uint64_t(0));
    CHECK_EQ(p.cycles(), uint32_t(0));
    // 首次 tryBegin(任意 now) 允许。
    CHECK(p.tryBegin(0));
    ReinitPolicy p2;
    CHECK(p2.tryBegin(12345));
    CHECK_EQ(p2.nextAttemptMs(), uint64_t(12345 + 500));
    // 预约后未到点不允许再次尝试。
    CHECK(!p2.tryBegin(12345 + 499));
    CHECK(p2.tryBegin(12345 + 500));
}

void recoveryBackoffDoubling() {
    // 放大多轮窗口（不触发冷却），聚焦退避翻倍与 30000 封顶。
    ReinitPolicy::Params params;
    params.maxCycles = 100;
    ReinitPolicy p(params);
    CHECK_EQ(p.backoffMs(), uint64_t(500));

    uint64_t t = 0;
    uint64_t gap = 500;  // 每次 tryBegin 预约使用的 backoff（失败前）。
    for (int i = 0; i < 8; ++i) {
        CHECK(p.tryBegin(t));
        p.onFailure(t);
        // 失败后 nextAttempt = now + 预约时使用的 backoff。
        CHECK_EQ(p.nextAttemptMs(), t + gap);
        gap = std::min<uint64_t>(gap * 2, 30000);
        CHECK_EQ(p.backoffMs(), gap);  // 500→1000→2000→4000→…→30000
        t = p.nextAttemptMs();
    }
    // 封顶 30000：再多失败也不再增长。
    const uint64_t prev = p.backoffMs();
    CHECK(p.tryBegin(t));
    p.onFailure(t);
    CHECK_EQ(p.backoffMs(), prev);
    CHECK_EQ(p.backoffMs(), uint64_t(30000));
}

void recoveryCooldownAfterMaxCycles() {
    ReinitPolicy p;  // 默认：maxCycles=3, cooldown=30000
    const uint64_t t0 = 1000;
    CHECK(p.tryBegin(t0));
    p.onFailure(t0);  // 1
    uint64_t t = p.nextAttemptMs();
    CHECK(p.tryBegin(t));
    p.onFailure(t);   // 2
    t = p.nextAttemptMs();
    CHECK(p.tryBegin(t));
    p.onFailure(t);   // 3 → 冷却
    CHECK_EQ(p.cycles(), uint32_t(0));
    CHECK_EQ(p.backoffMs(), uint64_t(500));
    CHECK_EQ(p.cooldownUntilMs(), t + 30000);
    // 冷却期内不允许尝试。
    CHECK(!p.tryBegin(t + 29999));
    // 冷却结束恢复允许。
    CHECK(p.tryBegin(t + 30000));
}

void recoverySuccessResets() {
    ReinitPolicy p;
    uint64_t t = 0;
    CHECK(p.tryBegin(t));
    p.onFailure(t);  // cycles=1, backoff=1000
    CHECK_EQ(p.cycles(), uint32_t(1));
    CHECK_EQ(p.backoffMs(), uint64_t(1000));

    const uint64_t t2 = p.nextAttemptMs();
    CHECK(p.tryBegin(t2));
    p.onSuccess(t2);  // 复位并预约 next=now+base
    CHECK_EQ(p.cycles(), uint32_t(0));
    CHECK_EQ(p.backoffMs(), uint64_t(500));
    CHECK_EQ(p.nextAttemptMs(), t2 + 500);
    CHECK(!p.tryBegin(t2 + 499));
    CHECK(p.tryBegin(t2 + 500));
}

void recoveryBoundedNoInfiniteLoop() {
    ReinitPolicy p;  // 默认参数
    uint64_t t = 0;
    uint32_t failures = 0;
    uint32_t prevCycle = 0;
    uint64_t lastCooldownEnd = 0;
    uint64_t recentFailures[4] = {0, 0, 0, 0};
    int recentCount = 0;

    while (failures < 1000) {
        if (t < p.nextAttemptMs()) {
            t = p.nextAttemptMs();
        }
        if (t < p.cooldownUntilMs()) {
            t = p.cooldownUntilMs();
        }
        CHECK(p.tryBegin(t));
        CHECK_MSG(t >= lastCooldownEnd, "no attempt during cooldown window");
        p.onFailure(t);
        ++failures;

        // 不变量：cycles 增量有界（1..maxCycles）、backoff 封顶、下次尝试已预约。
        CHECK(p.cycles() <= 3);
        CHECK(p.backoffMs() <= 30000);
        CHECK(p.nextAttemptMs() > t);

        if (p.cycles() == 0) {
            // 刚进入冷却：cooldownUntil 推进、冷却期内 tryBegin 拒绝。
            CHECK_EQ(p.backoffMs(), uint64_t(500));
            CHECK(p.cooldownUntilMs() >= t + 30000);
            CHECK(!p.tryBegin(t + 1));
            CHECK(!p.tryBegin(t + 29999));
            lastCooldownEnd = p.cooldownUntilMs();
        } else {
            CHECK_EQ(p.cycles(), prevCycle + 1);  // 每次失败增量恰好 1
        }
        prevCycle = p.cycles();

        // 任意 30s 窗口内失败数 ≤ 3（防重置风暴）。
        int inWindow = 0;
        const int check = recentCount < 4 ? recentCount : 4;
        for (int i = 0; i < check; ++i) {
            if (t - recentFailures[(recentCount - 1 - i) % 4] <= 30000) {
                ++inWindow;
            }
        }
        CHECK_MSG(inWindow < 3, "at most 3 failures per 30s window");
        recentFailures[recentCount % 4] = t;
        ++recentCount;

        ++t;
    }
    CHECK_EQ(failures, uint32_t(1000));
}

// ---- 3. 状态渲染 ----

// 行文本逐字符逐列比较：基于字体位模式精确校验 drawText 输出
// （字形 r 行 col 位 → 页式字节 bit r；行 y 为 8 的倍数时整行在单个 page）。
bool rowMatches(OledFb& fb, int row, const char* text) {
    if (row < 0 || row >= OledFb::kHeight) {
        return false;
    }
    const int page = row / 8;
    for (size_t k = 0; text[k] != '\0'; ++k) {
        const uint8_t* glyph = OledFb::fontGlyph(text[k]);
        for (int col = 0; col < OledFb::kFontWidth; ++col) {
            uint8_t slice = 0;
            for (int r = 0; r < OledFb::kFontHeight; ++r) {
                if ((glyph[r] >> col) & 1u) {
                    slice |= static_cast<uint8_t>(1u << r);
                }
            }
            const int x = static_cast<int>(k * OledFb::kFontWidth) + col;
            if (x >= OledFb::kWidth) {
                return false;
            }
            if (fb.byteAt(page, x) != slice) {
                return false;
            }
        }
    }
    return true;
}

void statusRenderLines() {
    StatusSnapshot s;
    s.transportType = 1;              // TCP
    s.transportConnected = true;
    s.sessionState = 1;               // CONN
    std::snprintf(s.ip, sizeof(s.ip), "192.0.2.15");
    s.apInfoValid = true;
    s.rssi = -55;
    s.channel = 6;
    s.frameCount = 123456;
    s.errorCount = 7;
    s.uptimeMs = 3661000;             // 1h 01m 01s
    s.freeHeap = 123456;

    OledFb fb;
    renderStatus(fb, s);

    // 8 行布局（每行 8px，行内逐字符逐列位模式精确匹配）。
    CHECK(rowMatches(fb, 0, "ESPView"));
    CHECK(rowMatches(fb, 8, "TCP CONN"));
    CHECK(rowMatches(fb, 16, "IP 192.0.2.15"));
    CHECK(rowMatches(fb, 24, "RSSI -55 CH 6"));
    CHECK(rowMatches(fb, 32, "FRM 123456"));
    CHECK(rowMatches(fb, 40, "ERR 7"));
    CHECK(rowMatches(fb, 48, "HEAP 123456"));
    CHECK(rowMatches(fb, 56, "UP 01:01:01"));

    // 行 0 像素非空（标题已渲染）。
    bool titleNonEmpty = false;
    for (int x = 0; x < OledFb::kWidth; ++x) {
        if (fb.byteAt(0, x) != 0) {
            titleNonEmpty = true;
            break;
        }
    }
    CHECK_MSG(titleNonEmpty, "row 0 must render ESPView title pixels");

    // 回退与 clamp：UART / sessionState 越界→'?' / IP 不可用→"--" /
    // RSSI 无效→"--" / FRM 12345678→999999 / HEAP 123456789→99999999。
    StatusSnapshot f;
    f.transportType = 0;              // UART
    f.sessionState = 9;               // 越界 → '?'
    f.apInfoValid = false;
    f.frameCount = 12345678;
    f.errorCount = 0;
    f.freeHeap = 123456789;
    f.uptimeMs = 0;
    renderStatus(fb, f);

    CHECK(rowMatches(fb, 8, "UART ?"));
    CHECK(rowMatches(fb, 16, "IP --"));
    CHECK(rowMatches(fb, 24, "RSSI -- CH --"));
    CHECK(rowMatches(fb, 32, "FRM 999999"));
    CHECK(rowMatches(fb, 40, "ERR 0"));
    CHECK(rowMatches(fb, 48, "HEAP 99999999"));

    // clamp：FRM 6 位封顶，第 7 位（x=80 起）必须为 0。
    bool seventhDigitBlank = true;
    for (int x = 80; x < OledFb::kWidth; ++x) {
        if (fb.byteAt(4, x) != 0) {
            seventhDigitBlank = false;
            break;
        }
    }
    CHECK_MSG(seventhDigitBlank, "FRM clamp leaves 7th digit column blank");
}

void statusRenderUptime() {
    OledFb fb;
    StatusSnapshot s;

    s.uptimeMs = 3661000;             // 1h 01m 01s
    renderStatus(fb, s);
    CHECK(rowMatches(fb, 56, "UP 01:01:01"));

    s.uptimeMs = 360000ull * 1000ull; // 100h → 小时 clamp 99
    renderStatus(fb, s);
    CHECK(rowMatches(fb, 56, "UP 99:00:00"));

    s.uptimeMs = 3599999;             // 0h 59m 59s（分/秒边界）
    renderStatus(fb, s);
    CHECK(rowMatches(fb, 56, "UP 00:59:59"));
}

}  // namespace

void runOledStatusTests() {
    std::printf("[oled_status]\n");
    configValidDefault();
    configRejectsEachInvalid();
    recoveryInitialAllowed();
    recoveryBackoffDoubling();
    recoveryCooldownAfterMaxCycles();
    recoverySuccessResets();
    recoveryBoundedNoInfiniteLoop();
    statusRenderLines();
    statusRenderUptime();
}