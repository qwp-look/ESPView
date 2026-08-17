# Example: HID 输入反向链路

> **Hardware required: ESP32 + 完整显示链路**（PC 虚拟显示，或 OLED 物理预览）。

## 步骤

1. 先建立显示链路：UART（[uart.md](uart.md)）或 TCP（[tcp.md](tcp.md)）。
2. 在 PC Qt 虚拟显示窗口内点击 / 按键：应用把输入编码为 `INPUT_KEY` /
   `INPUT_MOUSE` 反向消息发送到 ESP32。
3. ESP32 输入链注入 LVGL（`shared/input` 保持平台无关；USB stack 不进入 InputManager）。

期望：LVGL demo 对输入有可见响应（焦点 / 点击高亮 / 按键动作）；
协议消息定义见 `docs/DESIGN.md`（INPUT_KEY / INPUT_MOUSE）。

参考：`docs/input.md`、`docs/architecture-overview.md`
