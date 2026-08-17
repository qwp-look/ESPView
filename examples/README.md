# ESPView 可运行示例（Examples）

> 每个示例都是「复制即用」的命令序列。凡是需要真实硬件的示例都显式标注
> **Hardware required**；纯 host 示例不伪装成硬件示例。
> 占位符一律使用 `<...>` 或 `REPLACE_WITH_*`，仓库任何文件绝不出现真实凭据
> （SSID / 密码 / 本地 IP），详见 `docs/security.md`。

| 示例 | 内容 | Hardware required |
| --- | --- | --- |
| [host-verification.md](host-verification.md) | host 单测 + ctest + pc 工具（纯 host） | 无 |
| [benchmark.md](benchmark.md) | 协议 / 显示 / 输入 / OLED benchmark + 基线对比（纯 host） | 无 |
| [uart.md](uart.md) | UART 115200 baseline 端到端（构建 -> 烧录 -> PC 显示） | ESP32 开发板 + USB-UART（CH340） |
| [tcp.md](tcp.md) | Wi-Fi STA + TCP 端到端 | ESP32 + USB-UART + Wi-Fi AP + 同一局域网 PC |
| [wifi-provisioning.md](wifi-provisioning.md) | Wi-Fi Wizard 配网（UART 引导，RAM-only 凭据） | ESP32 + USB-UART + Wi-Fi AP |
| [display-modes.md](display-modes.md) | 四种显示模式（VirtualOnly / PhysicalOnly / Mirror / Split） | ESP32（物理模式另需 OLED） |
| [input.md](input.md) | HID 输入反向链路（键盘 / 鼠标 -> ESP32 / LVGL） | ESP32 + 完整显示链路 |
| [oled.md](oled.md) | SSD1306 / SH1106 128x64 OLED 接线 + 验证 | ESP32 + I2C OLED |

另见：

- 最短路径（从零到画面）：[quickstart.md](quickstart.md)
- Wi-Fi / TCP sdkconfig 占位片段：[sdkconfig.wifi-tcp.defaults.example](sdkconfig.wifi-tcp.defaults.example)
  （**example only**：只含占位符，真实凭据绝不提交）
- 各专题文档索引：`docs/README.md`
