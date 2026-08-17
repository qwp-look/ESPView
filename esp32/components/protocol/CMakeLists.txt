# shared/protocol 纯 C++17 共享库（ESP32 侧复用同一份源码，不复制）。
# 与 host-side 测试（shared/protocol/tests）共用同一实现，保证两端契约一致。
idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/crc32.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/packet.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/message.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/encoder.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/decoder.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/frame_assembler.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/protocol_endpoint.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol/runtime_stats.cpp"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../shared/protocol"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/transport"
)

# 编译要求：IDF 默认 -Wall -Werror；C++ 标准至少 C++17。
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

