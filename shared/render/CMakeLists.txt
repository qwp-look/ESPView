# ESPView M8-C (C1) — shared/render 纯 C++17 渲染 pipeline 库（PC/ESP32 测试与
# ESP32 侧复用；零平台依赖）。RenderFormat / IRenderStage / RenderPipeline /
# PipelinePlan / 各 stage / PipelineFactory。ESP32 侧由 esp32/components/oled
# 复用同一份源码（C2 起）。
cmake_minimum_required(VERSION 3.16)

add_library(espview_render STATIC
    render_pipeline.cpp
    stages/luminance_stage.cpp
    stages/threshold_stage.cpp
    stages/nearest_scale_stage.cpp
    stages/bilinear_scale_stage.cpp
    stages/crop_stage.cpp
    stages/ordered_dither_stage.cpp
    stages/fast_scale_threshold_stage.cpp
    pipeline_factory.cpp
)
target_include_directories(espview_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(espview_render PUBLIC cxx_std_17)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(espview_render PRIVATE -Wall -Wextra -Wpedantic)
endif()
