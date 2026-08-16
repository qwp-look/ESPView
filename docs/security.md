# 凭据与安全策略（Security）

> 本页汇总 ESPView 的凭据纪律：**Wi-Fi SSID/密码只存在于内存或本地未跟踪文件**，
> 绝不入库、绝不进日志/遥测/UI。所有示例均为占位符。

## 1. 凭据在哪里（允许的位置）

| 位置 | 允许？ | 说明 |
| --- | --- | --- |
| PC 内存（WizardState / QLineEdit） | 允许（临时） | 密码发送后立即清零；模型无密码序列化 API |
| ESP32 RAM（wifi_provisioning 副本） | 允许（临时） | 接收→RAM 副本→应用后清零；消息缓冲内副本同步清零 |
| 本地未跟踪 `esp32\sdkconfig` | 允许 | 已被 `.gitignore` 排除（覆盖 `esp32/sdkconfig*`）；只在本机存在 |
| git / 源码 / 文档 / 日志 | **禁止** | 所有示例用占位符 |
| QSettings | **禁止存密码** | 白名单键名不含 wifi/ssid/password/psk/credential |
| ERROR 文本遥测 / 诊断 / RuntimeStats | **禁止** | 日志只可记 SSID 长度，绝不打印/上报密码 |
| PNG dump / UI 文案 | **禁止** | 密码不可见 |

## 2. QSettings 白名单（9 键，M7-C3 冻结）

`persistedSettingsKeys()` 只允许保存：

- `transport/type`
- `uart/port`
- `uart/baud`
- `tcp/port`
- `window/size`
- `display/mode`
- `ui/language`
- `split/drawerVisible`
- `split/drawerWidth`

结构性保证：白名单键名不含 wifi/ssid/password/psk/credential；`TransportConfig`
无凭据字段。Wi-Fi 向导 `toSettingsMap()` 只导出 `tcpServerIp` / `tcpServerPort`
（SSID 为可选 metadata，默认也不导出）。

## 3. 构建/烧录脚本纪律（M7-D5/F4）

- `scripts\espview_build.bat` / `espview_flash.bat` / `espview_build_flash.bat`
  **从不读取或打印 `esp32\sdkconfig`**，也不把任何凭据写入文件；输出只显示
  固件 bin 路径与 profile 名。
- per-profile sdkconfig 隔离：`idf.py -D SDKCONFIG=build\<profile>\sdkconfig`
  （首次从本地 `esp32\sdkconfig` 引导拷贝），共享 sdkconfig 不再被构建/烧录覆盖
  （根因修复 M7-E 配置漂移）。
- `espview_e_ab_harness.py` 只发 `WIFI_SCAN_REQ`（不配置网络、密码零参与）；
  SSID 为非秘密 metadata（同既有探针）。

## 4. 配网安全边界（重要）

- 配网（`WIFI_CONFIG`）经 UART 下发为**明文载荷**（CRC32 仅完整性、非加密）；
  **M7-D 只能做局域网开发工具用途，不宣称安全 provisioning；不实现 TLS**。
- 建议：UART bootstrap 配网完成后断开物理通道；威胁模型限于可信开发局域网。
- TCP Server 默认 bind `0.0.0.0` 仅适合受信局域网；默认建议 bind `127.0.0.1`
  （Peer token 机制留待后续）。
- 凭据路径锁定 UART-only：WIFI_SCAN_RESULT / WIFI_STATUS **绝无密码字段**；
  密码永不进日志/遥测/诊断/PNG/UI（仅可记录长度）。
- NVS 明文持久化（"上电免配网"）为显式选项但**本阶段不实现**；当前无 flash
  encryption，属 plaintext at-rest 风险，须记录 + 提供 CLEAR 动作。

## 5. CLEAR 与开放网络

- `WIFI_CONFIG` 的 CLEAR（flags bit0）清除 ESP32 RAM 中的凭据并断开。
- 开放网络：向导显式勾选 "Open network (no password)"，空密码才允许 Apply；
  禁止用假密码填充。
- 探针降级：老固件对 `WIFI_SCAN_REQ` 回 ACK ERR → 向导提示「固件不支持
  Wi-Fi provisioning」，**不发真实凭据**。

## 6. 扫描纪律

- 扫描只发 `WIFI_SCAN_REQ`（maxEntries 可选，默认 32），密码零参与；
- 挂起/恢复机制零 SSID/密码参与；向导文案只显示「显示临时暂停/已恢复」，
  杜绝「电源不足已证实」暗示（i18n 测试断言文案不含 power/insufficient/电源/电量/不足）。

## 7. 红线措辞（CH340/RF）

- **RF 上电与 USB-UART/板级环境不稳定高度相关；物理机制属 hypothesis/high confidence；电源余量/EMI/USB 瞬态待硬件验证。** 禁止写「已确认供电不足」。
