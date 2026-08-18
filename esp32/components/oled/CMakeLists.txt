# ESPView M7-A/M7-C2：OLED 显示组件（esp_driver_i2c 新 API）。
# 复用 shared/oled 纯 C++17 源码（照 display 组件绝对路径模式，不复制）。
# M7-A/B：诊断状态显示，不依赖 protocol/transport/lvgl；状态由 main 经
# StatusSnapshot 注入。M7-C2：本组件承载 PhysicalDisplaySink（IDisplaySink），
# 依赖 shared/display 抽象与 shared/oled PhysicalRenderer（Agent D 交付）；
# PhysicalDisplaySink 仅在 CONFIG_ESPVIEW_OLED_ENABLE 时编译（OLED=n 无未定义引用）。
set(OLED_COMPONENT_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_fb.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_cmd.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_status.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_recovery.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/physical_renderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/scene_renderer.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled/oled_preview.cpp"
    "src/oled_i2c.cpp"
    "src/oled_controller.cpp"
    "src/oled_display.cpp"
)
# M7-C2：PhysicalDisplaySink（IDisplaySink 实现）仅在 OLED 启用时编译；
# OLED_ENABLE=n 时 main 不注册/不引用，无未定义引用。
if(CONFIG_ESPVIEW_OLED_ENABLE)
    list(APPEND OLED_COMPONENT_SRCS "src/physical_display_sink.cpp")
endif()

idf_component_register(
    SRCS ${OLED_COMPONENT_SRCS}
    INCLUDE_DIRS
        "include"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/oled"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/display"
    REQUIRES
        display
        esp_driver_i2c
        freertos
        driver
        esp_timer
        esp_common
        log
    PRIV_REQUIRES
        target_info  # M8-A7：compile-time target traits（仅 src/ 引用）
)

# 编译要求：IDF 默认 -Wall -Werror；C++ 标准至少 C++17。
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)