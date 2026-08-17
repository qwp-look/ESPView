# Example: Wi-Fi STA + TCP（端到端）

> **Hardware required: ESP32 + USB-UART（配网引导）+ Wi-Fi AP + 同一局域网 PC**。
> TCP server 目前仅支持单客户端；AP 故障场景尚未做硬件验证（见 `docs/wifi.md`）。

## 1. 构建 TCP profile

```bat
scripts\espview_build.bat -esp32 -b tcp
```

## 2. 注入 Wi-Fi 凭据与 PC 地址

- 方式 A（推荐）：Qt 应用内 Wi-Fi Wizard，见 [wifi-provisioning.md](wifi-provisioning.md)。
- 方式 B（menuconfig）：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
cd esp32
idf.py -B build\tcp menuconfig
# ESPView -> Protocol transport -> Wi-Fi STA + TCP
# ESPView -> Wi-Fi SSID = <your-wifi-ssid>          (example only)
# ESPView -> Wi-Fi password = <your-wifi-password>  (example only)
# ESPView -> PC TCP server IPv4 = <pc-server-ip>    (example only, port 8765)
```

真实凭据只保存在本地未跟踪的 `esp32/build/tcp/sdkconfig`，绝不提交。

## 3. 重建并烧录

```bat
scripts\espview_build.bat -esp32 -b tcp
scripts\espview_flash.bat -p COM4 -b tcp
```

## 4. PC 侧 TCP server

```bat
build\verify_qt\espview_virtual_display.exe --transport tcp --tcp-bind 0.0.0.0 --tcp-port 8765
```

期望：ESP32 连入 8765 -> HELLO -> 首帧 FULL -> PARTIAL 流动；
同局域网实测约 0.2-0.7 s/帧（默认 Wi-Fi 省电）。

参考：`docs/wifi.md`、[wifi-provisioning.md](wifi-provisioning.md)
