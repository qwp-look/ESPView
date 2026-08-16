# 输入链路（INPUT_KEY / INPUT_MOUSE）

> PC 捕获的鼠标/键盘事件沿反向链路发回 ESP32 驱动 LVGL。wire 上只有两种输入消息：
> `INPUT_KEY`（TYPE 0x20）与 `INPUT_MOUSE`（TYPE 0x21）；`INPUT_TOUCH`（0x22）为未来类型。
> 完整语义见 [docs/DESIGN.md](DESIGN.md) G/S 节（M3/M5-B 冻结，wire format 未修改）。

## 1. 链路

```
Qt VirtualScreenWidget → InputController → INPUT_* → Transport
  → ESP32 ProtocolEndpoint → InputManager → LvglInputAdapter
  → lv_indev read_cb（POINTER / KEYPAD / ENCODER）→ LVGL demo UI
```

- 键码统一用 **USB HID usage table**：PC 端 Qt::Key → HID 映射（`pc\src\input\qt_key_adapter.cpp`），
  ESP32 端直接消费，不依赖 Windows 虚拟键码。
- 鼠标坐标使用显示坐标系（0..width-1, 0..height-1），PC 按当前分辨率换算后发送。

## 2. 发送端语义（PC，M3 冻结）

| 事件 | 行为 |
| --- | --- |
| 键盘 autoRepeat | Qt `autoRepeat` 的 KeyDown **直接忽略**（不重复发）；KeyUp **始终发送** |
| MouseMove | **≤60Hz 节流 + 合并**，只发最新坐标 |
| MouseDown/Up/Wheel | 实时发送，不节流 |
| 断线 | PC 丢弃未发送的输入队列；重连握手后从全新状态继续 |

- `INPUT_MOUSE` 无事件类型字段：坐标 + buttons 掩码 + wheelDelta；ESP32 按
  buttons 相对上一状态的**变化位**推导 Down/Up。
- 输入走 Worker 线程队列（GUI 线程不直接操作串口）。

## 3. 接收端语义（ESP32）

- `InputManager::resetState()`：Transport 断开/会话重置时，对按下中的键补发本地
  KeyUp、对按下中的鼠标按钮补发 MouseUp(buttons=0)，**绝不把 release 回发给 PC**。
- `LvglInputAdapter`（`shared\input\lvgl_adapter.cpp`，纯 C++17 不含 lvgl.h）：
  - Pointer：点击保持窗口 `kPointerClickHoldReads=2`（LeftUp 后冻结点击点保持
    PRESSED 若干次 read_cb，保证 LVGL ~30ms 轮询不漏点击）；
  - Keyboard：32 项有界 ring buffer，满时丢**最新**项（不会造成 stuck key）；
  - Wheel：`wheelAccum_` 累加 → `enc_diff`，+1/-1/+2/-2 原样到达；
  - 越界坐标 → invalidEvents 计数并忽略，不 crash LVGL。
- 按钮映射：LEFT = LVGL pointer 唯一 primary 语义；**RIGHT/MIDDLE 记录
  ignoredButtons 统计但不消费**（LVGL v8.4 pointer indev 无第二/第三按钮，
  capability limitation，不改协议）。

## 4. 键映射与能力边界（S.7 冻结）

- 已映射：A–Z、0–9、空格、标点、Enter/Esc/Backspace/Tab、方向键、Home/End/Delete、
  Keypad（`shared\input\hid_lvgl_keymap.cpp` 线性表）。
- **不映射**（unmappedKeys 计数，不 crash）：修饰键 0xE0..0xE7（Ctrl/Shift/Alt/GUI，
  修饰状态已表达在 modifiers 掩码，LVGL 侧不合成修饰事件）、**F1–F12**、
  PageUp/PageDown、Insert、CapsLock 等。
- 组合键（Ctrl+A / Shift+A）在 PC 侧是独立事件序列；到 LVGL 只有 'A' 键事件。
- 注意：F11/F12 在**测试固件**中是验收钩子（模式循环/传输切换），生产固件
  `CONFIG_ESPVIEW_TEST_MODE_SWITCH=n`、`CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH=n`
  时 F11/F12 走正常（不映射）输入链路，见 [docs/display-modes.md](display-modes.md)。

## 5. 可观测性

- 诊断行（ERROR 文本通道，非 wire 格式）：`inp3 w=<wheelEvents>
  s=<wheelSteps> k=<consumedKeys> d=<keyQueueDropped> u=<unmappedKeys>
  b=<ignoredButtons> r=<resets>`。
- 延迟语义：ESP32 侧记录的是 local scheduler latency（input 收到 → LVGL read_cb
  消费），不是网络 RTT；FULL 帧传输期间 read_cb 被背压延迟，但输入在
  InputManager/adapter 层不丢失（实测 27/27 到达、0 dropped）。

## 6. 真实命令

```bat
:: 真实 COM3 输入链路验收工具（M3，无 Qt；pc\CMakeLists.txt 注册，需先构建）
cmake -S pc -B build\verify_host\pc -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=OFF
cmake --build build\verify_host\pc --target input_send_test -j 8
build\verify_host\pc\input_send_test.exe --port COM4 --baud 115200

:: host 映射单测（Qt::Key → HID，无硬件；Qt 目标构建内）
cmake -S pc -B build\verify_qt -G "MinGW Makefiles" -DESPVIEW_BUILD_QT_GUI=ON
cmake --build build\verify_qt --target input_mapper_test -j 8
build\verify_qt\input_mapper_test.exe

:: host 输入协议/适配器套件（并入 verify_host.bat）
scripts\verify_host.bat
```

## 7. 实测记录（M5-B，真实硬件 COM3 @ 115200）

- 鼠标 move/left/right/middle、wheel ±1、A/B/Enter/方向键、Ctrl+A、Shift+A：
  27 事件全收，0 dropped / 0 invalid。
- reconnect：按住 Ctrl+Left 断开 → 重连后 `inp2 r=1 sk=1 sb=1`，pressed keys=0 /
  buttons=0（无卡键/卡鼠标）。
- LVGL 消费：FULL 帧完成后 consumedKeys=4（A down/up + Enter down/up），
  wheel w=2 入账。
