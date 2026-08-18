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
# M7-C3：shared/display（DisplayUiState / PhysicalStatus / SplitState 纯模型，
# 零平台依赖；GUI 目标链接后复用同一份源码）。
add_subdirectory(../shared/display display_build)
# M8-A3：shared/transport（TransportManager / TransportSink / BoundedQueue ——
# 唯一 canonical transport 契约；PC backend 直接实现它，已删除 IPcTransport）。
add_subdirectory(../shared/transport transport_build)

# ------------------------------------------------------------
# M1-3B：com3_frame_test（真实 COM3 全模式验收工具，无 Qt）
# ------------------------------------------------------------
add_executable(com3_frame_test
    src/serial_transport.cpp
    src/com3_frame_test.cpp
)
target_include_directories(com3_frame_test PRIVATE src)
target_link_libraries(com3_frame_test PRIVATE espview_protocol espview_transport)

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
        src/display_mode_widget.cpp
        src/display_mode_widget.h
        src/display_status_panel.cpp
        src/display_status_panel.h
        src/i18n.cpp
        src/i18n.h
        src/language_selector.cpp
        src/language_selector.h
        src/split_drawer.cpp
        src/split_drawer.h
        src/physical_preview_state.cpp
        src/physical_preview_state.h
        src/physical_preview_widget.cpp
        src/physical_preview_widget.h
        src/wifi_wizard_state.cpp
        src/wifi_wizard_state.h
        src/wifi_wizard_dialog.cpp
        src/wifi_wizard_dialog.h
        src/virtual_screen_widget.cpp
        src/virtual_screen_widget.h
        src/connection_manager.cpp
        src/connection_manager.h
        ../shared/wifi/reconnect_policy.cpp  # M8-A5（HOSTUART-04）：UART 重开退避
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
    target_include_directories(espview_virtual_display PRIVATE src src/input ../shared/wifi)
    target_link_libraries(espview_virtual_display PRIVATE
        espview_protocol
        espview_input
        espview_display
        espview_transport
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
# M7-C3：i18n host test（纯 C++17，零 Qt；独立 main）
# ------------------------------------------------------------
add_executable(i18n_test
    src/i18n.cpp
    src/i18n_test.cpp
)
target_include_directories(i18n_test PRIVATE src)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(i18n_test PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME i18n_test COMMAND i18n_test)

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
# M8-B（B1/B2）：virtual_screen_offscreen_test —— Qt offscreen 布局 /
# 动态分辨率测试（QT_QPA_PLATFORM=offscreen，不依赖真实显示器）。
# 覆盖 letterbox 渲染、resize 不重建 QImage、动态分辨率 logicalSize。
# ------------------------------------------------------------
if(ESPVIEW_BUILD_QT_GUI)
    add_executable(virtual_screen_offscreen_test
        src/i18n.cpp
        src/virtual_screen_widget.cpp
        src/input/input_controller.cpp
        src/input/input_controller.h
        src/input/qt_key_adapter.cpp
        src/input/qt_key_adapter.h
        src/virtual_screen_offscreen_test.cpp
    )
    set_target_properties(virtual_screen_offscreen_test PROPERTIES
        AUTOMOC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(virtual_screen_offscreen_test PRIVATE src src/input)
    target_link_libraries(virtual_screen_offscreen_test PRIVATE
        espview_input
        espview_display
        Qt6::Widgets
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(virtual_screen_offscreen_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME virtual_screen_offscreen_test COMMAND virtual_screen_offscreen_test)
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
target_link_libraries(input_send_test PRIVATE espview_protocol espview_input espview_transport)

if(MINGW OR MSYS)
    target_link_libraries(input_send_test PRIVATE kernel32)
endif()

# ------------------------------------------------------------
# M7-D6：wifi_provision_probe（真实 UART Wi-Fi 扫描探测工具，无 Qt）。
# 链路：HostUartTransport → ProtocolEndpoint → WIFI_SCAN_REQ → 真实空口
# SCAN_RESULT；PASS = 收到扫描结果或老固件 ACK ERR 降级。不接收任何凭据。
# ------------------------------------------------------------
add_executable(wifi_provision_probe
    src/serial_transport.cpp
    src/wifi_provision_probe.cpp
)
target_include_directories(wifi_provision_probe PRIVATE src)
target_link_libraries(wifi_provision_probe PRIVATE espview_protocol espview_transport)

if(MINGW OR MSYS)
    target_link_libraries(wifi_provision_probe PRIVATE kernel32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(wifi_provision_probe PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(input_send_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------
# M7-F：win32_com_probe（最小 Win32-only serial probe，无 Qt / 无协议）。
# 纯字节搬运：只读原始串口、可选 EN 复位脉冲、掉线自动重枚举。不链接
# shared/protocol，不涉及任何 Wi-Fi 凭据。
# ------------------------------------------------------------
add_executable(win32_com_probe
    src/win32_com_probe.cpp
)

if(MINGW OR MSYS)
    target_link_libraries(win32_com_probe PRIVATE kernel32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(win32_com_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
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
target_link_libraries(tcp_transport_test PRIVATE espview_protocol espview_transport)

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
target_link_libraries(transport_config_test PRIVATE espview_transport)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(transport_config_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME transport_config_test COMMAND transport_config_test)
