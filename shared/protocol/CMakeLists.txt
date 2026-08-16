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
target_include_directories(espview_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(espview_protocol PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_protocol PRIVATE -Wall -Wextra -Wpedantic)
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
    # M7-E: ScanTransaction host tests (shared/wifi).
    add_subdirectory(../wifi wifi_build)
    add_subdirectory(tests)
endif()
