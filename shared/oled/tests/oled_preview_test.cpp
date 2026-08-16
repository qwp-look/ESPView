// ESPView M7-D2 — OledPreviewSlot Host Tests。
//
// 覆盖：
//   1. 无效槽：fresh / reset 后 snapshot=false、payload 空、valid()=false；
//      store(nullptr) no-op；
//   2. store→snapshot 往返：1KB 逐字节一致 + frameId 递增（0 起）；
//   3. payload AE.2 布局逐字段断言：frameId/width/height LE、pixelFormat/flags、
//      1024B 像素逐字节；非默认头参数编码；
//   4. 覆盖合并：store 两次只留最新（snapshot 与 payload 像素均为后帧）；
//   5. reset 清零：valid 清除、snapshot 拒绝、frameId 归零（复位后首帧 id=0）；
//   6. frameId 回绕：0..65535 → 0；
//   7. 并发 seqlock 不撕裂：写线程交替 store(A)/store(B)，读线程并发
//      snapshot/payload，任何成功帧像素必须完整等于 A 或 B（无混合/半帧），
//      frameId 严格 +1。
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

constexpr size_t kPixelsOffset = 8;

// 全字节填充模式（与 OledFb 页式无关，仅验证槽复制/编码的字节保真）。
void fillPattern(uint8_t* dst, uint8_t seed) {
    for (size_t i = 0; i < OledPreviewSlot::kSizeBytes; ++i) {
        dst[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i));
    }
}

bool payloadPixelsMatch(const std::vector<uint8_t>& payload,
                        const uint8_t* expected) {
    if (payload.size() != OledPreviewSlot::kPayloadSizeBytes) {
        return false;
    }
    return std::memcmp(payload.data() + kPixelsOffset, expected,
                       OledPreviewSlot::kSizeBytes) == 0;
}

// ---- 1. 无效槽 ----

void invalidSlotRejects() {
    OledPreviewSlot slot;
    CHECK(!slot.valid());
    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    uint16_t fid = 0xABCD;
    CHECK(!slot.snapshot(out, fid));
    CHECK_EQ(fid, uint16_t(0xABCD));  // 失败不触碰 out param
    CHECK(slot.makePhysicalPreviewPayload().empty());
    // 非法出参：拒绝且不崩溃。
    CHECK(!slot.snapshot(nullptr, fid));
}

void storeNullIsNoop() {
    OledPreviewSlot slot;
    slot.store(nullptr);
    CHECK(!slot.valid());
    CHECK(slot.makePhysicalPreviewPayload().empty());
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

// ---- 3. payload AE.2 布局 ----

void payloadLayout() {
    OledPreviewSlot slot;
    uint8_t src[OledPreviewSlot::kSizeBytes] = {};
    fillPattern(src, 0x5A);
    slot.store(src);

    // 推进 frameId 到 257（快照返回 0..256），使 frameId 高字节非零。
    uint8_t out[OledPreviewSlot::kSizeBytes] = {};
    for (uint16_t i = 0; i < 257; ++i) {
        uint16_t fid = 0;
        CHECK(slot.snapshot(out, fid));
        CHECK_EQ(fid, i);
    }
    CHECK(std::memcmp(out, src, OledPreviewSlot::kSizeBytes) == 0);

    const std::vector<uint8_t> payload = slot.makePhysicalPreviewPayload();
    CHECK_EQ(payload.size(), size_t(OledPreviewSlot::kPayloadSizeBytes));
    // frameId = 257 = 0x0101 → LE {0x01, 0x01}（取最新，已递增）。
    CHECK_EQ(int(payload[0]), 0x01);
    CHECK_EQ(int(payload[1]), 0x01);
    // width 128 = 0x0080 → LE {0x80, 0x00}。
    CHECK_EQ(int(payload[2]), 0x80);
    CHECK_EQ(int(payload[3]), 0x00);
    // height 64 = 0x0040 → LE {0x40, 0x00}。
    CHECK_EQ(int(payload[4]), 0x40);
    CHECK_EQ(int(payload[5]), 0x00);
    CHECK_EQ(int(payload[6]), int(OledPreviewSlot::kMono1));
    CHECK_EQ(int(payload[7]), 0);
    CHECK(payloadPixelsMatch(payload, src));

    // 非默认头参数（pixelFormat/flags/自定义 width/height）原样编码。
    const std::vector<uint8_t> custom =
        slot.makePhysicalPreviewPayload(100, 50, 2, 0x03);
    CHECK_EQ(custom.size(), size_t(OledPreviewSlot::kPayloadSizeBytes));
    CHECK_EQ(int(custom[0]), 0x02);  // frameId 258 = 0x0102 → LE {0x02, 0x01}
    CHECK_EQ(int(custom[1]), 0x01);
    CHECK_EQ(int(custom[2]), 100);
    CHECK_EQ(int(custom[3]), 0);
    CHECK_EQ(int(custom[4]), 50);
    CHECK_EQ(int(custom[5]), 0);
    CHECK_EQ(int(custom[6]), 2);
    CHECK_EQ(int(custom[7]), 0x03);
    CHECK(payloadPixelsMatch(custom, src));
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
    CHECK(std::memcmp(out, b, OledPreviewSlot::kSizeBytes) == 0);
    CHECK(std::memcmp(out, a, OledPreviewSlot::kSizeBytes) != 0);

    const std::vector<uint8_t> payload = slot.makePhysicalPreviewPayload();
    CHECK(payloadPixelsMatch(payload, b));
    CHECK(!payloadPixelsMatch(payload, a));
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
    CHECK(slot.makePhysicalPreviewPayload().empty());

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
            if ((i & 1) == 0) {
                if (!slot.snapshot(out, fid)) {
                    ++dropped;  // 仅写者极端连发（µs 级）时可能发生
                    continue;
                }
                CHECK((std::memcmp(out, a, OledPreviewSlot::kSizeBytes) == 0) ||
                      (std::memcmp(out, b, OledPreviewSlot::kSizeBytes) == 0));
            } else {
                const std::vector<uint8_t> payload = slot.makePhysicalPreviewPayload();
                if (payload.empty()) {
                    ++dropped;
                    continue;
                }
                CHECK_EQ(payload.size(), size_t(OledPreviewSlot::kPayloadSizeBytes));
                CHECK_EQ(int(payload[2]), int(OledPreviewSlot::kWidth) & 0xFF);
                CHECK_EQ(int(payload[4]), int(OledPreviewSlot::kHeight) & 0xFF);
                CHECK_EQ(int(payload[6]), int(OledPreviewSlot::kMono1));
                CHECK_EQ(int(payload[7]), 0);
                CHECK(payloadPixelsMatch(payload, a) || payloadPixelsMatch(payload, b));
                fid = static_cast<uint16_t>(payload[0] |
                                            (static_cast<uint16_t>(payload[1]) << 8));
            }
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

}  // namespace

// M7-D2：供 shared/protocol host 套件调用（test_main.cpp 声明）；独立目标
// oled_preview_test 经 OLED_PREVIEW_TEST_MAIN 提供 main 直接调用本函数。
void runOledPreviewTests() {
    invalidSlotRejects();
    storeNullIsNoop();
    storeSnapshotRoundTrip();
    payloadLayout();
    storeOverwriteKeepsLatest();
    resetClearsSlotAndFrameId();
    frameIdWraps();
    concurrentNoTornFrames();
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
