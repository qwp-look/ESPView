// ESPView — 测试专用 InMemoryBytePipe + 分块喂入工具（M1-3A）
//
// 仅供 host-side 测试使用，不是正式 Transport：
//   - InMemoryBytePipe：保存发送端写入的字节流，接收端以任意 chunk 大小读取；
//   - feedChunks：固定 chunk 尺寸序列（1 / 2 / 7 / 非对齐边界等）；
//   - feedRandomChunks：固定种子随机分块（可复现）；
//   - feedWholePackets：按完整 Packet（或每 N 个包）喂入（粘包/整包场景）。
// 禁止把本文件引入生产组件（esp32 / pc 端正式代码）。
// 纯 C++17，零平台依赖。

#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "decoder.h"
#include "packet.h"

namespace espview {
namespace proto {
namespace test {

// 字节管道：只保存字节流，不消费、不理解协议。
class InMemoryBytePipe {
public:
    void write(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
    void write(const std::vector<uint8_t>& v) { write(v.data(), v.size()); }
    const std::vector<uint8_t>& bytes() const { return buf_; }
    size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

// 按固定 chunk 尺寸序列喂入（sizes 循环使用；sizes 为空 = 一次全量）。
inline void feedChunks(StreamDecoder& dec, const std::vector<uint8_t>& data,
                       const std::vector<size_t>& sizes) {
    if (sizes.empty()) {
        dec.feed(data.data(), data.size());
        return;
    }
    size_t pos = 0;
    size_t k = 0;
    while (pos < data.size()) {
        size_t n = sizes[k % sizes.size()];
        const size_t remain = data.size() - pos;
        if (n > remain) {
            n = remain;
        }
        dec.feed(data.data() + pos, n);
        pos += n;
        ++k;
    }
}

// 固定种子随机分块（chunk ∈ [minChunk, maxChunk]），结果可复现。
inline void feedRandomChunks(StreamDecoder& dec, const std::vector<uint8_t>& data,
                             uint32_t seed, size_t minChunk, size_t maxChunk) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> dist(minChunk, maxChunk);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t n = dist(rng);
        const size_t remain = data.size() - pos;
        if (n > remain) {
            n = remain;
        }
        dec.feed(data.data() + pos, n);
        pos += n;
    }
}

// 按完整 Packet 喂入：解析每包 20B 头得到总长，每 group 个完整包一次 feed。
// group=1：一个完整 Packet；group=2..N：多个完整 Packet（粘包）。
inline void feedWholePackets(StreamDecoder& dec, const std::vector<uint8_t>& data,
                             size_t group) {
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = pos;
        for (size_t g = 0; g < group && end < data.size(); ++g) {
            PacketHeader h;
            if (decodeHeader(data.data() + end, data.size() - end, &h) != PacketError::kNone) {
                break;  // 数据异常（测试数据应全为合法包）
            }
            end += kPacketHeaderSize + h.length;
        }
    dec.feed(data.data() + pos, end - pos);
    pos = end;
    }
}

}  // namespace test
}  // namespace proto
}  // namespace espview
