#include "counting_allocator.h"

#include <cstdlib>
#include <new>

namespace espview {
namespace proto {
namespace test {

std::atomic<size_t> AllocationCounters::allocations{0};
std::atomic<size_t> AllocationCounters::bytes{0};
std::atomic<size_t> AllocationCounters::deallocations{0};

void resetAllocationCounters() {
    AllocationCounters::allocations.store(0, std::memory_order_relaxed);
    AllocationCounters::bytes.store(0, std::memory_order_relaxed);
    AllocationCounters::deallocations.store(0, std::memory_order_relaxed);
}

}  // namespace test
}  // namespace proto
}  // namespace espview

// ---- 全局替换（仅测试二进制；转发 malloc/free 保持行为等价）----
// M8-A1：编码热路径零堆分配断言依赖这些计数器。

void* operator new(std::size_t size) {
    espview::proto::test::AllocationCounters::allocations.fetch_add(1,
                                                                    std::memory_order_relaxed);
    espview::proto::test::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    espview::proto::test::AllocationCounters::allocations.fetch_add(1,
                                                                    std::memory_order_relaxed);
    espview::proto::test::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
    espview::proto::test::AllocationCounters::deallocations.fetch_add(1,
                                                                      std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p) noexcept {
    espview::proto::test::AllocationCounters::deallocations.fetch_add(1,
                                                                      std::memory_order_relaxed);
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    espview::proto::test::AllocationCounters::deallocations.fetch_add(1,
                                                                      std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    espview::proto::test::AllocationCounters::deallocations.fetch_add(1,
                                                                      std::memory_order_relaxed);
    std::free(p);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    espview::proto::test::AllocationCounters::allocations.fetch_add(1,
                                                                    std::memory_order_relaxed);
    espview::proto::test::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    espview::proto::test::AllocationCounters::allocations.fetch_add(1,
                                                                    std::memory_order_relaxed);
    espview::proto::test::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    espview::proto::test::AllocationCounters::deallocations.fetch_add(1,
                                                                      std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    espview::proto::test::AllocationCounters::deallocations.fetch_add(1,
                                                                      std::memory_order_relaxed);
    std::free(p);
}
