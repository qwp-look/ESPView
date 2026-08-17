# Example: OLED（SSD1306 / SH1106 128x64）

> **Hardware required: ESP32 + I2C OLED**（SSD1306 / SH1106，128x64；地址自动探测，实测 0x3C）。

## 接线

| OLED | ESP32 |
| --- | --- |
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |

I2C 400 kHz；OLED 刷新周期可配置，默认 500 ms。

## 构建 + 烧录

```bat
scripts\espview_build.bat -esp32 -b oled   :: OLED profile（或 menuconfig 开启 CONFIG_ESPVIEW_OLED_ENABLE）
scripts\espview_flash.bat -p COM4 -b oled
```

## 行为

- 两个角色：**诊断 / 状态页**（传输 / 会话 / IP / RSSI / 帧与错误计数 / heap / uptime）
  与 **PhysicalOnly / Mirror 模式的物理预览**（128x64 单色，约 2 Hz）。
- Wi-Fi 扫描期间 OLED 刷新默认挂起（`CONFIG_ESPVIEW_SCAN_SUSPEND_OLED=y`），
  扫描事务终止路径恢复。
- **OLED 永远不是权威 framebuffer**：ESP32 上的 LVGL 帧才是。

参考：`docs/oled.md`、[display-modes.md](display-modes.md)、`docs/hardware.md`
