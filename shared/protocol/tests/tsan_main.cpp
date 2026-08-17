// ESPView M8-A6 — TSan core-concurrency subset main.
//
// 独立 main：只链接并发/生命周期测试（endpoint_race / ack_concurrency /
// endpoint_lifecycle / deferred_control / lifecycle failure injection），
// 供 Linux TSan CI（-fsanitize=thread）运行（任务书 §十三/§四十二/§四十三）。
// 刻意不链接 counting_allocator.cpp：TSan 需要标准 malloc 拦截才能正确跟踪
// shadow memory；subset 不依赖分配计数断言。
// 纯 C++17。

#include <cstdio>

#include "test_util.h"

void runEndpointRaceTests();
void runAckConcurrencyTests();
void runEndpointLifecycleTests();
void runDeferredControlTests();
void runLifecycleTests();

int main() {
    // 无缓冲：测试名实时输出，便于定位挂起/超时位置（与 test_main.cpp 一致）。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("== ESPView TSan core concurrency subset ==\n");
    std::printf("[endpoint_race]\n");
    runEndpointRaceTests();
    std::printf("[ack_concurrency]\n");
    runAckConcurrencyTests();
    std::printf("[endpoint_lifecycle]\n");
    runEndpointLifecycleTests();
    std::printf("[deferred_control]\n");
    runDeferredControlTests();
    std::printf("[lifecycle_failure_injection]\n");
    runLifecycleTests();
    std::printf("----\nchecks: %d, failures: %d\n",
                espview::proto::test::gChecks.load(),
                espview::proto::test::gFailures.load());
    return espview::proto::test::gFailures.load() == 0 ? 0 : 1;
}
