# Example: UART 115200 baseline（端到端）

> **Hardware required: ESP32 开发板（经典 ESP32 D0WDQ6 已验证）+ USB-UART（CH340）+ USB 线**。
> 正式 baseline 为 **115200 8N1**；921600 仅 experimental（短包 / 控制面）。

## 1. 构建固件

```bat
scripts\espview_build.bat -esp32      :: 默认 profile uart_hw（UART + LVGL demo，console off）
```

## 2. 烧录

```bat
scripts\espview_flash.bat --dry-run   :: 先预检参数 / bin / flash_args
scripts\espview_flash.bat -p COM4     :: 烧录（默认 profile uart_hw，COM4 按实际替换）
```

## 3. PC 侧虚拟显示

```bat
scripts\verify_qt.bat
build\verify_qt\espview_virtual_display.exe --transport uart --port COM4 --baud 115200
```

期望：HELLO -> 首帧 FULL commit -> PARTIAL 流动，0 CRC 错误。
注意：320x240 整帧 @115200 约 13.5 s，FULL 帧慢属正常，等待即可。

参考：`docs/uart.md`、`docs/troubleshooting.md`、[quickstart.md](quickstart.md)
