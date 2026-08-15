# ESPView M7-A：独立 OLED 状态显示组件（esp_driver_i2c 新 API）。
# 复用 shared/oled 纯 C++17 源码（照 display 组件绝对路径模式，不复制）。
# OLED 组件协议无关：不依赖 protocol/display/lvgl；状态由 main 经
# StatusSnapshot 注入（std::function）。
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_fb.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_cmd.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_status.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_recovery.cpp"
        "src/oled_i2c.cpp"
        "src/oled_controller.cpp"
        "src/oled_display.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled"
    REQUIRES
        esp_driver_i2c
        freertos
        driver
        esp_timer
        esp_common
        log
)

# 编译要求：IDF 默认 -Wall -Werror；C++ 标准至少 C++17。
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
