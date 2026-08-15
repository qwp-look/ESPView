idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES espview protocol input testpattern esp_timer lvgl_port oled
)
