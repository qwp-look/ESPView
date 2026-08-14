# ESPView M5-A：LVGL 封装组件（display driver + flush_cb + demo UI）。
# lvgl 来自 component manager（idf_component.yml: lvgl/lvgl ^8.3.0）。
# 职责：
#   - 注册 LVGL display driver（320x240 RGB565，1/10 屏 draw buffer）；
#   - flush_cb：px_map → RemoteDisplay::writeRect()（有界 staging + TX 队列），
#     无论成败都调用 lv_disp_flush_ready()；
#   - TX 任务经 ProtocolEndpoint::sendMessageStreaming 排空队列（115200 整帧节流）；
#   - 堆统计 / 帧统计（ERROR 文本通道，wire format 未修改）。
idf_component_register(
    SRCS "src/lvgl_port.cpp" "src/lvgl_app.cpp" "src/lvgl_indev.cpp"
    INCLUDE_DIRS "include"
    REQUIRES display esp_timer freertos heap input log lvgl
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
