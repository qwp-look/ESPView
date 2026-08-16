# OLED（SSD1306 / SH1106）

> OLED 承担两个角色：①独立系统诊断显示（M7-A）；②正式 Physical Display Sink
> （M7-C2，经 DisplayRouter 收 LVGL 应用帧）。它**永远不是权威 framebuffer**。
> 硬件接线见 [docs/hardware.md](hardware.md)。

## 1. 硬件与驱动参数（实测冻结）

| 参数 | 值 |
| --- | --- |
| 面板 | 128×64，1bpp 页式 fb（8 pages × 128B = 1KB） |
| 控制器 | SSD1306（实测）/ SH1106（132×64、列偏移 2，可选） |
| I2C 地址 | 7-bit `0x3C`（AUTO 探测 0x08..0x77，优先 0x3C/0x3D） |
| SDA / SCL | GPIO21 / GPIO22（Kconfig `ESPVIEW_OLED_SDA_GPIO` / `ESPVIEW_OLED_SCL_GPIO`） |
| I2C 时钟 | 400 kHz；内部上拉（板级无外部上拉时） |
| 刷新周期 | `ESPVIEW_OLED_REFRESH_MS`（默认 500ms；M7-A 期状态页为 1000ms 节拍） |
| 上传 | 每段 ≤32B（含控制字节），同步 I2C（`trans_queue_depth=0`） |
| 任务 | `espview_oled`，优先级 2（低于 stats=3 / session=5），栈 4096 |
| 启用 | `CONFIG_ESPVIEW_OLED_ENABLE`（默认 **n**；=n 时零代码路径） |

- 控制器探测只能确认地址、无法可靠区分 SSD1306/SH1106 → Kconfig choice：
  AUTO（默认 SSD1306）/ 强制 SSD1306 / 强制 SH1106。
- OLED 任务**绝不触碰 protocol sendMutex / Transport**；I2C 上传只发生在 OLED 任务内。

## 2. 诊断页与遥测

- 状态页 8 行（8×8 字体）：`ESPView` / `UART|TCP + 会话状态` / `IP <ip>|--` /
  `RSSI CH`（仅 TCP 且 apInfo 有效）/ `FRM <n>` / `ERR <n>` / `HEAP <n>` / `UP hh:mm:ss`。
- 诊断遥测（ERROR 文本通道，非 wire 格式，statsLoop 每 3s 一行）：
  `oled a=0x3C c=SSD1306 err=0 ok=1`。`ok=1` 同时是 PC 侧判断物理显示可用的唯一
  真实观测源（capability-driven UI）。
- 被动监控：`py -3.10 scripts\pc_oled_monitor.py`

## 3. 物理显示后端（M7-C2）

- `PhysicalDisplaySink`（`esp32\components\oled`）注册进 `DisplayRouter`：
  capabilities = 128×64 / mono 1bpp / canReadback=false / kPhysical。
- 渲染：`PhysicalRenderer`（`shared\oled`，host 可测）RGB565→Mono1
  （luminance = (299R+587G+114B)/1000，threshold 128），源 320×240 按 2/5 最近邻
  缩放 + 垂直 center crop（可见源区 y∈[40,199]），无堆分配；整帧 153600B 实测 ≈0.25ms。
- Application 场景下经共享 1KB 应用 fb（mutex 保护，绝不持有像素指针）；Diagnostics
  场景 no-op（诊断页由 OLED 任务自绘）。
- 物理不可用（未 kReady）→ Router 收敛 kDegraded，Virtual 侧不受影响。

## 4. 扫描期间 suspend/resume（M7-E）

- Wi-Fi 扫描（RF 活动窗口）期间 OLED 刷新默认**挂起**（Kconfig
  `ESPVIEW_SCAN_SUSPEND_OLED`，默认 **y** = mode B）：挂起窗口 I2C 流量 = 0、
  preview 停发、`refreshCount` 不递增。
- 状态机：`shared\wifi\scan_transaction.{h,cpp}`（纯 C++17，ESP32 与 host 测试共用），
  相位 `kIdle → kPreparing → kDisplaySuspended → kScanning → kCollecting →
  kRestoring → kIdle`；核心不变量：一旦 suspend 成功，任何终态路径（成功/失败/
  超时/断线）恰好调用一次 resume。
- 挂起是原子正交标志（`OledState::kSuspendedForWifiScan` 为观察态），不落生命周期；
  挂起**绝不**关闭/重建 I2C bus；10s 看门狗兜底扫描卡死。
- 挂起中 `PhysicalDisplaySink::isAvailable()` 返回 false → Router 短暂 kDegraded，
  恢复后自愈（不视为错误状态）。
- A/B/C 实验（`scripts\espview_e_ab_harness.py`）：A = suspend=n；B = suspend=y
  （生产默认）；C = OLED=n。实验默认不构建/不烧录（假设固件已就绪）。

## 5. PHYSICAL_PREVIEW（ESP32→PC，M7-D2）

- `TYPE 0x13`，payload 1032B 定长（frameId 2B + width 2B + height 2B + pixelFormat 1B
  + flags 1B + 1024B mono pixels），单包（<4096B），fire-and-forget。
- 频率上限 10Hz，默认 2Hz（对齐 500ms 节拍）；UART 115200 下为 1Hz。
- PC 侧 `PhysicalPreviewWidget` 显示在 Split Drawer 顶部；`ui/previewEnabled`
  QSettings 白名单键控制开关。
- 挂起窗口整函数跳过（preview 字节流量 = 0）。

## 6. 镜像问题（历史根因，供排查）

- 2026-08-15 实机发现水平镜像：segment remap 0xA1/0xA0 对照均镜像（控制器疑似忽略
  remap 命令）→ 修正为按页反转列序上传（地址列 C ← fb 列 127-C）；随后发现字形仍
  单个镜像 → `drawText` 字体按 **bit0=最左** 读取（字体表即 bit0 约定）。
- 自购新屏出现镜像时按此顺序排查：①`oled a=` 遥测确认地址/控制器；②确认固件
  为 2026-08-15 后的镜像修正版本；③逐项对照 Y.1 记录。
- 排查步骤见 [docs/troubleshooting.md](troubleshooting.md)「OLED 镜像/无显示」。

## 7. 错误恢复（有界）

- 连续失败达 `MAX_REINIT`（3）触发恢复：bus_reset + 重发 init/清屏 → 失败则整体重建
  （bus → probe → addDevice → init）；重初始化轮数有界 + 指数退避 0.5s→30s + 30s
  冷却，**不允许无限重置循环**。
- `stop()` 幂等：置停止标志 + 唤醒 + 有界等待（2s）。
