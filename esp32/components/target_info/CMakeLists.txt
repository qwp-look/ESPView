# ESPView M8-A7 — compile-time target traits（header-only；零业务依赖）。
# 只提供 TargetInfo / GPIO 约束 / UART0 默认引脚等编译期事实；
# 被 espview（UART）与 oled（I2C）组件引用，避免 target 知识散落
# （M8-A7 任务书 §13/§14；DESIGN AS.2 D7）。
idf_component_register(
    INCLUDE_DIRS "include"
)
