// ESPView M8-A3 — BoundedQueue / BoundedByteBuffer Host Tests。
//
// 覆盖 §三十五.9 queue ownership：drop-newest / latest-wins / takeDropped /
// takeAll / 溢出计数；10k / 100k 确定性压力（§32：禁止 sleep random）。
// 纯 C++17，零平台依赖。

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../bounded_queue.h"
#include "test_util.h"

namespace {

using espview::transport::BoundedByteBuffer;
using espview::transport::BoundedQueue;

}  // namespace

void runBoundedQueueTests() {
    std::printf("[bounded_queue]\n");

    // ---- 1. drop-newest：满时拒绝新元素并计数 ----
    {
        BoundedQueue<int> q(3);
        CHECK(q.push(1, false));
        CHECK(q.push(2, false));
        CHECK(q.push(3, false));
        CHECK(!q.push(4, false));  // 满：拒绝（丢新）
        CHECK_EQ(q.size(), 3u);
        CHECK_EQ(q.dropped(), 1u);
        int v = 0;
        CHECK(q.pop(v));
        CHECK_EQ(v, 1);
        CHECK(q.pop(v));
        CHECK_EQ(v, 2);
        CHECK(q.pop(v));
        CHECK_EQ(v, 3);
        CHECK(!q.pop(v));
        CHECK_EQ(q.takeDropped(), 1u);
        CHECK_EQ(q.dropped(), 0u);  // 取出后清零
    }

    // ---- 2. latest-wins：满时丢最旧，保留最新 ----
    {
        BoundedQueue<int> q(3);
        CHECK(q.push(1, true));
        CHECK(q.push(2, true));
        CHECK(q.push(3, true));
        CHECK(q.push(4, true));  // 丢 1
        CHECK(q.push(5, true));  // 丢 2
        CHECK_EQ(q.size(), 3u);
        CHECK_EQ(q.dropped(), 2u);
        int v = 0;
        CHECK(q.pop(v));
        CHECK_EQ(v, 3);
        CHECK(q.pop(v));
        CHECK_EQ(v, 4);
        CHECK(q.pop(v));
        CHECK_EQ(v, 5);
        CHECK_EQ(q.takeDropped(), 2u);
    }

    // ---- 3. takeAll：FIFO 序整体取出 ----
    {
        BoundedQueue<std::string> q(4);
        q.push(std::string("a"), false);
        q.push(std::string("b"), false);
        const std::vector<std::string> all = q.takeAll();
        CHECK_EQ(all.size(), 2u);
        CHECK(all[0] == "a");
        CHECK(all[1] == "b");
        CHECK(q.empty());
    }

    // ---- 4. BoundedByteBuffer：drop-newest 丢弃新字节的溢出部分 ----
    {
        BoundedByteBuffer bb(4);
        const uint8_t a[] = {1, 2};
        const uint8_t b[] = {3, 4, 5, 6};
        bb.append(a, 2);
        bb.append(b, 4);  // 溢出 2 字节（丢弃本次 append 的尾部新字节）
        CHECK_EQ(bb.size(), 4u);
        CHECK_EQ(bb.dropped(), 2u);
        const std::vector<uint8_t> out = bb.takeAll();
        CHECK_EQ(out.size(), 4u);
        CHECK_EQ(out[0], 1u);
        CHECK_EQ(out[1], 2u);
        CHECK_EQ(out[2], 3u);
        CHECK_EQ(out[3], 4u);
        CHECK_EQ(bb.takeDropped(), 2u);
        CHECK(bb.empty());
    }

    // ---- 5. 10k stress：drop-newest 保留前 256 个，FIFO 序不变 ----
    {
        BoundedQueue<int> q(256);
        for (int i = 0; i < 10000; ++i) {
            q.push(i, false);
        }
        CHECK_EQ(q.size(), 256u);
        CHECK_EQ(q.dropped(), 10000u - 256u);
        int expected = 0;
        int v = 0;
        while (q.pop(v)) {
            CHECK_EQ(v, expected);
            ++expected;
        }
        CHECK_EQ(expected, 256);
    }

    // ---- 6. 100k stress：latest-wins 保留最后 512 个，FIFO 序不变 ----
    {
        BoundedQueue<uint64_t> q(512);
        for (uint64_t i = 0; i < 100000u; ++i) {
            q.push(i, true);
        }
        CHECK_EQ(q.size(), 512u);
        CHECK_EQ(q.dropped(), 100000u - 512u);
        uint64_t expected = 100000u - 512u;
        uint64_t v = 0;
        while (q.pop(v)) {
            CHECK_EQ(v, expected);
            ++expected;
        }
        CHECK_EQ(expected, 100000u);
    }

    // ---- 7. 100k BoundedByteBuffer stress：只保留前 4096 字节 ----
    {
        BoundedByteBuffer bb(4096);
        std::vector<uint8_t> chunk(37, 0xAB);
        for (int i = 0; i < 10000; ++i) {
            bb.append(chunk.data(), chunk.size());
        }
        CHECK_EQ(bb.size(), 4096u);
        CHECK_EQ(bb.dropped(), 37u * 10000u - 4096u);
        const std::vector<uint8_t> out = bb.takeAll();
        CHECK_EQ(out.size(), 4096u);
        CHECK_EQ(out[0], 0xABu);
        CHECK_EQ(out.back(), 0xABu);
    }

    // ---- 8. capacity=0：拒绝一切 push（避免空 deque pop_front UB），计数 ----
    {
        BoundedQueue<int> q0(0);
        CHECK(!q0.push(1, true));
        CHECK(!q0.push(2, false));
        CHECK_EQ(q0.dropped(), 2u);
        CHECK(q0.empty());
        BoundedByteBuffer bb0(0);
        const uint8_t ab[] = {0x01, 0x02};
        bb0.append(ab, 2);
        CHECK_EQ(bb0.size(), 0u);
        CHECK_EQ(bb0.dropped(), 2u);
    }

    // ---- 9. drop 回调：latest-wins 驱逐前先回调（资源清理钩子，如密码清零）----
    {
        BoundedQueue<std::string> q(2);
        int drops = 0;
        q.push(std::string("a"), true, [&drops](std::string& s) { ++drops; s.clear(); });
        q.push(std::string("b"), true, [&drops](std::string& s) { ++drops; s.clear(); });
        CHECK(q.push(std::string("c"), true, [&drops](std::string& s) { ++drops; s.clear(); }));
        CHECK_EQ(drops, 1);  // 驱逐队头 "a"
        CHECK_EQ(q.dropped(), 1u);
        std::string v;
        CHECK(q.pop(v));
        CHECK(v == "b");
        CHECK(q.pop(v));
        CHECK(v == "c");
        CHECK_EQ(q.size(), 0u);
    }

    std::printf("[bounded_queue] done\n");
}
