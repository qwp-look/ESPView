# 显示模式（Display Modes）

> M7-C1 冻结四模式路由，M7-C3 冻结 PC 侧 UI 语义，M8-B B3 硬化切换状态机。数值 0..3 是 wire 上的 SET_MODE
> payload（`kWindow=0` / `kDevice=1` / `kMirror=2` / `kSplit=3`，协议常量名沿用历史）。
> 用户可见文案只显示模式名，绝不显示 0/1/2/3。

## 1. 四种模式语义（冻结，DESIGN.md AA.7 / AB.2）

| 值 | 模式 | Virtual（PC Qt） | Physical（OLED 应用帧） | 场景 |
| --- | --- | --- | --- | --- |
| 0 | Virtual Only | Application 帧 | 禁用（OLED 继续独立系统诊断页） | Diagnostics |
| 1 | Physical Only | 禁用（Qt 仍是控制器/状态面板） | Application 帧 | Application |
| 2 | Mirror | Application 帧 | Application 帧（同一逻辑源，不要求像素级一致） | Application |
| 3 | Split | Application 帧 | Diagnostics | 双场景并存 |

- 模式→场景映射由 `esp32/main/main.cpp` 的 `sceneOf(mode)` 派生：Mirror/PhysicalOnly →
  Application（OLED 显示 LVGL 应用缩略帧）；Split/VirtualOnly → Diagnostics（诊断页持续刷新）。
- OLED 系统诊断页（transport/session/IP/RSSI/FRM/ERR/HEAP/UP）在任何模式下都独立存在，
  见 [docs/oled.md](oled.md)。

## 2. v0.1 现状：编译期默认 + 运行时切换

- 上电初始模式 = Kconfig `ESPVIEW_DEFAULT_MODE`（int 0..3，默认 0=Virtual Only）。
- 当前现成构建 profile `uart_hw` 使用 `ESPVIEW_DEFAULT_MODE=2`（Mirror），
  OLED、LVGL、TEST hooks 全开（见 `scripts\espview_build.bat -h` 与
  `scripts\README.md` profile 说明）。
- **运行时切换已实现**（M7-C3 + M8-B B3 硬化）：UI Apply → `SET_MODE`（ACK_REQ）→
  ACK ok → FULL resync pending → 新 FULL 帧 → READY。两阶段看门狗：ACK 超时 10s →
  Failed；ACK ok 但 FULL 未到 → FULL 超时 15s（幂等，恢复 Apply 可重试）；ACK fail
  → 回退到 appliedMode + 错误提示。任何 PhysicalOnly/Mirror/Split 切换失败后
  仍可 Apply VirtualOnly 强制恢复（M8-B 最高优先级要求，host 测试覆盖）。
- 早期设计（DESIGN.md F 节）只实现编译期模式；M7-C 之后运行时 SET_MODE 0..3 全部
  可用（HELLO `mode_mask=0b1111`；SET_MODE 白名单 0..3）。

## 3. PC 侧 UI 操作

- 下拉框四模式选择 + Apply；Apply 期间禁用（防重复）。
- 物理模式（1/2/3）的可选性由 OLED 遥测驱动（capability-driven）：收到
  `oled a=... c=... err=... ok=1` → physicalAvailable=true → 可选；未收到 →
  标为 "(Unavailable)"。默认允许选择全部模式，但状态标签一律以遥测为准
  （无遥测 → Unknown，禁止显示 OK）。
- 断开时允许改选择，Apply 进入 "Waiting for connection"（不假装成功）；重连且
  FULL 后若 selected != applied 自动补发一次 SET_MODE。
- 模式切换与 Transport 切换正交：走独立 `sendDisplayMode` 队列，保留会话，
  不复用 `switchTransport`（不 stop/join）。
- Split 模式下右侧 Drawer（200..560px）显示物理诊断/状态文本 + 物理预览
  （见 [docs/oled.md](oled.md) 的 PHYSICAL_PREVIEW）；绝不在 drawer 伪造 OLED
  framebuffer（当前 wire 无 Physical framebuffer uplink）。

## 4. 测试钩子（生产固件关闭）

- `CONFIG_ESPVIEW_TEST_MODE_SWITCH`（默认 n）：=y 时 F11（HID 0x44）运行时循环
  0→1→2→3→0，经 ERROR 文本通道上报 `mod sw=<mode> st=<router state> scene=<scene>`。
- `CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH`（默认 n）：=y 时 F12 运行时切换 UART↔TCP
  （M6-D 验收钩子）。生产 profile 两者均关闭。
- 这些钩子非 wire 格式、不进生产固件；普通 F1–F12 键对 LVGL 应用**不映射**
  （见 [docs/input.md](input.md) §7）。

## 5. 相关命令

```bat
:: 构建 uart_hw 验收固件（DEFAULT_MODE=2，Mirror）
scripts\espview_build.bat -esp32 -b uart_hw

:: 启动 GUI 后：Transport UI 下拉选择模式 → Apply → 观察状态面板 Display Mode 分组
build\verify_qt\espview_virtual_display.exe --transport uart --port COM4 --baud 115200
```
