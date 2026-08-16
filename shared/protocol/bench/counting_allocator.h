// ESPView — 基准专用全局分配计数器（M8-A1 Task 7）
//
// 仅链接进 espview_protocol_bench 可执行文件（绝不可链接进 libespview_protocol
// 或任何生产组件）。全局 operator new/new[]/delete/delete[]（含 nothrow 与
// sized-delete 变体）转发 malloc/free，同时累计分配次数/字节（std::atomic）。
// 基准以 DELTA 断言/报告：计时批次前后各读一次，差值即该批次的分配量。
// 纯 C++17。

#pragma once

#include <atomic>
#include <cstddef>

namespace espview {
namespace proto {
namespace bench {

struct AllocationCounters {
    static std::atomic<size_t> allocations;    // operator new/new[] 调用次数
    static std::atomic<size_t> bytes;          // 请求的字节总数
    static std::atomic<size_t> deallocations;  // operator delete/delete[] 调用次数
};

// 清零计数器（计时批次前调用；relaxed 语义足够——单线程 DELTA）。
void resetAllocationCounters();

}  // namespace bench
}  // namespace proto
}  // namespace espview
