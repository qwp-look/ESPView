# 变更日志（Changelog）

> 里程碑历史提炼自 `git log`（真实提交）。当前里程碑范围 M0..M8-B 已提交（HEAD 见 git log）。
> M0..M6-E 集中在 v0.1 初始提交；M7/M8 系列逐里程碑提交。
> （docs/ 共 19 份文档，均已随里程碑提交）。

## v0.1 基础（M0..M6-E，2026-08-12..14）

| 提交 | 日期 | 内容 |
| --- | --- | --- |
| `540be96` | 2026-08-12 | Initial commit（项目初始化） |
| `fb2f44e` | 2026-08-14 | ESPView v0.1：M0–M6-E 完成——协议核心（Packet/Message/Frame + CRC32）、UART/TCP transport、Qt Virtual Display、LVGL backend、input bridge、生产 profile、README & DESIGN |
| `3e1e181` | 2026-08-14 | readme |

## M7-A..C4（OLED 与多显示，2026-08-15）

| 提交 | 里程碑 | 内容 |
| --- | --- | --- |
| `aa9890a` | M7-B | OLED 生产语义收尾：lifecycle / transport snapshot / observability + DESIGN Z 节 |
| `1d4689a` | M7-C1 | Display 抽象：DisplayCapabilities / IDisplaySink / DisplayRouter + additive kSplit |
| `ef48ba7` | M7-C2 | Physical display backend（OLED 正式成为 Physical Sink） |
| `9e01a99` | M7-C3 | Qt 多显示 UI：四模式选择、Split Drawer、i18n（en+zh） |
| `ba9489b` | M7-C4 | Capability 与 Split 诊断打磨 |

## M7-D（协议扩展与 UX，2026-08-15）

| 提交 | 里程碑 | 内容 |
| --- | --- | --- |
| `01ab4cf` | M7-D 冻结 | DESIGN AD/AE/AF：CAPABILITIES / PHYSICAL_PREVIEW / Wi-Fi provisioning 协议冻结 |
| `92be619` | M7-D1 | CAPABILITIES 消息（定长 32B，多字节 LE） |
| `666c1f0` | M7-D2 | PHYSICAL_PREVIEW（1032B 单包，2Hz 默认） |
| `a3eddff` | M7-D3 | Wi-Fi provisioning 协议族（0x06..0x09 + 错误码 5..12 + WifiStatusPhase） |
| `1fbebd5` | M7-D4 | Wi-Fi Wizard（WifiWizardState + Dialog + i18n） |
| `c039032` | M7-D5 | Build/Flash 工作流（espview_build / espview_flash / espview_build_flash） |

## M7-D6..F（验收、电源与硬件证据，2026-08-16）

| 提交 | 里程碑 | 内容 |
| --- | --- | --- |
| `a2f765d` | M7-D6 | UART 扫描验证 + 电源缓解（startScan 修正、PASSIVE/PM80MHz/2dBm） |
| `64cfcb4` | M7-E | OLED wifi-scan suspend/resume + ScanTransaction（shared/wifi） |
| `cf589e8` | M7-E | ScanTransaction 集成 + OLED 扫描挂起 |
| `17b6bfc` | M7-E | Wizard 扫描显示暂停/恢复状态 + i18n |
| `fcdaa50` | M7-E | A/B/C 硬件实验 harness（espview_e_ab_harness.py） |
| `04102b4` | M7-E | DESIGN AJ：power-aware provisioning 设计 |
| `cb8b5b9` | M7-E | 修复 esptool 直调参数顺序（--no-reset，esptool 5.3.1） |
| `56f9dff` | M7-E | 记录 A/B/C 硬件结果（DESIGN AJ.8） |
| `bd892d1` | M7-F F1 | 硬件证据探针：win32_com_probe + boot 插桩 |
| `71217c9` | M7-F F2 | Provisioning 事务硬化（timeout / retry / disconnect 收敛） |
| `456763a` | M7-F F3 | Wizard 硬化 + preview/OLED 修复（15 文件） |
| `5d758a2` | M7-F F4 | Build/Flash UX + per-profile sdkconfig 隔离 |
| `c48efcd` | M7-F F5 | 硬件证据矩阵 + AJ.8 / AI.3 / AG.2 措辞修正（当时 HEAD） |

## M7-G..H 与 M8 系列（2026-08-16..17，已提交）

- M7-G：用户文档集（docs/ 12 份 + 本 changelog）与 README 重构。
- M7-H：文档收尾 + 贡献指南/FAQ。
- M8-A1..A5：benchmark、sanitizer 准备、transport/display/input/OLED 抽象合并、
  硬件验收记录。
- M8-A6：CI / benchmark / sanitizer / build matrix / docs tooling（9 workflows、
  TSan subset、bench_compare、fresh-clone gate）；首跑修复记录见 DESIGN AR.9。
- M8-A7：文档时效 / 资源预算 / target 与 profile 抽象 / S3 准备 / 仓库维护
  （A7-1..A7-10）：docs 审计与用户/开发文档、DESIGN 语义同步、RESOURCE_BUDGET.md、
  target_info 组件 + Kconfig target 参数化、TARGETS/HISTORICAL_PROFILES 拆分、
  sdkconfig.defaults.{esp32,esp32s3} + partitions.esp32s3.csv + build/flash `-t` +
  ci_collect_artifacts `--target`、verify_lvgl ESPIDF_PROFILE、examples/ 9 份、
  check_docs 新 check 11/12/13、fresh-clone target/profile 矩阵校验；
  验证见 DESIGN AS/AT 章（2026-08-18）。
- M8-A8：Direct Wi-Fi Hardware Regression（跳过 provisioning，直接本地未跟踪 profile
  注入 Wi-Fi/TCP 参数）：Wi-Fi STA + TCP + 协议 + LVGL/OLED/Qt 全链路验收 PASS；
  证据见 DESIGN AU 章（2026-08-18）。
- M8-B：FINAL DISPLAY UX + DYNAMIC DISPLAY STATE + ROUTING HARDENING——Qt
  resize/fullscreen/letterbox、动态分辨率三态、DisplayMode 状态机 + 两阶段看门狗 +
  失败回退 VirtualOnly、ACK 可靠投递（pending 单槽）、对端超时双因子、Split Drawer /
  Physical Preview 一致、i18n；真实硬件 UART/TCP 模式矩阵各 32/32 + UART 重连 ×10
  全 PASS；证据见 DESIGN AV 章（2026-08-18）。

## 版本策略说明

- 协议版本 `VERSION=0x01` 冻结：M7 系列全部为 wire additive（新增 TYPE/错误码/
  kSplit=3），不改 Packet Header / CRC / 既有消息 / Frame 语义。
- host 测试规模随里程碑增长：M0 207,900 checks → M8-B 394,892 checks
  （不含 i18n/transport_config 独立套件；实测数字见 DESIGN.md 对应章节）。
