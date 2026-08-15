add_executable(espview_protocol_tests
    test_main.cpp
    crc_test.cpp
    packet_test.cpp
    encoder_test.cpp
    decoder_test.cpp
    frame_assembler_test.cpp
    pipeline_test.cpp
    frame_pipeline_test.cpp
    protocol_endpoint_test.cpp
    endpoint_concurrency_test.cpp
    capabilities_test.cpp
    streaming_encoder_test.cpp
    input_test.cpp
    lvgl_adapter_test.cpp
    runtime_stats_test.cpp
    ../../display/tests/remote_display_test.cpp
    ../../display/tests/display_router_test.cpp
    ../../display/tests/display_ui_state_test.cpp
    ../../display/tests/physical_status_test.cpp
    ../../display/tests/physical_capability_snapshot_test.cpp
    ../../display/tests/split_state_test.cpp
    ../../transport/tests/transport_manager_test.cpp
    ../../transport/tests/transport_sink_test.cpp
    ../../transport/tests/transport_pipeline_test.cpp
    ../../oled/tests/oled_test.cpp
    ../../oled/tests/oled_status_test.cpp
    ../../oled/tests/physical_renderer_test.cpp
)
target_link_libraries(espview_protocol_tests PRIVATE espview_protocol espview_input espview_display espview_transport espview_oled)

# remote_display_test.cpp 位于 shared/display/tests（相对引用本目录 test_util.h）。
target_include_directories(espview_protocol_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../display/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../transport
)

# M7-B：GCC16 对 remote_display_test.cpp 的 -Warray-bounds 属编译器误报
# （make_shared + std::function 析构链），仅对该文件单独抑制；不改测试逻辑。
set_source_files_properties(../../display/tests/remote_display_test.cpp PROPERTIES COMPILE_OPTIONS "-Wno-array-bounds")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_protocol_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME espview_protocol_tests COMMAND espview_protocol_tests)
