// ESPView M7-D2 — OledPreviewSlot Host Tests。
//
// 覆盖（M8-A4：AE.2 编码已归 protocol 层，本测试只验证槽语义）：
//   1. 无效槽：fresh / reset 后 snapshot=false、valid()=false；store(nullptr) no-op；
//   2. store→snapshot 往返：1KB 逐字节一致 + frameId 递增（0 起）；
//   3. frameId 长序列递增 + 高字节回绕（AE.2 0..65535）；
//   4. 覆盖合并：store 两次只留最新（snapshot 像素为后帧）；
//   5. reset 清零：valid 清除、snapshot 拒绝、frameId 归零（复位后首帧 id=0）；
//   6. frameId 回绕：0..65535 → 0；
//   7. 并发 seqlock 不撕裂：写线程交替 store(A)/store(B)，读线程并发
//      snapshot，任何成功帧像素必须完整等于 A 或 B（无混合/半帧），
//      frameId 严格 +1；
//   8. M8-A4 写者互斥回归：store 与 reset 双线程并发，槽内容必须始终是
//      某次完整发布（A/B/全零），不得撕裂（修复 Agent C/K 双写者竞态）。
// 纯 C++17，零平台依赖；独立可执行（CMake 目标 oled_preview_test，定义
// OLED_PREVIEW_TEST_MAIN 提供 main），亦可并入 shared/protocol host 套件
// （去掉该定义后由 test_main.cpp 调用 runOledPreviewTests()）。

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

#include "oled_preview.h"
#include "test_util.h"

namespace {

using espview::oled::OledPreviewSlot;

// 全字节填充模式（与 OledFb 页式无关，仅验证槽复制字节保真）。
void fillPattern(uint8_t* dst, uint8_t seed) {
    for (size_t i = 0; i < OledPreviewSlot::kSizeBytes; ++i) {
        dst[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
    }
}

// 帧字节是否完整等于某参考帧（撕裂检测用）。
bool bytesEqual(const uint8_t* lhs, const uint8_t* rhs) {
    return std::memcmp(lhs, rhs, OledPreviewSlot::kSizeBytes) == 0;
}

// ---- 1. 无效槽 ----

void invalidSlotRejects() {
    OledPreviewSlot slot;
    CHECK(!slot.valid());
    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    uint16_t fid = 0xABCD;
    CHECK(!slot.snapshot(out, fid));
    CHECK_EQ(fid, uint16_t(0xABCD));  // 失败不触碰 out param
    // 非法出参：拒绝且不崩溃。
    CHECK(!slot.snapshot(nullptr, fid));
}

void storeNullIsNoop() {
    OledPreviewSlot slot;
    slot.store(nullptr);
    CHECK(!slot.valid());
    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    uint16_t fid = 0;
    CHECK(!slot.snapshot(out, fid));
}

// ---- 2. store→snapshot 往返 + frameId 递增 ----

void storeSnapshotRoundTrip() {
    OledPreviewSlot slot;
    uint8_t src[OledPreviewSlot::kSizeBytes] = {};
    fillPattern(src, 0x00);
    slot.store(src);
    CHECK(slot.valid());

    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    uint16_t fid = 0;
    CHECK(slot.snapshot(out, fid));
    CHECK_EQ(fid, uint16_t(0));  // reset 后首帧 id = 0
    CHECK(std::memcmp(out, src, OledPreviewSlot::kSizeBytes) == 0);

    // 第二次快照：帧内容不变，frameId 递增。
    uint8_t out2[OledPreviewSlot::kSizeBytes] = {};
    CHECK(slot.snapshot(out2, fid));
    CHECK_EQ(fid, uint16_t(1));
    CHECK(std::memcmp(out2, src, OledPreviewSlot::kSizeBytes) == 0);
}

// ---- 3. frameId 长序列递增（高字节非零）+ 字节保真 ----

void snapshotFrameIdAdvances() {
    OledPreviewSlot slot;
    uint8_t src[OledPreviewSlot::kSizeBytes] = {};
    fillPattern(src, 0x5A);
    slot.store(src);

    // 推进 frameId 到 257（快照返回 0..256），使 frameId 高字节非零
    // （AE.2 编码由 protocol 层负责，本层只保证计数与字节保真）。
    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    for (uint16_t i = 0; i < 257; ++i) {
        uint16_t fid = 0;
        CHECK(slot.snapshot(out, fid));
        CHECK_EQ(fid, i);
    }
    CHECK(bytesEqual(out, src));
}

// ---- 4. 覆盖合并（store 两次只留最新）----

void storeOverwriteKeepsLatest() {
    OledPreviewSlot slot;
    uint8_t a[OledPreviewSlot::kSizeBytes] = {};
    uint8_t b[OledPreviewSlot::kSizeBytes] = {};
    fillPattern(a, 0x00);
    fillPattern(b, 0xFF);
    slot.store(a);
    slot.store(b);  // 最新帧合并：覆盖旧槽

    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    uint16_t fid = 0;
    CHECK(slot.snapshot(out, fid));
    CHECK(bytesEqual(out, b));
    CHECK(!bytesEqual(out, a));
}

// ---- 5. reset 清零 ----

void resetClearsSlotAndFrameId() {
    OledPreviewSlot slot;
    uint8_t src[OledPreviewSlot::kSizeBytes] = {};
    fillPattern(src, 0x33);
    slot.store(src);

    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    uint16_t fid = 0;
    CHECK(slot.snapshot(out, fid));
    CHECK_EQ(fid, uint16_t(0));

    slot.reset();
    CHECK(!slot.valid());
    CHECK(!slot.snapshot(out, fid));  // 拒绝读取

    // reset 后重新 store：frameId 归零（新首帧 id = 0）。
    slot.store(src);
    CHECK(slot.snapshot(out, fid));
    CHECK_EQ(fid, uint16_t(0));
    CHECK(std::memcmp(out, src, OledPreviewSlot::kSizeBytes) == 0);
}

// ---- 6. frameId 0..65535 回绕 ----

void frameIdWraps() {
    OledPreviewSlot slot;
    uint8_t src[OledPreviewSlot::kSizeBytes] = {};
    fillPattern(src, 0x11);
    slot.store(src);

    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    uint16_t fid = 0;
    for (uint32_t i = 0; i < 65535; ++i) {
        CHECK(slot.snapshot(out, fid));
        CHECK_EQ(fid, uint16_t(i));
    }
    CHECK(slot.snapshot(out, fid));
    CHECK_EQ(fid, uint16_t(65535));  // 最后一帧 65535
    CHECK(slot.snapshot(out, fid));
    CHECK_EQ(fid, uint16_t(0));  // 回绕到 0（AE.2：0..65535 回绕）
}

// ---- 7. 并发 seqlock：无撕裂帧 ----

void concurrentNoTornFrames() {
    OledPreviewSlot slot;
    uint8_t a[OledPreviewSlot::kSizeBytes] = {};
    uint8_t b[OledPreviewSlot::kSizeBytes] = {};
    std::memset(a, 0xAA, sizeof(a));
    std::memset(b, 0x55, sizeof(b));
    slot.store(a);  // 预置有效帧，避免读侧撞上首帧窗口

    std::atomic<bool> start{false};
    std::atomic<int> writerStores{0};
    constexpr int kStoreIters = 1500;
    constexpr int kReadIters = 6000;
    // 写者发布节拍：1ms busy-wait（≈1kHz，仍远超设计上限 10Hz；本平台
    // std::this_thread::sleep_for 亚毫秒为空操作、毫秒级被 Windows 定时器量化
    // 到 ~15.6ms，故用 steady_clock 自旋模拟 OLED 周期发布）。缓存争用下 store 的
    // odd 窗口可放大到数十 µs，1ms 稳定窗口仍远大于它，读者基本不饿死（有界自旋兜底）。
    constexpr auto kPublishPeriod = std::chrono::microseconds(1000);  // ≈1kHz，仍 100× 设计上限 10Hz

    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kStoreIters; ++i) {
            slot.store((i & 1) == 0 ? a : b);
            ++writerStores;
            const auto deadline =
                std::chrono::steady_clock::now() + kPublishPeriod;
            while (std::chrono::steady_clock::now() < deadline) {
            }
        }
    });

    std::thread reader([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        int last = -1;
        int successes = 0;
        int dropped = 0;
        uint8_t out[OledPreviewSlot::kSizeBytes] = {};
        for (int i = 0; i < kReadIters; ++i) {
            uint16_t fid = 0;
            if (!slot.snapshot(out, fid)) {
                ++dropped;  // 仅写者极端连发（µs 级）时可能发生
                continue;
            }
            // 任何成功帧像素必须完整等于 A 或 B（无混合/半帧）。
            CHECK(bytesEqual(out, a) || bytesEqual(out, b));
            // frameId 严格 +1（每次成功读恰好消耗一个 id）。
            CHECK_EQ(int(fid), last + 1);
            last = fid;
            ++successes;
        }
        // 撕裂帧在任何调度/机器负载下都不得出现（上面像素不变式保证）。
        // 丢帧仅来自有界自旋（1000 次）在缓存争用/机器负载下的偶发耗尽 ——
        // AE.3 明确允许背压整帧丢弃（fire-and-forget 自重同步）；1kHz 测试节奏
        // （100× 设计上限）下以 100/6000 作有界校验，仍能捕获系统性饿死。
        CHECK(dropped < 100);
        CHECK(successes > 0);
    });

    start.store(true, std::memory_order_release);
    writer.join();
    CHECK_EQ(writerStores.load(std::memory_order_relaxed), kStoreIters);
    reader.join();
}

// ---- 8. M8-A4 回归：store/reset 双写者互斥（修复 Agent C/K 竞态）----
// 修复前 store（OLED 任务）与 reset（会话状态回调任务）并发会撕裂 seqlock
// 奇偶序列 + 槽字节；修复后 writeMutex_ 串行化写者。本测试让两写者并发，
// 读线程只允许看到 A / B / 全零三种完整帧之一（无撕裂），且槽状态自洽。
void concurrentStoreResetWriters() {
    OledPreviewSlot slot;
    uint8_t a[OledPreviewSlot::kSizeBytes];
    uint8_t b[OledPreviewSlot::kSizeBytes];
    std::memset(a, 0xA5, sizeof(a));
    std::memset(b, 0x5A, sizeof(b));
    slot.store(a);

    std::atomic<bool> start{false};
    std::atomic<int> storeDone{0};
    std::atomic<int> resetDone{0};
    constexpr int kStoreIters = 2000;
    constexpr int kResetIters = 200;
    constexpr int kReadIters = 4000;

    std::thread storer([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kStoreIters; ++i) {
            slot.store((i & 1) == 0 ? a : b);
            ++storeDone;
        }
    });
    std::thread reseter([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kResetIters; ++i) {
            slot.reset();
            ++resetDone;
        }
    });
    std::thread reader([&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        int drops = 0;
        uint8_t zero[OledPreviewSlot::kSizeBytes] = {};
        uint8_t out[OledPreviewSlot::kSizeBytes] = {};
        for (int i = 0; i < kReadIters; ++i) {
            uint16_t fid = 0;
            if (!slot.snapshot(out, fid)) {
                ++drops;  // 写者互斥下失败只来自复位窗口（valid=false）
                continue;
            }
            // 只允许完整帧：A / B / 全零（reset 发布），禁止混合。
            CHECK(bytesEqual(out, a) || bytesEqual(out, b) ||
                  bytesEqual(out, zero));
        }
        CHECK(drops >= 0);
    });

    start.store(true, std::memory_order_release);
    storer.join();
    reseter.join();
    reader.join();
    CHECK_EQ(storeDone.load(std::memory_order_relaxed), kStoreIters);
    CHECK_EQ(resetDone.load(std::memory_order_relaxed), kResetIters);
    // 终止后状态自洽：valid 与 snapshot 一致。
    if (slot.valid()) {
        uint8_t out[OledPreviewSlot::kSizeBytes] = {};
        uint16_t fid = 0;
        CHECK(slot.snapshot(out, fid));
        CHECK(bytesEqual(out, a) || bytesEqual(out, b));
    } else {
        uint8_t out[OledPreviewSlot::kSizeBytes] = {};
        uint16_t fid = 0;
        CHECK(!slot.snapshot(out, fid));
    }
}

}  // namespace

// M7-D2：供 shared/protocol host 套件调用（test_main.cpp 声明）；独立目标
// oled_preview_test 经 OLED_PREVIEW_TEST_MAIN 提供 main 直接调用本函数。
void runOledPreviewTests() {
    invalidSlotRejects();
    storeNullIsNoop();
    storeSnapshotRoundTrip();
    snapshotFrameIdAdvances();
    storeOverwriteKeepsLatest();
    resetClearsSlotAndFrameId();
    frameIdWraps();
    concurrentNoTornFrames();
    concurrentStoreResetWriters();
}

#if defined(OLED_PREVIEW_TEST_MAIN)
int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== OledPreviewSlot host tests ==\n");
    runOledPreviewTests();
    std::printf("----\nchecks: %d, failures: %d\n", espview::proto::test::gChecks.load(),
                espview::proto::test::gFailures.load());
    return espview::proto::test::gFailures.load() == 0 ? 0 : 1;
}
#endif
