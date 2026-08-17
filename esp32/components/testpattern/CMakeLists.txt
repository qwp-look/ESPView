# ESPView M1-3C：TestPattern（确定性帧发送脚本，test-only）。
# heap：heap_caps_get_free_size（大 RECT 堆统计）；esp_timer：流式耗时统计。
idf_component_register(
    SRCS "src/test_pattern.cpp"
    INCLUDE_DIRS "include"
    REQUIRES display protocol freertos heap esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
