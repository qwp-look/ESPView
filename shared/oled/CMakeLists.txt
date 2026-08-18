# ESPView M7-A/M7-B/M7-C2/M7-D2 — shared/oled 纯 C++17 共享库（PC/ESP32 测试与 ESP32 侧复用）。
# OledFb（128x64 页式 framebuffer）+ SSD1306/SH1106 命令/分段生成 + 配置校验/
# 恢复策略状态机/状态渲染 + PhysicalRenderer（RGB565->Mono1 降采样渲染）+
# OledPreviewSlot（M7-D2 PHYSICAL_PREVIEW 预览槽，seqlock 无锁）；
# 零平台依赖。ESP32 侧由 esp32/components/oled 复用。
cmake_minimum_required(VERSION 3.16)

add_library(espview_oled STATIC
    oled_fb.cpp
    oled_cmd.cpp
    oled_status.cpp
    oled_recovery.cpp
    physical_renderer.cpp
    scene_renderer.cpp
    oled_preview.cpp
)
target_include_directories(espview_oled PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../display  # M8-B B2: SceneRenderer 依赖 display::LogicalScene
)
target_compile_features(espview_oled PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_oled PRIVATE -Wall -Wextra -Wpedantic)
endif()

# M7-D2：OledPreviewSlot host 测试。独立可执行目标 oled_preview_test（仅在 host 测试构建
# 上下文；verify_host 的 `--target espview_protocol_tests` 不构建它，故不注册 add_test，
# 避免 ctest 报 Not Run）。验证：cmake --build <build>/protocol --target oled_preview_test
# 后直接运行 exe。测试体 runOledPreviewTests() 亦可并入 shared/protocol host 套件
# （协议侧 test_main.cpp + tests/CMakeLists.txt 登记由主代理/协议代理负责——本文件
# 不动协议侧任何文件）。复用 ../protocol/tests/test_util.h（仅 include，不修改）。
if(ESPVIEW_BUILD_TESTS)
    enable_testing()
    add_executable(oled_preview_test
        tests/oled_preview_test.cpp
    )
    target_link_libraries(oled_preview_test PRIVATE espview_oled)
    target_include_directories(oled_preview_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../protocol/tests
    )
    # 独立可执行需要 main；并入协议套件时去掉该定义即可（无 main 冲突）。
    target_compile_definitions(oled_preview_test PRIVATE OLED_PREVIEW_TEST_MAIN)
    target_compile_features(oled_preview_test PUBLIC cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(oled_preview_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
