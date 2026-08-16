# 硬件要求与接线（Hardware）

> 本页是「跑通 ESPView」需要的硬件清单与接线图。所有引脚/地址均为本仓库实测值
> （见 [docs/DESIGN.md](DESIGN.md) Y 节 / AK 节）。

## 1. 硬件清单

| 部件 | 要求 | 说明 |
| --- | --- | --- |
| ESP32 开发板 | 经典 ESP32（双核 Xtensa，`IDF_TARGET="esp32"`） | 验收板为 **ESP32-D0WDQ6 rev v1.1** |
| Flash | 4 MiB | 分区表：single factory app = 2 MiB、无 OTA（`esp32\partitions.csv`） |
| USB-UART | CH340（板载或外置模块） | 经典 ESP32 无原生 USB，阶段 1 只能 UART-over-USB |
| 线材 | USB 数据线（最好带屏蔽/短距离） | 供电稳定性敏感，见 §4 |
| Wi-Fi | 2.4 GHz 局域网 | 仅 TCP 模式需要；AP 断电（outage）未纳入验收 |
| OLED（可选） | 128×64 I2C，SSD1306 或 SH1106 | 诊断显示 + 物理显示后端，地址 `0x3C` |
| PC | Windows + MSYS2 MinGW64 + Qt 6 | 构建与 GUI 目标，见 [docs/getting-started.md](getting-started.md) |

## 2. UART 链路（协议通道）

- 协议 UART 使用 **UART0**（ESP32 侧 `ESPVIEW_UART_PORT=0`，默认 TX=GPIO1、RX=GPIO3），
  经板载 CH340 映射为 PC 的 COM 口。
- **正式 baseline = 115200 8N1**（M1-3C 冻结）；921600 为 experimental，见
  [docs/uart.md](uart.md)。
- 固件默认 console-off（`CONFIG_ESP_CONSOLE_NONE=y`），GPIO9/10 不占用，避免干扰启动/flash。
- 复位脉冲（验收/握手用）：外置 USB-SERIAL 模块实测接线为 **DTR→GPIO0、RTS→EN**；
  板载 CH340 的 COM4 同样支持 DTR/RTS 复位（`--no-reset` 可跳过）。

## 3. OLED 接线（可选，SSD1306 128×64）

| OLED 引脚 | 接 ESP32 | 说明 |
| --- | --- | --- |
| SDA | **GPIO21** | I2C 数据（Kconfig `ESPVIEW_OLED_SDA_GPIO=21`） |
| SCL | **GPIO22** | I2C 时钟（Kconfig `ESPVIEW_OLED_SCL_GPIO=22`） |
| VCC | 3.3V | 模块供电 |
| GND | GND | 共地 |

- I2C 地址：**7-bit `0x3C`**（实测）。固件默认 AUTO 探测 `0x08..0x77`，优先 `0x3C/0x3D`；
  探测到后上报遥测 `oled a=0x3C c=SSD1306 err=0 ok=1`。
- 控制器：AUTO（默认按 SSD1306）/ 强制 SSD1306 / 强制 SH1106（SH1106 是 132×64、
  列偏移 2，命令序列不同）。
- I2C 时钟 400 kHz；板级无外部上拉时驱动开启内部上拉（`enable_internal_pullup=1`），
  高速/长线场景建议外接上拉。
- 详细驱动语义（页式 1KB fb、≤32B 分段上传、有界恢复）见 [docs/oled.md](oled.md)。

## 4. 供电注意事项（重要）

- 本项目验收记录了一条重要的环境相关现象：**Wi-Fi RF 上电与 USB-UART/板级环境不稳定高度相关；物理机制属 hypothesis/high confidence；电源余量/EMI/USB 瞬态待硬件验证。**
  该表述是有意的固定措辞：当前没有任何电压/电流实测能证明「供电不足」，只记录了
  Observed 现象与高可信假设（见 DESIGN.md AI.3 / AJ.2 / AK.4 结论强度分级）。
- 现象：Wi-Fi RF 上电（boot 期或 `WIFI_SCAN_REQ` 后）与 CH340 USB 掉线时间上强相关
  （判别实验：无 RF 的 `uart_hw` 固件 45s 无掉线；RF 固件 boot ~747ms 掉线）。
  少数复现 ESP32 无复位横幅挂死。
- 软件侧已做的缓解（无法可靠消除掉线）：PASSIVE 扫描（无 probe 发射）、
  `CONFIG_PM_ENABLE=y` + 固定 80MHz、`esp_wifi_set_max_tx_power(8)`（2dBm）、
  扫描期间 OLED 挂起（M7-E，见 [docs/oled.md](oled.md)）。
- **推荐硬件路径**：带供电 USB HUB / 外接 5V 供电 / 换回板载 CH340 / 优质线材；
  供电改善后可直接复用 `scripts\espview_e_ab_harness.py` 完成扫描期 A/B/C 对比。

## 5. 关于 CH340 版本与波特率

- 部分 CH340 版本在 2M 波特率下不稳定；正式 baseline 一律 115200。
- 外置 USB-SERIAL CH340 模块（COM4）与板载 CH340 在本仓库都实测过；
  掉线现象与外置模块 + RF 上电相关（AK.3 证据矩阵），板载路径稳定无掉线（M6-C 基线）。

## 6. 不支持/未验收的硬件项

- 无原生 USB 设备模式（TinyUSB 未实现）；无真实 LCD（DEVICE/MIRROR 实机未做，
  当前为四模式路由中的 Virtual/Physical 组合，见 [docs/display-modes.md](display-modes.md)）；
  无触摸（`INPUT_TOUCH` 为未来类型）。
