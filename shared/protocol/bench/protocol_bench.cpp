// ESPView — shared/protocol 基准（M8-A1 Task 7）
//
// 独立可执行（自带 main；非测试框架；guard 失败 → 非零退出）。
// 六项基准 × 载荷 {1024, 4096, 65536, 153600, 1048576}，固定迭代次数
// （编译期表；1 MiB 用 16 次，其余 64 次），5 trials，中位数。
// 固定种子 std::mt19937(0x5EED) 载荷在计时区外构建；批量循环夹在两个
// steady_clock::now() 之间；I/O（printf）全部在计时区外。
// 每项操作含正确性 guard（decode/CRC/checksum、onMessage 恰好一次、
// frame commit == 1），累加进 volatile checksum 并在退出时打印，
// 防止优化器消除。
// CSV 列：op,payload_bytes,wire_bytes,packets,iterations,trial,
//         total_elapsed_us,elapsed_us_per_op,bytes_per_sec,alloc_count,alloc_bytes
// 纯 C++17。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "counting_allocator.h"
#include "decoder.h"
#include "encoder.h"
#include "frame_assembler.h"
#include "message.h"
#include "packet.h"
#include "protocol.h"

namespace {

using espview::proto::DecoderError;
using espview::proto::FrameAssembler;
using espview::proto::FrameType;
using espview::proto::IMessagePayloadSource;
using espview::proto::kMaxPacketPayload;
using espview::proto::kPacketHeaderSize;
using espview::proto::CommittedFrame;
using espview::proto::FrameBeginInfo;
using espview::proto::FrameDiscardReason;
using espview::proto::makeFrameBegin;
using espview::proto::makeFrameEnd;
using espview::proto::makeFrameRect;
using espview::proto::makeHeader;
using espview::proto::RectInfo;
using espview::proto::Message;
using espview::proto::MessageEncoder;
using espview::proto::MessageHeader;
using espview::proto::MessageType;
using espview::proto::PacketError;
using espview::proto::PacketHeader;
using espview::proto::PixelFormat;
using espview::proto::SequenceCounter;
using espview::proto::StreamDecoder;

constexpr uint64_t kSeed = 0x5EED;
constexpr int kTrials = 5;

// ---- guard 累加（volatile 防优化器消除；退出时打印）----
volatile uint64_t gSum = 0;         // 输出字节/载荷校验和
volatile uint64_t gDispatches = 0;  // decoder onMessage 次数
volatile uint64_t gCommits = 0;     // frame commit 次数
volatile uint64_t gDiscards = 0;    // frame discard 次数
volatile uint64_t gErrors = 0;      // decoder 错误次数
volatile uint64_t gLastError = 0;   // 最近一次 decoder 错误码

void failGuard(const char* what) {
    std::fprintf(stderr, "GUARD FAIL: %s\n", what);
    std::exit(1);
}

void sumAccum(uint64_t v) { gSum = gSum + v; }
void bumpDispatch() { gDispatches = gDispatches + 1; }
void bumpCommit() { gCommits = gCommits + 1; }
void bumpDiscard() { gDiscards = gDiscards + 1; }
void bumpError() { gErrors = gErrors + 1; }

void addToSum(const uint8_t* d, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; ++i) {
        s += static_cast<uint8_t>(d[i]);
    }
    sumAccum(s);
}

// ---- 基准用例 ----
struct BenchCase {
    size_t payloadBytes = 0;
    size_t iterations = 0;
    size_t packets = 0;   // ceil(payload / 4096)
    size_t wireBytes = 0; // packets*20 + payload
    std::vector<uint8_t> data;
};

// quick=true（--quick，CI smoke）：小载荷 + 少迭代，仍覆盖单包与多包；
// 默认完整表 = M8-A1 基线格式（任务书 §十七 Fast CI 只 smoke）。
std::vector<BenchCase> makeCases(bool quick) {
    const std::pair<size_t, size_t> quickTable[] = {{1024, 8}, {65536, 4}};
    const std::pair<size_t, size_t> fullTable[] = {
        {1024, 64}, {4096, 64}, {65536, 64}, {153600, 64}, {1048576, 16}};
    const std::pair<size_t, size_t>* table = quick ? quickTable : fullTable;
    const size_t tableCount = quick
        ? sizeof(quickTable) / sizeof(quickTable[0])
        : sizeof(fullTable) / sizeof(fullTable[0]);
    std::vector<BenchCase> out;
    for (size_t i = 0; i < tableCount; ++i) {
        const auto& kv = table[i];
        BenchCase c;
        c.payloadBytes = kv.first;
        c.iterations = kv.second;
        c.packets = (c.payloadBytes + kMaxPacketPayload - 1) / kMaxPacketPayload;
        c.wireBytes = c.packets * kPacketHeaderSize + c.payloadBytes;
        std::mt19937 rng(kSeed);
        c.data.resize(c.payloadBytes);
        for (auto& b : c.data) {
            b = static_cast<uint8_t>(rng() & 0xFFu);
        }
        out.push_back(std::move(c));
    }
    return out;
}

// ---- 计时 / 中位数 / CSV ----
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

void emitRow(const char* op, const BenchCase& c, int trial, uint64_t totalUs,
             uint64_t allocs, uint64_t allocBytes) {
    const double perOp =
        static_cast<double>(totalUs) / static_cast<double>(c.iterations);
    const double bps = perOp > 0.0
                           ? (static_cast<double>(c.wireBytes) * 1e6) / perOp
                           : 0.0;
    std::printf("%s,%zu,%zu,%zu,%zu,%d,%llu,%.3f,%.0f,%llu,%llu\n", op, c.payloadBytes,
                c.wireBytes, c.packets, c.iterations, trial,
                static_cast<unsigned long long>(totalUs), perOp, bps,
                static_cast<unsigned long long>(allocs),
                static_cast<unsigned long long>(allocBytes));
}

// trial=5 行 = 5 次 trial 的中位数汇总（0..4 为原始 trial）。
void emitMedianRow(const char* op, const BenchCase& c, const uint64_t totals[5],
                   const uint64_t allocs[5], const uint64_t allocBytes[5]) {
    emitRow(op, c, 5, medianOf(totals, 5), medianOf(allocs, 5), medianOf(allocBytes, 5));
}

// ---- 分块（≤4096；末块较小）----
std::vector<size_t> chunkSizes(size_t total) {
    std::vector<size_t> v;
    size_t rem = total;
    while (rem > 0) {
        const size_t n = std::min<size_t>(kMaxPacketPayload, rem);
        v.push_back(n);
        rem -= n;
    }
    if (v.empty()) {
        v.push_back(0);
    }
    return v;
}

// ---- 1) packet_encode ----
void benchPacketEncode(const std::vector<BenchCase>& cases) {
    for (const auto& c : cases) {
        const std::vector<size_t> chunks = chunkSizes(c.payloadBytes);
        uint8_t buf[kPacketHeaderSize + kMaxPacketPayload];
        uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
        for (int t = 0; t < kTrials; ++t) {
            espview::proto::bench::resetAllocationCounters();
            Timer tm;
            tm.start();
            uint64_t guard = 0;
            for (size_t it = 0; it < c.iterations; ++it) {
                for (size_t p = 0; p < chunks.size(); ++p) {
                    const PacketHeader h = makeHeader(
                        static_cast<uint8_t>(MessageType::kFrameRect), 0,
                        static_cast<uint16_t>(p & 0xFFFFu),
                        static_cast<uint32_t>(chunks[p]));
                    size_t written = 0;
                    const PacketError err = encodePacket(
                        h, c.data.data() + p * kMaxPacketPayload, chunks[p], buf,
                        sizeof(buf), &written);
                    if (err != PacketError::kNone) {
                        failGuard("packet_encode: encodePacket");
                    }
                    guard += written;
                    guard += static_cast<uint8_t>(buf[0] + buf[written - 1]);
                }
            }
            const uint64_t totalUs = tm.elapsedUs();
            const uint64_t allocs = loadAllocs();
            const uint64_t allocBytes = loadAllocBytes();
            totals[t] = totalUs;
            tallocs[t] = allocs;
            tallocBytes[t] = allocBytes;
            sumAccum(guard);
            emitRow("packet_encode", c, t, totalUs, allocs, allocBytes);
        }
        emitMedianRow("packet_encode", c, totals, tallocs, tallocBytes);
        // 正确性（计时区外）：完整 decode + CRC 校验 + payload 校验和。
        {
            uint8_t vbuf[kPacketHeaderSize + kMaxPacketPayload];
            for (size_t p = 0; p < chunks.size(); ++p) {
                const PacketHeader h = makeHeader(
                    static_cast<uint8_t>(MessageType::kFrameRect), 0,
                    static_cast<uint16_t>(p & 0xFFFFu),
                    static_cast<uint32_t>(chunks[p]));
                size_t written = 0;
                if (encodePacket(h, c.data.data() + p * kMaxPacketPayload, chunks[p],
                                 vbuf, sizeof(vbuf), &written) != PacketError::kNone) {
                    failGuard("packet_encode verify: encode");
                }
                PacketHeader h2;
                if (decodeHeader(vbuf, written, &h2) != PacketError::kNone) {
                    failGuard("packet_encode verify: decode");
                }
                if (verifyPacketCrc(h2, vbuf + kPacketHeaderSize, chunks[p]) !=
                    PacketError::kNone) {
                    failGuard("packet_encode verify: crc");
                }
                addToSum(vbuf + kPacketHeaderSize, chunks[p]);
            }
        }
    }
}

// ---- 2) packet_decode ----
void benchPacketDecode(const std::vector<BenchCase>& cases) {
    for (const auto& c : cases) {
        const std::vector<size_t> chunks = chunkSizes(c.payloadBytes);
        std::vector<std::vector<uint8_t>> wire;  // 预编码（计时区外）
        for (size_t p = 0; p < chunks.size(); ++p) {
            const PacketHeader h = makeHeader(
                static_cast<uint8_t>(MessageType::kFrameRect), 0,
                static_cast<uint16_t>(p & 0xFFFFu), static_cast<uint32_t>(chunks[p]));
            std::vector<uint8_t> pkt(kPacketHeaderSize + chunks[p]);
            size_t written = 0;
            if (encodePacket(h, c.data.data() + p * kMaxPacketPayload, chunks[p],
                             pkt.data(), pkt.size(), &written) != PacketError::kNone) {
                failGuard("packet_decode setup: encode");
            }
            pkt.resize(written);
            wire.push_back(std::move(pkt));
        }
        uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
        for (int t = 0; t < kTrials; ++t) {
            espview::proto::bench::resetAllocationCounters();
            Timer tm;
            tm.start();
            uint64_t guard = 0;
            for (size_t it = 0; it < c.iterations; ++it) {
                for (const auto& pkt : wire) {
                    PacketHeader h;
                    if (decodeHeader(pkt.data(), pkt.size(), &h) != PacketError::kNone) {
                        failGuard("packet_decode: decodeHeader");
                    }
                    if (verifyPacketCrc(h, pkt.data() + kPacketHeaderSize,
                                        pkt.size() - kPacketHeaderSize) !=
                        PacketError::kNone) {
                        failGuard("packet_decode: crc");
                    }
                    guard += h.length;
                }
            }
            const uint64_t totalUs = tm.elapsedUs();
            totals[t] = totalUs;
            tallocs[t] = loadAllocs();
            tallocBytes[t] = loadAllocBytes();
            sumAccum(guard);
            emitRow("packet_decode", c, t, totalUs, tallocs[t], tallocBytes[t]);
        }
        emitMedianRow("packet_decode", c, totals, tallocs, tallocBytes);
    }
}// ---- 3) message_encode ----
void benchMessageEncode(const std::vector<BenchCase>& cases) {
    for (const auto& c : cases) {
        Message msg;
        msg.type = static_cast<uint8_t>(MessageType::kFrameRect);
        msg.flags = 0;
        msg.payload = c.data;  // 计时区外复制（1 MiB 用例）
        uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
        for (int t = 0; t < kTrials; ++t) {
            SequenceCounter seq;
            MessageEncoder enc(seq);
            std::vector<std::vector<uint8_t>> out;
            out.reserve(c.packets);
            espview::proto::bench::resetAllocationCounters();
            Timer tm;
            tm.start();
            uint64_t guard = 0;
            for (size_t it = 0; it < c.iterations; ++it) {
                out.clear();
                if (enc.encode(msg, out) != PacketError::kNone) {
                    failGuard("message_encode: encode");
                }
                guard += out.size();
                guard += static_cast<uint8_t>(out[0][0]);
            }
            const uint64_t totalUs = tm.elapsedUs();
            const uint64_t allocs = loadAllocs();
            const uint64_t allocBytes = loadAllocBytes();
            totals[t] = totalUs;
            tallocs[t] = allocs;
            tallocBytes[t] = allocBytes;
            sumAccum(guard);
            emitRow("message_encode", c, t, totalUs, allocs, allocBytes);
            // 正确性（计时区外）：最后一批包 decode + CRC + 载荷校验和。
            size_t total = 0;
            for (const auto& pkt : out) {
                PacketHeader h;
                if (decodeHeader(pkt.data(), pkt.size(), &h) != PacketError::kNone) {
                    failGuard("message_encode verify: decode");
                }
                if (verifyPacketCrc(h, pkt.data() + kPacketHeaderSize,
                                    pkt.size() - kPacketHeaderSize) !=
                    PacketError::kNone) {
                    failGuard("message_encode verify: crc");
                }
                addToSum(pkt.data() + kPacketHeaderSize, pkt.size() - kPacketHeaderSize);
                total += pkt.size() - kPacketHeaderSize;
            }
            if (total != c.payloadBytes) {
                failGuard("message_encode verify: payload length");
            }
        }
        emitMedianRow("message_encode", c, totals, tallocs, tallocBytes);
    }
}

// ---- 4) stream_encode ----
struct StreamSource : public IMessagePayloadSource {
    const uint8_t* data = nullptr;
    size_t len = 0;
    size_t off = 0;
    size_t read(uint8_t* dst, size_t maxBytes) override {
        const size_t n = std::min(maxBytes, len - off);
        if (n > 0) {
            std::memcpy(dst, data + off, n);
            off += n;
        }
        return n;
    }
};

// 捕获空的 sink：原始函数指针 → std::function SBO（无堆分配），并累加 gSum
// 防优化器消除。≤1 指针捕获约束满足。
bool streamSink(const uint8_t* d, size_t n) {
    addToSum(d, n);
    return true;
}

void benchStreamEncode(const std::vector<BenchCase>& cases) {
    for (const auto& c : cases) {
        const MessageHeader hdr{static_cast<uint8_t>(MessageType::kFrameRect), 0};
        uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
        for (int t = 0; t < kTrials; ++t) {
            SequenceCounter seq;
            MessageEncoder enc(seq);
            StreamSource src;
            src.data = c.data.data();
            src.len = c.data.size();
            src.off = 0;
            espview::proto::bench::resetAllocationCounters();
            Timer tm;
            tm.start();
            uint64_t guard = 0;
            for (size_t it = 0; it < c.iterations; ++it) {
                src.off = 0;
                const PacketError err = enc.encodeStreaming(hdr, src, streamSink);
                if (err != PacketError::kNone) {
                    failGuard("stream_encode: encodeStreaming");
                }
                guard += src.off;  // 源进度 = 已编码载荷字节（防 elision）
            }
            const uint64_t totalUs = tm.elapsedUs();
            const uint64_t allocs = loadAllocs();
            const uint64_t allocBytes = loadAllocBytes();
            totals[t] = totalUs;
            tallocs[t] = allocs;
            tallocBytes[t] = allocBytes;
            sumAccum(guard);
            emitRow("stream_encode", c, t, totalUs, allocs, allocBytes);
            // 正确性（计时区外）：一次完整 encodeStreaming → 收集 + decode/CRC +
            // 载荷重构长度校验。
            {
                StreamSource vsrc;
                vsrc.data = c.data.data();
                vsrc.len = c.data.size();
                vsrc.off = 0;
                std::vector<std::vector<uint8_t>> out;
                const PacketError err = enc.encodeStreaming(
                    hdr, vsrc, [&out](const uint8_t* d, size_t n) {
                        out.emplace_back(d, d + n);
                        return true;
                    });
                if (err != PacketError::kNone) {
                    failGuard("stream_encode verify: encode");
                }
                size_t total = 0;
                for (const auto& pkt : out) {
                    PacketHeader h;
                    if (decodeHeader(pkt.data(), pkt.size(), &h) != PacketError::kNone) {
                        failGuard("stream_encode verify: decode");
                    }
                    if (verifyPacketCrc(h, pkt.data() + kPacketHeaderSize,
                                        pkt.size() - kPacketHeaderSize) !=
                        PacketError::kNone) {
                        failGuard("stream_encode verify: crc");
                    }
                    total += pkt.size() - kPacketHeaderSize;
                }
                if (total != c.payloadBytes) {
                    failGuard("stream_encode verify: payload length");
                }
            }
        }
        emitMedianRow("stream_encode", c, totals, tallocs, tallocBytes);
    }
}

// ---- 5) decoder_feed ----
void decoderMsgFn(const Message& m) {
    bumpDispatch();
    addToSum(m.payload.data(), m.payload.size());
}
void decoderErrFn(DecoderError e) {
    gLastError = static_cast<uint64_t>(e);
    bumpError();
}

void benchDecoderFeed(const std::vector<BenchCase>& cases) {
    for (const auto& c : cases) {
        // 预编码 wire（计时区外）：整个载荷 = 一条消息。
        Message msg;
        msg.type = static_cast<uint8_t>(MessageType::kFrameRect);
        msg.flags = 0;
        msg.payload = c.data;
        SequenceCounter seq;
        MessageEncoder enc(seq);
        std::vector<std::vector<uint8_t>> pkts;
        if (enc.encode(msg, pkts) != PacketError::kNone) {
            failGuard("decoder_feed setup: encode");
        }
        std::vector<uint8_t> wire;
        for (const auto& pkt : pkts) {
            wire.insert(wire.end(), pkt.begin(), pkt.end());
        }
        uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
        for (int t = 0; t < kTrials; ++t) {
            espview::proto::bench::resetAllocationCounters();
            Timer tm;
            tm.start();
            uint64_t guard = 0;
            for (size_t it = 0; it < c.iterations; ++it) {
                // 每次 feed 都是独立消息：decoder 按迭代重建，expectedSeq 从 0 起。
                StreamDecoder dec(decoderMsgFn, nullptr, decoderErrFn);
                const uint64_t before = gDispatches;
                dec.feed(wire.data(), wire.size());
                if (gDispatches - before != 1u) {
                    failGuard("decoder_feed: onMessage != 1");
                }
                if (gErrors != 0u) {
                    failGuard("decoder_feed: decoder error");
                }
                guard += gDispatches;
            }
            const uint64_t totalUs = tm.elapsedUs();
            const uint64_t allocs = loadAllocs();
            const uint64_t allocBytes = loadAllocBytes();
            totals[t] = totalUs;
            tallocs[t] = allocs;
            tallocBytes[t] = allocBytes;
            sumAccum(guard);
            emitRow("decoder_feed", c, t, totalUs, allocs, allocBytes);
        }
        emitMedianRow("decoder_feed", c, totals, tallocs, tallocBytes);
    }
}// ---- 6) frame_assembly ----
uint16_t geomW(size_t payloadBytes) {
    switch (payloadBytes) {
        case 1024:
            return 32;
        case 4096:
            return 64;
        case 65536:
            return 256;
        case 153600:
            return 320;
        default:  // 1048576
            return 1024;
    }
}
uint16_t geomH(size_t payloadBytes) {
    switch (payloadBytes) {
        case 1024:
            return 16;
        case 4096:
            return 32;
        case 65536:
            return 128;
        case 153600:
            return 240;
        default:  // 1048576
            return 512;
    }
}

void benchFrameAssembly(const std::vector<BenchCase>& cases) {
    for (const auto& c : cases) {
        const uint16_t w = geomW(c.payloadBytes);
        const uint16_t h = geomH(c.payloadBytes);
        const uint32_t pixelBytes = static_cast<uint32_t>(c.payloadBytes);
        // 单条 FRAME_RECT 的 message payload = 8B rect 头 + 像素；1 MiB 载荷
        // （8 + 1048576 > kMaxMessagePayload）无法放入单条 rect，拆成两条
        // 512×512（各 524288 px = 524296 B <= 1 MiB），帧像素总量仍 1048576。
        const bool split = (c.payloadBytes == 1048576u);
        const uint16_t rw = split ? 512u : w;
        const uint16_t rh = split ? 512u : h;
        const uint16_t rectCount = split ? 2u : 1u;
        const size_t rectPixelBytes = static_cast<size_t>(rw) * rh * 2u;
        const auto begin =
            makeFrameBegin(1, FrameType::kFull, PixelFormat::kRgb565, w, h, pixelBytes);
        const auto rect1 = makeFrameRect(0, 0, rw, rh, c.data.data(), rectPixelBytes);
        std::optional<Message> rect2;
        if (split) {
            rect2 = makeFrameRect(rw, 0, rw, rh, c.data.data() + rectPixelBytes,
                                  rectPixelBytes);
        }
        const auto end = makeFrameEnd(1, rectCount, pixelBytes, false);
        if (!begin.has_value() || !rect1.has_value() ||
            (split && !rect2.has_value())) {
            failGuard("frame_assembly setup: builders");
        }
        // 预编码 wire（计时区外）。encode() 会在入口清空输出 vector，
        // 因此每条消息必须编码到独立 vector 再拼接。
        std::vector<uint8_t> wire;
        uint64_t wireBytes = 0;
        uint64_t packets = 0;
        {
            SequenceCounter seq;
            MessageEncoder enc(seq);
            std::vector<std::vector<uint8_t>> bp, r1p, r2p, ep;
            if (enc.encode(*begin, bp) != PacketError::kNone ||
                enc.encode(*rect1, r1p) != PacketError::kNone ||
                (split && enc.encode(*rect2, r2p) != PacketError::kNone) ||
                enc.encode(end, ep) != PacketError::kNone) {
                failGuard("frame_assembly setup: encode");
            }
            const std::vector<std::vector<uint8_t>>* all[] = {&bp, &r1p, &r2p, &ep};
            for (const auto* v : all) {
                for (const auto& pkt : *v) {
                    wire.insert(wire.end(), pkt.begin(), pkt.end());
                }
            }
            wireBytes = wire.size();
            packets = bp.size() + r1p.size() + r2p.size() + ep.size();
        }
        BenchCase ce = c;
        ce.wireBytes = wireBytes;
        ce.packets = packets;
        uint64_t totals[kTrials], tallocs[kTrials], tallocBytes[kTrials];
        for (int t = 0; t < kTrials; ++t) {
            espview::proto::bench::resetAllocationCounters();
            Timer tm;
            tm.start();
            uint64_t guard = 0;
            for (size_t it = 0; it < c.iterations; ++it) {
                // 每次 feed 都是独立完整帧：assembler+decoder 按迭代重建（seq 从 0 起）。
                FrameAssembler::Callbacks acb;
                acb.onBegin = [](const FrameBeginInfo&) {};
                acb.onRect = [](const RectInfo&, const uint8_t*, size_t) {};
                acb.onCommit = [](const CommittedFrame& f) {
                    bumpCommit();
                    sumAccum(f.byteCount);
                };
                acb.onDiscard = [](FrameDiscardReason) { bumpDiscard(); };
                FrameAssembler assembler(std::move(acb));
                StreamDecoder dec(
                    [&assembler](const Message& m) { assembler.onMessage(m); }, nullptr,
                    decoderErrFn);
                const uint64_t before = gCommits;
                dec.feed(wire.data(), wire.size());
                if (gErrors != 0u) {
                    std::fprintf(stderr, "frame_assembly debug: lastError=%llu\n",
                                 static_cast<unsigned long long>(gLastError));
                    failGuard("frame_assembly: decoder error");
                }
                if (gCommits - before != 1u) {
                    std::fprintf(stderr,
                                 "frame_assembly debug: payload=%zu it=%zu commits=%llu discards=%llu errors=%llu\n",
                                 c.payloadBytes, it,
                                 static_cast<unsigned long long>(gCommits - before),
                                 static_cast<unsigned long long>(gDiscards),
                                 static_cast<unsigned long long>(gErrors));
                    failGuard("frame_assembly: commit != 1");
                }
                if (gDiscards != 0u) {
                    failGuard("frame_assembly: unexpected discard");
                }
                guard += gCommits;
            }
            const uint64_t totalUs = tm.elapsedUs();
            const uint64_t allocs = loadAllocs();
            const uint64_t allocBytes = loadAllocBytes();
            totals[t] = totalUs;
            tallocs[t] = allocs;
            tallocBytes[t] = allocBytes;
            sumAccum(guard);
            emitRow("frame_assembly", ce, t, totalUs, allocs, allocBytes);
        }
        emitMedianRow("frame_assembly", ce, totals, tallocs, tallocBytes);
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<BenchCase> cases = makeCases(quick);
    std::printf(
        "op,payload_bytes,wire_bytes,packets,iterations,trial,total_elapsed_us,"
        "elapsed_us_per_op,bytes_per_sec,alloc_count,alloc_bytes\n");
    benchPacketEncode(cases);
    benchPacketDecode(cases);
    benchMessageEncode(cases);
    benchStreamEncode(cases);
    benchDecoderFeed(cases);
    benchFrameAssembly(cases);
    std::fprintf(stderr,
                 "guards: sum=%llu dispatches=%llu commits=%llu discards=%llu errors=%llu\n",
                 static_cast<unsigned long long>(gSum),
                 static_cast<unsigned long long>(gDispatches),
                 static_cast<unsigned long long>(gCommits),
                 static_cast<unsigned long long>(gDiscards),
                 static_cast<unsigned long long>(gErrors));
    return 0;
}
