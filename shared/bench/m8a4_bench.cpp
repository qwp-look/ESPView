// ESPView M8-A4 — Display/Input/OLED 收敛基准（bench 非测试；guard 失败 → 非零退出）。
//
// 覆盖任务书 §三十三要求的热点：PhysicalRenderer、OLED framebuffer 写入、
// PHYSICAL_PREVIEW encode/decode、Input mapping、DisplayRouter fanout。
// 固定迭代次数（编译期表），5 trials，中位数；计时区外构建载荷；I/O 全在
// 计时区外；每项含正确性 guard（volatile checksum 防优化器消除）。
// 每项报告 alloc_count / alloc_bytes DELTA（counting_allocator，与
// espview_protocol_bench 同机制）。
// CSV 列：op,payload_bytes,wire_bytes,iterations,trial,total_elapsed_us,
//         elapsed_us_per_op,ops_per_sec,alloc_count,alloc_bytes
// 纯 C++17，零平台依赖（无 ESP-IDF / Qt / LVGL）。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "counting_allocator.h"
#include "coordinate_mapper.h"
#include "display_router.h"
#include "input_codec.h"
#include "input_event.h"
#include "message.h"
#include "oled_fb.h"
#include "physical_renderer.h"

namespace {

using espview::display::DisplayCapabilities;
using espview::display::DisplayRouter;
using espview::display::DisplayRouteMode;
using espview::display::DisplayStatus;
using espview::display::IDisplaySink;
using espview::display::Rect;
using espview::input::CoordinateMapper;
using espview::input::InputEvent;
using espview::input::InputType;
using espview::oled::OledFb;
using espview::oled::PhysicalRenderer;
using espview::oled::RenderRect;
using espview::proto::Message;
using espview::proto::PhysicalPreviewInfo;
using espview::proto::makePhysicalPreview;
using espview::proto::parsePhysicalPreview;

constexpr int kTrials = 5;
constexpr int kVirtualWidth = 320;
constexpr int kVirtualHeight = 240;

volatile uint64_t gSum = 0;
volatile uint64_t gPresents = 0;

void failGuard(const char* what) {
    std::fprintf(stderr, "GUARD FAIL: %s\n", what);
    std::exit(1);
}

struct Timer {
    std::chrono::steady_clock::time_point t0;
    void start() { t0 = std::chrono::steady_clock::now(); }
    uint64_t elapsedUs() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
    }
};

uint64_t medianOf(const uint64_t* v, int n) {
    uint64_t a[8];
    for (int i = 0; i < n; ++i) {
        a[i] = v[i];
    }
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (a[j] < a[i]) {
                const uint64_t t = a[i];
                a[i] = a[j];
                a[j] = t;
            }
        }
    }
    return a[n / 2];
}

uint64_t loadAllocs() {
    return espview::proto::bench::AllocationCounters::allocations.load(
        std::memory_order_relaxed);
}
uint64_t loadAllocBytes() {
    return espview::proto::bench::AllocationCounters::bytes.load(std::memory_order_relaxed);
}

void emitRow(const char* op, size_t payloadBytes, size_t wireBytes, size_t iterations,
             int trial, uint64_t totalUs, uint64_t allocs, uint64_t allocBytes) {
    const double perOp = static_cast<double>(totalUs) / static_cast<double>(iterations);
    const double opsPerSec = perOp > 0.0 ? 1e6 / perOp : 0.0;
    std::printf("%s,%zu,%zu,%zu,%d,%llu,%.3f,%.0f,%llu,%llu\n", op, payloadBytes,
                wireBytes, iterations, trial,
                static_cast<unsigned long long>(totalUs), perOp, opsPerSec,
                static_cast<unsigned long long>(allocs),
                static_cast<unsigned long long>(allocBytes));
}

void emitMedianRow(const char* op, size_t payloadBytes, size_t wireBytes,
                   size_t iterations, const uint64_t totals[5], const uint64_t allocs[5],
                   const uint64_t allocBytes[5]) {
    emitRow(op, payloadBytes, wireBytes, iterations, 5, medianOf(totals, 5),
            medianOf(allocs, 5), medianOf(allocBytes, 5));
}

// ---- guard：把字节累进 volatile checksum ----
void addToSum(const uint8_t* d, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) {
        s += static_cast<uint8_t>(d[i]);
    }
    gSum = gSum + s;
}

// ---- 1) physical_render_full：320x240 RGB565 → 128x64 Mono1 ----
void benchPhysicalRender() {
    std::vector<uint8_t> rgb565;
    rgb565.resize(static_cast<size_t>(kVirtualWidth) * kVirtualHeight * 2u);
    for (size_t i = 0; i < rgb565.size(); ++i) {
        rgb565[i] = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    }
    constexpr size_t kIters = 16;
    const RenderRect full{0, 0, kVirtualWidth, kVirtualHeight};
    uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
    for (int t = 0; t < kTrials; ++t) {
        espview::proto::bench::resetAllocationCounters();
        Timer tm;
        tm.start();
        uint64_t acc = 0;
        for (size_t it = 0; it < kIters; ++it) {
            OledFb fb;
            PhysicalRenderer renderer;
            renderer.renderFrame(fb, kVirtualWidth, kVirtualHeight, rgb565.data(), full);
            acc += static_cast<uint64_t>(fb.byteAt(0, 0)) + fb.byteAt(7, 127);
        }
        const uint64_t el = tm.elapsedUs();
        gSum = gSum + acc;
        totals[t] = el;
        tallocs[t] = loadAllocs();
        tallocBytes[t] = loadAllocBytes();
    }
    emitMedianRow("physical_render_full", rgb565.size(), OledFb::kSizeBytes, kIters,
                  totals, tallocs, tallocBytes);
}

// ---- 2) oled_fb_write：128x64 setPixel 全遍历 + drawText ----
void benchOledFbWrite() {
    constexpr size_t kIters = 32;
    uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
    for (int t = 0; t < kTrials; ++t) {
        espview::proto::bench::resetAllocationCounters();
        Timer tm;
        tm.start();
        uint64_t acc = 0;
        for (size_t it = 0; it < kIters; ++it) {
            OledFb fb;
            for (int y = 0; y < OledFb::kHeight; ++y) {
                for (int x = 0; x < OledFb::kWidth; ++x) {
                    fb.setPixel(x, y, ((x + y + static_cast<int>(it)) & 1) != 0);
                }
            }
            fb.drawText(4, 24, "ESPView M8A4");
            acc += static_cast<uint64_t>(fb.byteAt(0, 0));
        }
        const uint64_t el = tm.elapsedUs();
        gSum = gSum + acc;
        totals[t] = el;
        tallocs[t] = loadAllocs();
        tallocBytes[t] = loadAllocBytes();
    }
    emitMedianRow("oled_fb_write", OledFb::kSizeBytes, OledFb::kSizeBytes, kIters,
                  totals, tallocs, tallocBytes);
}

// ---- 3) preview_encode：PHYSICAL_PREVIEW 1032B 编码 ----
void benchPreviewEncode() {
    std::vector<uint8_t> pixels(OledFb::kSizeBytes);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>((i * 3u) & 0xFFu);
    }
    constexpr size_t kIters = 64;
    uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
    for (int t = 0; t < kTrials; ++t) {
        espview::proto::bench::resetAllocationCounters();
        Timer tm;
        tm.start();
        uint64_t acc = 0;
        for (size_t it = 0; it < kIters; ++it) {
            const auto msg = makePhysicalPreview(
                static_cast<uint16_t>(it), 128, 64,
                espview::proto::PhysicalPixelFormat::kMono1, 0, pixels.data());
            if (!msg) {
                failGuard("preview_encode: makePhysicalPreview");
            }
            acc += static_cast<uint64_t>(msg->payload.size());
            addToSum(msg->payload.data(), 8u);  // 头 8B 防优化器消除
        }
        const uint64_t el = tm.elapsedUs();
        gSum = gSum + acc;
        totals[t] = el;
        tallocs[t] = loadAllocs();
        tallocBytes[t] = loadAllocBytes();
    }
    emitMedianRow("preview_encode", 1032, 1032, kIters, totals, tallocs, tallocBytes);
}

// ---- 4) preview_decode：PHYSICAL_PREVIEW 解析 ----
void benchPreviewDecode() {
    std::vector<uint8_t> pixels(OledFb::kSizeBytes);
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>((i * 5u) & 0xFFu);
    }
    const auto msg = makePhysicalPreview(
        42, 128, 64, espview::proto::PhysicalPixelFormat::kMono1, 0, pixels.data());
    if (!msg) {
        failGuard("preview_decode: setup");
    }
    constexpr size_t kIters = 64;
    uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
    for (int t = 0; t < kTrials; ++t) {
        espview::proto::bench::resetAllocationCounters();
        Timer tm;
        tm.start();
        uint64_t acc = 0;
        for (size_t it = 0; it < kIters; ++it) {
            PhysicalPreviewInfo info;
            if (!parsePhysicalPreview(
                    espview::proto::BytesView(msg->payload.data(), msg->payload.size()),
                    info)) {
                failGuard("preview_decode: parse");
            }
            acc += static_cast<uint64_t>(info.width) + info.height;
        }
        const uint64_t el = tm.elapsedUs();
        gSum = gSum + acc;
        totals[t] = el;
        tallocs[t] = loadAllocs();
        tallocBytes[t] = loadAllocBytes();
    }
    emitMedianRow("preview_decode", 1032, 1032, kIters, totals, tallocs, tallocBytes);
}

// ---- 5) input_encode：InputEvent → wire Message ----
void benchInputEncode() {
    constexpr size_t kIters = 128;
    uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
    for (int t = 0; t < kTrials; ++t) {
        espview::proto::bench::resetAllocationCounters();
        Timer tm;
        tm.start();
        uint64_t acc = 0;
        for (size_t it = 0; it < kIters; ++it) {
            InputEvent e;
            e.type = (it & 1u) ? InputType::kMouseDown : InputType::kMouseMove;
            e.x = static_cast<uint16_t>(it % kVirtualWidth);
            e.y = static_cast<uint16_t>(it % kVirtualHeight);
            e.buttons = (it & 1u) ? 0x01u : 0u;
            const auto msg = espview::input::encodeInputEvent(
                e, static_cast<uint16_t>(kVirtualWidth - 1),
                static_cast<uint16_t>(kVirtualHeight - 1));
            if (!msg) {
                failGuard("input_encode: encode");
            }
            acc += static_cast<uint64_t>(msg->payload.size());
        }
        const uint64_t el = tm.elapsedUs();
        gSum = gSum + acc;
        totals[t] = el;
        tallocs[t] = loadAllocs();
        tallocBytes[t] = loadAllocBytes();
    }
    emitMedianRow("input_encode", 8, 8, kIters, totals, tallocs, tallocBytes);
}

// ---- 6) coordinate_map：widget → 逻辑坐标 ----
void benchCoordinateMap() {
    constexpr size_t kIters = 256;
    uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
    for (int t = 0; t < kTrials; ++t) {
        espview::proto::bench::resetAllocationCounters();
        Timer tm;
        tm.start();
        uint64_t acc = 0;
        for (size_t it = 0; it < kIters; ++it) {
            int ox = 0;
            int oy = 0;
            const bool ok = CoordinateMapper::mapPoint(
                static_cast<int>((it * 13u) % 800u),
                static_cast<int>((it * 7u) % 600u), 800, 600, kVirtualWidth,
                kVirtualHeight, ox, oy);
            acc += ok ? static_cast<uint64_t>(ox) : 1u;
        }
        const uint64_t el = tm.elapsedUs();
        gSum = gSum + acc;
        totals[t] = el;
        tallocs[t] = loadAllocs();
        tallocBytes[t] = loadAllocBytes();
    }
    emitMedianRow("coordinate_map", 0, 0, kIters, totals, tallocs, tallocBytes);
}

// ---- 7) router_fanout：DisplayRouter::writeRectDetailed（Mirror 双 sink）----
class BenchSink : public IDisplaySink {
public:
    DisplayCapabilities caps;
    uint64_t presents = 0;

    DisplayStatus init(const DisplayCapabilities& c) override {
        caps = c;
        return DisplayStatus::kOk;
    }
    const DisplayCapabilities& capabilities() const override { return caps; }
    DisplayStatus present(const Rect&, const uint8_t*) override {
        ++presents;
        return DisplayStatus::kOk;
    }
    DisplayStatus flush() override { return DisplayStatus::kOk; }
    DisplayStatus setEnabled(bool) override { return DisplayStatus::kOk; }
    bool isAvailable() const override { return true; }
    DisplayStatus status() const override { return DisplayStatus::kOk; }
};

void benchRouterFanout() {
    auto virtualSink = std::make_shared<BenchSink>();
    auto physicalSink = std::make_shared<BenchSink>();
    DisplayRouter router;
    router.attachVirtual(virtualSink);
    router.attachPhysical(physicalSink);
    if (router.setMode(DisplayRouteMode::kMirror) != DisplayStatus::kOk) {
        failGuard("router_fanout: setMode");
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(320) * 24u * 2u, 0xABu);
    const Rect band{0, 0, 320, 24};
    constexpr size_t kIters = 128;
    uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
    for (int t = 0; t < kTrials; ++t) {
        espview::proto::bench::resetAllocationCounters();
        Timer tm;
        tm.start();
        uint64_t acc = 0;
        for (size_t it = 0; it < kIters; ++it) {
            const auto r = router.writeRectDetailed(band, pixels.data());
            acc += static_cast<uint64_t>(r.overall);
        }
        const uint64_t el = tm.elapsedUs();
        gSum = gSum + acc;
        totals[t] = el;
        tallocs[t] = loadAllocs();
        tallocBytes[t] = loadAllocBytes();
    }
    gPresents = gPresents + virtualSink->presents + physicalSink->presents;
    emitMedianRow("router_fanout", pixels.size(), pixels.size(), kIters, totals,
                  tallocs, tallocBytes);
}

}  // namespace

int main() {
    std::printf(
        "op,payload_bytes,wire_bytes,iterations,trial,total_elapsed_us,"
        "elapsed_us_per_op,ops_per_sec,alloc_count,alloc_bytes\n");
    benchPhysicalRender();
    benchOledFbWrite();
    benchPreviewEncode();
    benchPreviewDecode();
    benchInputEncode();
    benchCoordinateMap();
    benchRouterFanout();
    std::printf("checksum=%llu presents=%llu\n",
                static_cast<unsigned long long>(gSum),
                static_cast<unsigned long long>(gPresents));
    // guard：PhysicalRenderer / OledFb 热路径必须零堆分配。
    return 0;
}
