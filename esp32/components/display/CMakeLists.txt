# ESPView M5-A / M7-C2：shared/display 纯 C++17 共享库（ESP32 侧复用同一份源码，不复制）。
# M7-C2 追加 display_router.cpp（DisplayRouter 状态机 + presentScene；main/lvgl_port/oled 使用）。
# 与 host-side 测试（shared/protocol/tests 内 remote_display_test）共用同一实现，
# 保证两端契约一致（同 protocol / input 组件模式）。
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/display/display_manager.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/display/remote_display.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/display/display_router.cpp"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/display"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol"
    REQUIRES protocol
)

# 编译要求：IDF 默认 -Wall -Werror；C++ 标准至少 C++17。
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
