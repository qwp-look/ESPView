# Example: Wi-Fi 配网（Wi-Fi Wizard）

> **Hardware required: ESP32 + USB-UART + Wi-Fi AP**。
> 配网引导必须走 UART（凭据路径 UART-only），完成 apply 后凭据立即从 RAM 擦除。

## 步骤

1. 烧录任意 UART profile 固件（`uart` 或 `tcp`）并保持 USB-UART 连接（见 [uart.md](uart.md)）。
2. PC 启动 Qt 虚拟显示（UART transport）：

```bat
build\verify_qt\espview_virtual_display.exe --transport uart --port COM4 --baud 115200
```

3. 打开 Wi-Fi Wizard，按顺序执行：

   1. **Init** -- 连接说明；确认 ESP32 通过 USB-UART 在线。
   2. **Connect + capabilities** -- 应用连接并读取设备能力。
   3. **Scan** -- `WIFI_SCAN_REQ` -> 返回扫描列表（扫描期间 OLED 刷新默认挂起）。
   4. **Select SSID, enter password** -- 空密码 = 开放网络。
   5. **Configure TCP server** -- 填 PC 的 LAN IP 与端口（8765）。
   6. **Apply** -- `WIFI_CONFIG`(ACK) -> ESP32 Wi-Fi connect -> `WIFI_STATUS`
      （connecting -> GOT_IP -> TCP connected）-> 首帧 FULL -> **Done**。

4. 若 UART 链路断开，UI 显示 "UART bootstrap unavailable"，不会误报密码错误。

参考：`docs/wifi.md`、`README.md`「Configuring Wi-Fi (Wi-Fi Wizard)」
