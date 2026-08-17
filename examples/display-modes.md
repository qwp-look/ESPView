# Example: 四种显示模式

> **Hardware required: ESP32**（PhysicalOnly / Mirror 模式另需 OLED；纯 VirtualOnly 无额外硬件）。

模式语义已冻结（wire `SET_MODE` 0..3，加法语义）：

| Mode | Qt Virtual Display | OLED | 用途 |
| --- | --- | --- | --- |
| VirtualOnly (0) | Application (LVGL frame) | Diagnostics（系统状态页） | 默认；PC-only |
| PhysicalOnly (1) | Disabled | Application（LVGL 缩略） | OLED-only |
| Mirror (2) | Application | Application | 同一逻辑帧（不要求像素级一致） |
| Split (3) | Application | Diagnostics | Qt + OLED 诊断并行 |

## 运行期切换

Qt 应用的模式下拉框发送 `SET_MODE`（ACK_REQ），随后做一次 FULL resync。
v0.1 中 OLED sink 可选（`CONFIG_ESPVIEW_OLED_ENABLE`，默认 n）；未启用 OLED 时，
要求物理 sink 的模式会被优雅拒绝。

## 编译期默认模式

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'
cd esp32
idf.py -B build\uart_hw menuconfig
# ESPView Display Routing -> Default DisplayRouteMode (0..3)
```

默认值 0（VirtualOnly）；`CONFIG_ESPVIEW_DEFAULT_MODE` 在 Kconfig 中定义。

参考：`docs/display-modes.md`、`docs/oled.md`
