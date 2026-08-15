# ESPView M7-A/M7-B — shared/oled 纯 C++17 共享库（PC/ESP32 测试与 ESP32 侧复用）。
# OledFb（128x64 页式 framebuffer）+ SSD1306/SH1106 命令/分段生成 + 配置校验/
# 恢复策略状态机/状态渲染；零平台依赖。ESP32 侧由 esp32/components/oled 复用。
cmake_minimum_required(VERSION 3.16)

add_library(espview_oled STATIC
    oled_fb.cpp
    oled_cmd.cpp
    oled_status.cpp
    oled_recovery.cpp
)
target_include_directories(espview_oled PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(espview_oled PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_oled PRIVATE -Wall -Wextra -Wpedantic)
endif()