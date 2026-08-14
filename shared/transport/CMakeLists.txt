cmake_minimum_required(VERSION 3.16)
project(espview_transport LANGUAGES CXX)

# M6-C：shared/transport 纯 C++17 静态库（ESP32 与 host 测试共用；零平台依赖）。
add_library(espview_transport STATIC
    transport_manager.cpp
    transport_sink.cpp
)
target_include_directories(espview_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(espview_transport PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_transport PRIVATE -Wall -Wextra -Wpedantic)
endif()
