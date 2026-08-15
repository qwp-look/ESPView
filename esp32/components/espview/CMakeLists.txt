# ESPView 组件（M1-1：UartTransport；M6-A：Wi-Fi STA + TcpTransport；M6-C：Transport 适配层）。
idf_component_register(
    SRCS
        "src/uart_transport.cpp"
        "src/wifi_sta.cpp"
        "src/wifi_provisioning.cpp"
        "src/tcp_transport.cpp"
        "src/transport_manager.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/transport/transport_manager.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/transport/transport_sink.cpp"
    INCLUDE_DIRS
        "include"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/transport"
    REQUIRES
        esp_driver_uart
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        lwip
        freertos
        esp_common
        log
    PRIV_REQUIRES
        esp_timer
        esp_pm
)

# 编译要求：IDF 默认 -Wall -Werror（无 warning 验收）；C++ 标准至少 C++17。
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
