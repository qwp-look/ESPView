# ESPView 快速开始（Quick Start）

> 目标：从零到「跑通测试 / 烧录固件 / 看到画面」的最短路径，命令复制即用。
> 环境：Windows PowerShell + MSYS2 MinGW64 + ESP-IDF v6.0.2（profile
> `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1`）。
> 详细测试与通过标准见 `docs/testing.md`；构建/烧录脚本参数见 `scripts/README.md`。

## 0. 仓库布局

```text
shared/   协议 / 显示 / 传输 / OLED / Wi-Fi 纯 C++17 源码 + host 单测
pc/       PC 侧：Qt Virtual Display + 硬件测试工具
esp32/    ESP-IDF 固件（LVGL app，UART / TCP 传输）
scripts/  verify_host / verify_qt / verify_lvgl / build / flash 脚本
docs/     DESIGN.md（设计 + 实测证据）、testing.md（测试指南）
```

## 1. Host-only 验证（无需硬件）

```bat
scripts\verify_host.bat
```

期望：末尾 `verify_host: ALL PASS`，退出码 0（ctest 2/2，协议套件 ~38 万 checks）。
等价手拆命令与通过标准见 `docs/testing.md` §2。

## 2. Qt 构建检查（无需硬件）

```bat
scripts\verify_qt.bat
```

期望：`verify_qt: ALL PASS (espview_virtual_display.exe)`。

## 3. ESP32 固件构建 + 烧录（UART baseline，需 ESP32 + COM 口）

```bat
scripts\espview_build.bat -esp32        :: 构建默认 profile uart_hw（LVGL + UART 115200）
scripts\espview_flash.bat -p COM4       :: 烧录（115200；不想真烧先试 --dry-run）
```

前提：ESP32 已连接（板载 CH340 或外置 USB-SERIAL 模块），先运行
`scripts\espview_build.bat --check` 确认 MSYS2 / ESP-IDF 环境可用。

## 4. 真实硬件链路（UART）

```bat
set ESPVIEW_COM3=COM4
scripts\verify_lvgl.bat                 :: host 测试 + 固件构建 + 可选 COM sanity
```

或者手动跑 LVGL 验收：

```bat
<python> scripts\pc_com3_lvgl_sanity.py --port COM4 --baud 115200
```

期望：首帧 FULL + PARTIAL 流动、0 CRC、dirty ratio 输出，脚本退出码 0。

## 5. Wi-Fi + TCP 模式（示例配置）

仓库默认 `sdkconfig.defaults` 是 UART 传输。要跑 TCP 模式：

1. 复制示例配置到 ESP32 工程（该文件名被 `.gitignore` 的 `esp32/sdkconfig.*`
   覆盖，不会误提交）：

   ```powershell
   Copy-Item ..\examples\sdkconfig.wifi-tcp.defaults.example esp32\sdkconfig.defaults.wifi-tcp
   ```

2. 编辑 `esp32\sdkconfig.defaults.wifi-tcp`，把 `REPLACE_WITH_*` 占位符替换为
   本地值（SSID / 密码 / PC 的 LAN IP）。**真实凭据只保存在本机，绝不提交。**

3. 构建并烧录：

   ```powershell
   . 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
   cd esp32
   idf.py -B build\wifi-tcp -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wifi-tcp" build
   idf.py -p COM4 flash
   ```

4. PC 侧启动 TCP server + 虚拟显示（从 `build\verify_qt` 路径启动，防火墙规则
   只放行该 exe）：

   ```bat
   build\verify_qt\espview_virtual_display.exe
   ```

   期望：PC 侧监听 `0.0.0.0:8765`，ESP32 连入该端口 → HELLO → 首帧 FULL commit
   → PARTIAL 流动。

> 备选（不使用示例片段）：按 `README.md` 用 `idf.py menuconfig` 在本地
> `esp32\sdkconfig` 注入 Wi-Fi 凭据与 PC 地址后 `idf.py -p COM4 flash monitor`。

## 6. 硬件注意事项（重要）

- 正式 UART baseline = **115200 8N1**；921600 仅 experimental（短包/控制面）。
- 当前板卡存在已知环境限制：Wi-Fi RF 上电与 CH340 USB 掉线强相关（高可信假设，
  未证实因果）；Wi-Fi/TCP 硬件测试建议使用带供电 USB HUB 或外接 5V 供电。
- 320×240 整帧 @115200 ≈ 13.5 s：UART 模式下 FULL 帧很慢属正常，PC 端等待即可。
- 详见 `docs/testing.md` §7 Known Limitations。

## 7. 凭据红线

- 不写真实凭据到本仓库任何文件；示例一律用占位符。
- Wi-Fi SSID/密码只存在于本地未跟踪 `esp32\sdkconfig*`（`.gitignore` 已排除，
  例外 `esp32/sdkconfig.defaults`）。
