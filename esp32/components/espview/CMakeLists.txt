# ESPView 组件（M1-1：UartTransport；M6-A：Wi-Fi STA + TcpTransport；
# M8-A3：TransportBase 共享骨架 + shared/transport TransportManager/TransportSink；legacy 适配层已删除）。
idf_component_register(
    SRCS
        "src/uart_transport.cpp"
        "src/wifi_sta.cpp"
        "src/wifi_provisioning.cpp"
        "src/tcp_transport.cpp"
        "src/transport_base.cpp"  # M8-A3：UART/TCP backend 共享骨架（原 transport_manager.cpp 适配层已删除）
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/transport/transport_manager.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/transport/transport_sink.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/wifi/scan_transaction.cpp"  # M7-E：ScanTransaction（Agent K 并行交付）
    INCLUDE_DIRS
        "include"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/transport"
        "${CMAKE_CURRENT_LIST_DIR}/../../../shared/wifi"  # M7-E：scan_transaction.h
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
