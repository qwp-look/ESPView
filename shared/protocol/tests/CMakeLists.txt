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
    streaming_encoder_test.cpp
    input_test.cpp
    lvgl_adapter_test.cpp
    runtime_stats_test.cpp
    ../../display/tests/remote_display_test.cpp
    ../../transport/tests/transport_manager_test.cpp
    ../../transport/tests/transport_sink_test.cpp
    ../../transport/tests/transport_pipeline_test.cpp
)
target_link_libraries(espview_protocol_tests PRIVATE espview_protocol espview_input espview_display espview_transport)

# remote_display_test.cpp 位于 shared/display/tests（相对引用本目录 test_util.h）。
target_include_directories(espview_protocol_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../display/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../../transport
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_protocol_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_test(NAME espview_protocol_tests COMMAND espview_protocol_tests)

