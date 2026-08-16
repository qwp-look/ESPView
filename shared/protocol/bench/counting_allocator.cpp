#include "counting_allocator.h"

#include <cstdlib>
#include <new>

namespace espview {
namespace proto {
namespace bench {

std::atomic<size_t> AllocationCounters::allocations{0};
std::atomic<size_t> AllocationCounters::bytes{0};
std::atomic<size_t> AllocationCounters::deallocations{0};

void resetAllocationCounters() {
    AllocationCounters::allocations.store(0, std::memory_order_relaxed);
    AllocationCounters::bytes.store(0, std::memory_order_relaxed);
    AllocationCounters::deallocations.store(0, std::memory_order_relaxed);
}

}  // namespace bench
}  // namespace proto
}  // namespace espview

// ---- 全局替换（仅基准二进制；转发 malloc/free 保持行为等价）----

void* operator new(std::size_t size) {
    espview::proto::bench::AllocationCounters::allocations.fetch_add(1,
                                                                     std::memory_order_relaxed);
    espview::proto::bench::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    espview::proto::bench::AllocationCounters::allocations.fetch_add(1,
                                                                     std::memory_order_relaxed);
    espview::proto::bench::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept {
    espview::proto::bench::AllocationCounters::deallocations.fetch_add(1,
                                                                       std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p) noexcept {
    espview::proto::bench::AllocationCounters::deallocations.fetch_add(1,
                                                                       std::memory_order_relaxed);
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    espview::proto::bench::AllocationCounters::deallocations.fetch_add(1,
                                                                       std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    espview::proto::bench::AllocationCounters::deallocations.fetch_add(1,
                                                                       std::memory_order_relaxed);
    std::free(p);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    espview::proto::bench::AllocationCounters::allocations.fetch_add(1,
                                                                     std::memory_order_relaxed);
    espview::proto::bench::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    espview::proto::bench::AllocationCounters::allocations.fetch_add(1,
                                                                     std::memory_order_relaxed);
    espview::proto::bench::AllocationCounters::bytes.fetch_add(size, std::memory_order_relaxed);
    return std::malloc(size ? size : 1);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    espview::proto::bench::AllocationCounters::deallocations.fetch_add(1,
                                                                       std::memory_order_relaxed);
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    espview::proto::bench::AllocationCounters::deallocations.fetch_add(1,
                                                                       std::memory_order_relaxed);
    std::free(p);
}
