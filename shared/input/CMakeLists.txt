# ESPView M3 — shared/input 纯 C++17 共享库（PC 与 ESP32 共用）。
# InputEvent / InputCodec / InputManager / KeyboardMapper / CoordinateMapper / InputPolicy。
# 零平台依赖（无 Qt / ESP-IDF / Windows API）。ESP32 侧由 esp32/components/input
# 复用同一份源码（与 shared/protocol 组件模式一致）。
cmake_minimum_required(VERSION 3.16)
project(espview_input LANGUAGES CXX)

add_library(espview_input STATIC
    input_codec.cpp
    input_manager.cpp
    keyboard_mapper.cpp
    coordinate_mapper.cpp
    hid_lvgl_keymap.cpp
    lvgl_adapter.cpp
)
target_include_directories(espview_input PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol  # input_codec 依赖 message.h
)
target_compile_features(espview_input PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_input PRIVATE -Wall -Wextra -Wpedantic)
endif()
