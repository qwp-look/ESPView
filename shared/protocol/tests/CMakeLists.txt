add_executable(espview_protocol_tests
    test_main.cpp
    byte_order_test.cpp
    decoder_timeout_test.cpp
    ack_req_test.cpp
    counting_allocator.cpp
    crc_test.cpp
    packet_test.cpp
    encoder_test.cpp
    decoder_test.cpp
    frame_assembler_test.cpp
    pipeline_test.cpp
    frame_pipeline_test.cpp
    protocol_endpoint_test.cpp
    endpoint_concurrency_test.cpp
    ack_concurrency_test.cpp
    endpoint_race_test.cpp
    endpoint_lifecycle_test.cpp
    endpoint_stress_test.cpp
    deferred_control_test.cpp
    capabilities_test.cpp
    physical_preview_test.cpp
    wifi_provisioning_test.cpp
    streaming_encoder_test.cpp
    input_test.cpp
    lvgl_adapter_test.cpp
    runtime_stats_test.cpp
    ../../display/tests/remote_display_test.cpp
    ../../display/tests/display_router_test.cpp
    ../../display/tests/display_ui_state_test.cpp
    ../../display/tests/logical_scene_test.cpp
    ../../display/tests/physical_status_test.cpp
    ../../display/tests/physical_capability_snapshot_test.cpp
    ../../display/tests/split_state_test.cpp
    ../../transport/tests/transport_manager_test.cpp
    ../../transport/tests/transport_sink_test.cpp
    ../../transport/tests/transport_pipeline_test.cpp
    ../../transport/tests/bounded_queue_test.cpp
    ../../transport/tests/lifecycle_test.cpp
    ../../wifi/tests/reconnect_policy_test.cpp
    ../../oled/tests/oled_test.cpp
    ../../oled/tests/oled_status_test.cpp
    ../../oled/tests/physical_renderer_test.cpp
    ../../oled/tests/scene_renderer_test.cpp
    ../../oled/tests/oled_preview_test.cpp
    ../../render/tests/render_pipeline_test.cpp
    ../../../pc/src/physical_preview_state.cpp
    ../../../pc/src/physical_preview_state_test.cpp
    ../../../pc/src/wifi_wizard_state.cpp
    ../../../pc/src/wifi_wizard_state_test.cpp
)
target_link_libraries(espview_protocol_tests PRIVATE espview_protocol espview_input espview_display espview_transport espview_oled espview_wifi espview_render)

# remote_display_test.cpp 位于 shared/display/tests（相对引用本目录 test_util.h）。
target_include_directories(espview_protocol_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../display/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../transport
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../pc/src
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_protocol_tests PRIVATE -Wall -Wextra -Wpedantic)
    # M7-B：GCC16 对 remote_display_test.cpp 的 -Warray-bounds 属编译器误报
    # （make_shared + std::function 析构链），仅对该文件单独抑制；不改测试逻辑。
    # M8-A2：加编译器判定（仅 GNU/Clang）——MSVC 的 cl 拒绝该参数（Curie F）。
    set_source_files_properties(../../display/tests/remote_display_test.cpp PROPERTIES
                                COMPILE_OPTIONS "-Wno-array-bounds")
    # M8-A5：GCC16 对 lifecycle_test.cpp 的 -Warray-bounds 同属 std::function 误报（与 remote_display 同模式）。
    set_source_files_properties(../../transport/tests/lifecycle_test.cpp PROPERTIES
                                COMPILE_OPTIONS "-Wno-array-bounds")
endif()

add_test(NAME espview_protocol_tests COMMAND espview_protocol_tests)

# M8-A6：TSan 专用 core-concurrency subset（默认不构建；sanitizer CI 开启）。
# 只链接任务书 §十三/§四十二/§四十三 要求的并发/生命周期测试；不链接
# counting_allocator.cpp（TSan 需要标准 malloc 拦截）。Linux TSan 专用。
option(ESPVIEW_BUILD_TSAN_SUBSET "Build TSan core-concurrency subset target" OFF)
if(ESPVIEW_BUILD_TSAN_SUBSET)
    add_executable(espview_tsan_concurrency_tests
        tsan_main.cpp
        endpoint_race_test.cpp
        ack_concurrency_test.cpp
        endpoint_lifecycle_test.cpp
        deferred_control_test.cpp
        ../../transport/tests/lifecycle_test.cpp
    )
    target_link_libraries(espview_tsan_concurrency_tests PRIVATE
        espview_protocol espview_transport
    )
    target_include_directories(espview_tsan_concurrency_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../transport
        ${CMAKE_CURRENT_SOURCE_DIR}/../../transport/tests
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(espview_tsan_concurrency_tests PRIVATE -Wall -Wextra -Wpedantic)
        # 与 espview_protocol_tests 相同的 GCC16 -Warray-bounds 误报抑制。
        set_source_files_properties(../../transport/tests/lifecycle_test.cpp PROPERTIES
                                    COMPILE_OPTIONS "-Wno-array-bounds")
    endif()
    add_test(NAME espview_tsan_concurrency_tests COMMAND espview_tsan_concurrency_tests)
endif()
