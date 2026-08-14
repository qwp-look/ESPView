# ESPView M3：shared/input 纯 C++17 共享库（ESP32 侧复用同一份源码，不复制）。
# 与 host-side 测试 / PC 端共用同一实现，保证两端契约一致（同 protocol 组件模式）。
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/input/input_codec.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/input/input_manager.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/input/keyboard_mapper.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/input/coordinate_mapper.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/input/hid_lvgl_keymap.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/input/lvgl_adapter.cpp"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../shared/input"
    REQUIRES protocol
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
