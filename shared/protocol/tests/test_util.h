// 极简主机侧测试工具（无第三方依赖，C++17）。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>

namespace espview {
namespace proto {
namespace test {

// M8-A1：并发测试（endpoint_concurrency_test）的 CHECK 会在多个线程内执行，
// 计数器必须是原子的（否则 ++ 丢失更新 → 总 check 数不确定）。
inline std::atomic<int> gChecks = 0;
inline std::atomic<int> gFailures = 0;

inline void reportFailure(int line, const char* file, const std::string& expr,
                          const std::string& detail) {
    gFailures.fetch_add(1, std::memory_order_relaxed);
    std::printf("  FAIL %s:%d  %s", file, line, expr.c_str());
    if (!detail.empty()) {
        std::printf("   {%s}", detail.c_str());
    }
    std::printf("\n");
}

// 通用等值检查：按 long long 比较/打印，兼容整数与 scoped enum。
template <typename A, typename B>
void checkEq(int line, const char* file, const char* exprA, const char* exprB,
             const A& a, const B& b) {
    gChecks.fetch_add(1, std::memory_order_relaxed);
    const long long va = static_cast<long long>(a);
    const long long vb = static_cast<long long>(b);
    if (va != vb) {
        reportFailure(line, file, std::string(exprA) + " == " + exprB,
                      std::to_string(va) + " vs " + std::to_string(vb));
    }
}

}  // namespace test
}  // namespace proto
}  // namespace espview

#define CHECK(cond)                                                       \
    do {                                                                  \
        espview::proto::test::gChecks.fetch_add(1, std::memory_order_relaxed);                                  \
        if (!(cond)) {                                                    \
            espview::proto::test::reportFailure(__LINE__, __FILE__,       \
                                                std::string(#cond), "");  \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b) \
    espview::proto::test::checkEq(__LINE__, __FILE__, #a, #b, (a), (b))

#define CHECK_MSG(cond, msg)                                              \
    do {                                                                  \
        espview::proto::test::gChecks.fetch_add(1, std::memory_order_relaxed);                                  \
        if (!(cond)) {                                                    \
            espview::proto::test::reportFailure(__LINE__, __FILE__,       \
                                                std::string(#cond), msg); \
        }                                                                 \
    } while (0)
