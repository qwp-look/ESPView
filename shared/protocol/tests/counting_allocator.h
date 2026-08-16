// ESPView — 测试专用全局分配计数器（M8-A1 Task 6/7）
//
// 仅链接进 espview_protocol_tests 可执行文件（绝不可链接进 libespview_protocol
// 或任何生产组件）。全局 operator new/new[]/delete/delete[]（含 nothrow 与
// sized-delete 变体）转发 malloc/free，同时累计分配次数/字节（std::atomic）。
// 测试以 DELTA 断言：二进制还链接 display/transport/oled 等 TU，绝对值不稳定。
// 纯 C++17。

#pragma once

#include <atomic>
#include <cstddef>

namespace espview {
namespace proto {
namespace test {

struct AllocationCounters {
    static std::atomic<size_t> allocations;    // operator new/new[] 调用次数
    static std::atomic<size_t> bytes;          // 请求的字节总数
    static std::atomic<size_t> deallocations;  // operator delete/delete[] 调用次数
};

// 清零计数器（测试前后调用；relaxed 语义足够——单线程断言 DELTA）。
void resetAllocationCounters();

}  // namespace test
}  // namespace proto
}  // namespace espview
