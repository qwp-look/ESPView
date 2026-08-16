# 常见问题（FAQ）

> 对应任务书「常见问题」类的提问，结合本仓库真实实现给出答案。
> 每个答案都可追溯到 [docs/DESIGN.md](DESIGN.md) 的冻结章节；涉及 CH340/RF 的
> 措辞遵循红线：**RF 上电与 USB-UART/板级环境不稳定高度相关；物理机制属 hypothesis/high confidence；电源余量/EMI/USB 瞬态待硬件验证。**

## Q1. 为什么 UART 下一整帧要 ~13.5 秒？

320×240 RGB565 整帧 = 153600 B；115200 baud 的有效 payload 吞吐实测 ≈11.1 KB/s，
所以 FULL 帧 ≈13.5s（DESIGN.md J 节实测）。这是物理带宽上限，不是软件缺陷。常态
目标是 dirty-rect PARTIAL 增量：小矩形时负载 <5%。需要整屏吞吐请走 TCP
（Wi-Fi FULL 实测 235–254ms）。

## Q2. 为什么 921600 是 experimental，不能用于验收？

经典 ESP32 + 当前 CH340/Windows/驱动组合下，921600 大帧突发实测丢字节（短包/
控制面可用）；2M 波特率部分 CH340 版本不稳。正式 baseline 一律 115200 8N1
（M1-3C 冻结，DESIGN.md N.3 / X.16）。

## Q3. OLED 显示文字左右镜像怎么办？

镜像根因是字体/列序约定：固件按页反转列序上传 + 字体按 bit0=最左读取
（2026-08-15 修正，DESIGN.md Y.1）。确保固件为 M7-A 修正后版本；新屏仍镜像时
检查 Kconfig 控制器选择（AUTO 默认 SSD1306；SH1106 变体需强制指定）。
排查步骤见 [docs/troubleshooting.md](troubleshooting.md) §3。

## Q4. 为什么 Wi-Fi 扫描会失败/掉线？

RF 上电与 USB-UART/板级环境不稳定高度相关（红线措辞见上）。固件侧已做
PASSIVE 扫描 + PM 80MHz + 2dBm 降流组合与 10s 扫描看门狗；UART 控制路径
（ACK）验收通过，但本板卡的物理空口扫描依赖供电充足环境。供电改善后复用
`scripts\espview_e_ab_harness.py` 完成 A/B/C 对比。排查见 troubleshooting §2。

## Q5. 为什么 PC 侧出现 `ReadFile err=5`？

CH340 链路断开。判别实验表明与 Wi-Fi RF 上电时间强相关（无 RF 的 uart_hw 固件
45s 无掉线；RF 固件 boot ~747ms 掉线，DESIGN.md AK.3）。恢复：拔插 USB /
close+reopen；硬件路径：带供电 USB HUB / 外接 5V / 换回板载 CH340 / 优质线材。

## Q6. TCP 连不上是什么原因？

按顺序查：①PC Server bind/端口/防火墙（`--tcp-bind 0.0.0.0 --tcp-port 8765`）；
②固件侧 SSID/密码/PC IP（本地未跟踪 sdkconfig，占位符）；③ESP32 诊断行 `trx`
的 RSSI/channel（STA 未连接 vs 已连但 TCP 不可达）；④AP 断电（AP outage）未纳入
验收（DESIGN.md X.15）。详见 [docs/wifi.md](wifi.md) 与 troubleshooting §5。

## Q7. Drawer 里没有物理预览（No Preview）？

检查 `ui/previewEnabled` 开关、OLED 遥测 `ok=1`、等待 1–2s（2Hz/1Hz 节拍）；
扫描挂起窗口内 preview 停发属正常。详见 [docs/oled.md](oled.md) §5。

## Q8. 我的 Wi-Fi 凭据会不会被提交/入库？

不会。SSID/密码只存在于：PC 内存（向导）、ESP32 RAM（临时副本）、本地未跟踪
`esp32\sdkconfig`（`.gitignore` 排除）。QSettings 白名单 9 键不含任何凭据键；
脚本不读不打印 sdkconfig；密码不进日志/遥测/PNG/UI。详见
[docs/security.md](security.md)。

## Q9. F11 / F12 是干什么的？

测试固件专用验收钩子：F11（`CONFIG_ESPVIEW_TEST_MODE_SWITCH=y`）循环显示模式
0→1→2→3→0；F12（`CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH=y`）运行时切换 UART↔TCP。
生产固件两者默认 n，F1–F12 对 LVGL 不映射（见 [docs/input.md](input.md) §4）。

## Q10. 怎么切换显示模式？

GUI 下拉选择 + Apply（`SET_MODE` + ACK + FULL resync）；上电默认
`ESPVIEW_DEFAULT_MODE`（0..3）。物理模式可选性由 OLED 遥测 `ok=1` 驱动。
详见 [docs/display-modes.md](display-modes.md)。

## Q11. 怎么知道固件支持 Wi-Fi provisioning？

向导探针降级：对老固件发 `WIFI_SCAN_REQ`，回 ACK ERR（kInvalidParam）→ 提示
「固件不支持 Wi-Fi provisioning」，不发真实凭据（DESIGN.md AG.3 / AF.3）。
也可用 `build\verify_host\pc\wifi_provision_probe.exe --port COM4 --baud 115200`
直接探测（退出码 0 = 收到 SCAN_RESULT；该工具由 `pc\CMakeLists.txt` 注册，
`verify_host.bat` 不默认构建）。

## Q12. Split（模式 3）现在能做什么？

Split = Virtual=Application、Physical=Diagnostics 双场景并存（M7-C1/C2 已实现），
PC 侧 Drawer 显示物理诊断文本 + 预览。它**不是**第二套完整 framebuffer 镜像；
当前 wire 无 Physical framebuffer uplink（DESIGN.md AA/AB）。

## Q13. 我需要装什么工具链？

MSYS2 MinGW64（g++/cmake/ctest）+ Qt 6 + ESP-IDF v6.0.2 + Python 3.10（pyserial）。
先跑 `scripts\espview_build.bat --check` 验证。详见 [docs/getting-started.md](getting-started.md)。

## Q14. 如何验证构建/烧录流程但不真烧？

`scripts\espview_build.bat --check`（只 preflight）；
`scripts\espview_flash.bat --dry-run`（参数 + COM + bin 校验，不烧录）；
`scripts\verify_host.bat`（host 全套，无需硬件）。

## Q15. 怎么验证画面像素正确？

GUI 加 `--dump-png <dir>` 保存每个 FULL commit 的 `full_<frameId>.png`，
与 LVGL 画面比对；仓库还有 `scripts\verify_png_pixels.ps1` 像素校验工具与
`scripts\pc_com3_lvgl_sanity.py`（真实硬件帧流 sanity）。
