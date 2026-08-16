# ESPView M7-E — shared/wifi 纯 C++17 静态库 espview_wifi（ScanTransaction）。
# Wi-Fi 扫描期间 OLED/I2C 暂停/恢复事务状态机；零平台依赖（PC host 测试与
# ESP32 侧共用；ESP32 侧由 esp32/components/espview 复用）。
# host 测试：独立可执行 scan_transaction_test（定义 SCAN_TRANSACTION_TEST_MAIN
# 提供 main；并入 shared/protocol host 套件时去掉该定义即可，无 main 冲突）。
cmake_minimum_required(VERSION 3.16)
project(espview_wifi LANGUAGES CXX)

add_library(espview_wifi STATIC
    scan_transaction.cpp
)
target_include_directories(espview_wifi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(espview_wifi PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_wifi PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(ESPVIEW_BUILD_TESTS "Build host-side wifi scan transaction tests" ON)
if(ESPVIEW_BUILD_TESTS)
    enable_testing()
    add_executable(scan_transaction_test
        tests/scan_transaction_test.cpp
    )
    target_link_libraries(scan_transaction_test PRIVATE espview_wifi)
    # 复用 ../protocol/tests/test_util.h（仅 include，不修改）。
    target_include_directories(scan_transaction_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/tests
    )
    # 独立可执行需要 main；并入协议套件时去掉该定义即可（无 main 冲突）。
    target_compile_definitions(scan_transaction_test PRIVATE SCAN_TRANSACTION_TEST_MAIN)
    target_compile_features(scan_transaction_test PUBLIC cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(scan_transaction_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME scan_transaction_test COMMAND scan_transaction_test)
endif()