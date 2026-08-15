# ESPView M5-A — shared/display 纯 C++17 共享库（PC/ESP32 测试与 ESP32 侧复用）。
# IDisplay / DisplayManager / RemoteDisplay + 帧统计；零平台依赖
# （无 ESP-IDF / Qt / LVGL）。ESP32 侧由 esp32/components/display 复用同一份源码。
cmake_minimum_required(VERSION 3.16)

add_library(espview_display STATIC
    display_manager.cpp
    remote_display.cpp
    display_router.cpp
    display_ui_state.cpp
    physical_status.cpp
    split_state.cpp
    physical_capability_snapshot.cpp
)
target_include_directories(espview_display PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol  # remote_display 依赖 message/encoder/endpoint 头
)
target_compile_features(espview_display PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_display PRIVATE -Wall -Wextra -Wpedantic)
endif()
