cmake_minimum_required(VERSION 3.16)
project(espview_protocol LANGUAGES CXX)

# 纯 C++17 静态库，零平台依赖（ESP32 与 PC 共用）。
add_library(espview_protocol STATIC
    crc32.cpp
    packet.cpp
    message.cpp
    encoder.cpp
    decoder.cpp
    frame_assembler.cpp
    protocol_endpoint.cpp
    runtime_stats.cpp
)
target_include_directories(espview_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../transport)  # M8-A3：protocol_endpoint 引用 transport::SendStatus
target_compile_features(espview_protocol PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_protocol PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(ESPVIEW_BUILD_BENCH "Build host-side protocol benchmark" ON)
if(ESPVIEW_BUILD_BENCH)
    # M8-A1 Task 7：独立基准可执行（非测试；benchmark 不 add_test）。
    # counting_allocator.cpp 只链接进本可执行文件（全局 operator new 覆盖，
    # 绝不可链接进 libespview_protocol）。
    add_executable(espview_protocol_bench
        bench/protocol_bench.cpp
        bench/counting_allocator.cpp
    )
    target_link_libraries(espview_protocol_bench PRIVATE espview_protocol)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(espview_protocol_bench PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

option(ESPVIEW_BUILD_TESTS "Build host-side protocol tests" ON)
if(ESPVIEW_BUILD_TESTS)
    enable_testing()
    # M3：input 层测试依赖 shared/input（InputCodec/InputManager/...）。
    add_subdirectory(../input input_build)
    # M5-A：display 层测试依赖 shared/display（IDisplay/DisplayManager/RemoteDisplay）。
    add_subdirectory(../display display_build)
    # M6-C：transport 层测试依赖 shared/transport（ITransport/TransportManager/TransportSink）。
    add_subdirectory(../transport transport_build)
    # M7-A：OLED 层测试依赖 shared/oled（OledFb / 命令/分段生成）。
    add_subdirectory(../oled oled_build)
    # M8-C（C1）：渲染 pipeline 层测试依赖 shared/render（RenderPipeline / stages / factory）。
    add_subdirectory(../render render_build)
    # M7-E: ScanTransaction host tests (shared/wifi).
    add_subdirectory(../wifi wifi_build)
    add_subdirectory(tests)

    # M8-A4：Display/Input/OLED 收敛基准（任务书 §三十三）。依赖 display/input/
    # oled 库（tests 块创建），故置于此；与 espview_protocol_bench 独立。
    if(ESPVIEW_BUILD_BENCH)
        add_executable(espview_m8a4_bench
            ../bench/m8a4_bench.cpp
            bench/counting_allocator.cpp
        )
        target_include_directories(espview_m8a4_bench PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/bench
        )
        target_link_libraries(espview_m8a4_bench PRIVATE
            espview_display espview_input espview_oled espview_protocol
        )
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(espview_m8a4_bench PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()
endif()
