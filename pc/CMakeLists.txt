cmake_minimum_required(VERSION 3.16)
project(espview_pc_com3 LANGUAGES CXX)

# ESPView M1-3B：PC 侧 COM3 真实帧管线测试工具（C++17 + Win32 COM API）。
# ESPView M2：Qt 6 Virtual Display（espview_virtual_display，Qt 6 Widgets + SerialPort）。
# 不依赖 ESP-IDF；Qt 仅 M2 GUI target 需要（可用 -DESPVIEW_BUILD_QT_GUI=OFF 跳过）。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ESPVIEW_BUILD_TESTS OFF CACHE BOOL "Build host-side protocol tests" FORCE)
add_subdirectory(../shared/protocol protocol_build)
set_target_properties(espview_protocol PROPERTIES EXCLUDE_FROM_ALL TRUE)
# M3：shared/input（InputEvent / InputCodec / InputManager / KeyboardMapper /
# CoordinateMapper / InputPolicy）—— PC 与 ESP32 共用，零平台依赖。
add_subdirectory(../shared/input input_build)

# ------------------------------------------------------------
# M1-3B：com3_frame_test（真实 COM3 全模式验收工具，无 Qt）
# ------------------------------------------------------------
add_executable(com3_frame_test
    src/serial_transport.cpp
    src/com3_frame_test.cpp
)
target_include_directories(com3_frame_test PRIVATE src)
target_link_libraries(com3_frame_test PRIVATE espview_protocol)

if(MINGW OR MSYS)
    target_link_libraries(com3_frame_test PRIVATE kernel32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(com3_frame_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------
# M2：espview_virtual_display（Qt 6 Virtual Display）
# 规范来源：docs/DESIGN.md L/M2 节。复用 shared/protocol + HostUartTransport，
# 不复制协议实现；Qt 不接触 Packet/CRC/CHUNKED/SEQ/StreamDecoder 内部。
# 本机：MSYS2 MinGW64 Qt 6.11.1（C:\msys64\mingw64）。
# ------------------------------------------------------------
option(ESPVIEW_BUILD_QT_GUI "Build Qt 6 Virtual Display GUI" ON)
if(ESPVIEW_BUILD_QT_GUI)
    # 本机默认 Qt 根：MSYS2 MinGW64。可用 -DCMAKE_PREFIX_PATH=... 覆盖。
    if(WIN32 AND MINGW AND NOT CMAKE_PREFIX_PATH)
        set(CMAKE_PREFIX_PATH "C:/msys64/mingw64")
    endif()
    find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort)

    add_executable(espview_virtual_display
        src/serial_transport.cpp
        src/host_tcp_transport.cpp
        src/display_frame.h
        src/serial_worker.cpp
        src/serial_worker.h
        src/virtual_screen_widget.cpp
        src/virtual_screen_widget.h
        src/connection_manager.cpp
        src/connection_manager.h
        src/input/input_controller.cpp
        src/input/input_controller.h
        src/input/qt_key_adapter.cpp
        src/input/qt_key_adapter.h
        src/main.cpp
    )
    set_target_properties(espview_virtual_display PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(espview_virtual_display PRIVATE src src/input)
    target_link_libraries(espview_virtual_display PRIVATE
        espview_protocol
        espview_input
        Qt6::Widgets
        Qt6::SerialPort
    )
    if(MINGW OR MSYS)
        target_link_libraries(espview_virtual_display PRIVATE kernel32 ws2_32)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(espview_virtual_display PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# ------------------------------------------------------------
# M3：input_mapper_test（KeyboardMapper / CoordinateMapper / InputPolicy /
# qt_key_adapter 的 Qt::Key→HostKey 表）。依赖 QtCore（qt_key_adapter），
# 因此只在 ESPVIEW_BUILD_QT_GUI=ON 时构建（verify_host 以 Qt OFF 配置，
# 保证 host 回归不依赖 Qt）。
# ------------------------------------------------------------
if(ESPVIEW_BUILD_QT_GUI)
    add_executable(input_mapper_test
        src/input/qt_key_adapter.cpp
        src/input_mapper_test.cpp
    )
    set_target_properties(input_mapper_test PROPERTIES
        AUTOMOC OFF
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(input_mapper_test PRIVATE src src/input)
    target_link_libraries(input_mapper_test PRIVATE espview_input Qt6::Core)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(input_mapper_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# ------------------------------------------------------------
# M3：input_send_test（真实 COM3 输入链路验收工具，无 Qt）。
# Qt::Key 语义由 host 测试（input_mapper_test）覆盖；本工具直接使用
# 共享 InputEvent → input_codec → ProtocolEndpoint → HostUartTransport。
# ------------------------------------------------------------
add_executable(input_send_test
    src/serial_transport.cpp
    src/input_send_test.cpp
)
target_include_directories(input_send_test PRIVATE src)
target_link_libraries(input_send_test PRIVATE espview_protocol espview_input)

if(MINGW OR MSYS)
    target_link_libraries(input_send_test PRIVATE kernel32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(input_send_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------
# M6-A：tcp_transport_test（127.0.0.1 loopback host test，无 Qt / 无真实 Wi-Fi）。
# 覆盖 §二十八 1-19：connect / disconnect / reconnect / partial recv / sticky recv /
# send short write / remote close / timeout / invalid address / multiple-connect
# refusal + Protocol integration（HELLO / PING/PONG / FULL / CHUNKED FULL /
# PARTIAL / CRC corruption / seq gap / FULL resync / reconnect resync）。
# 依赖 WinSock2（MinGW 链接 ws2_32；`#pragma comment` 仅 MSVC）。
# ------------------------------------------------------------
add_executable(tcp_transport_test
    src/host_tcp_transport.cpp
    src/tcp_transport_test.cpp
)
target_include_directories(tcp_transport_test PRIVATE src ../shared/protocol/tests)
target_link_libraries(tcp_transport_test PRIVATE espview_protocol)

if(MINGW OR MSYS)
    target_link_libraries(tcp_transport_test PRIVATE ws2_32 kernel32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tcp_transport_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------
# M6-D：transport_config_test（纯 host 配置校验测试，无 Qt / 无硬件）。
# 覆盖 validateTransportConfig：默认值（TCP/COM4/115200/0.0.0.0:8765）、
# 合法配置、非法配置（空 COM、baud=0、TCP 空 bind、port=0）、相等比较。
# ------------------------------------------------------------
add_executable(transport_config_test
    src/transport_config_test.cpp
)
target_include_directories(transport_config_test PRIVATE src ../shared/protocol/tests)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(transport_config_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME transport_config_test COMMAND transport_config_test)
