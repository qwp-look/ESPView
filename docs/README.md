# ESPView — 文档索引（docs/）

> **唯一权威**：协议、架构与验收记录的**唯一权威**是 [docs/DESIGN.md](DESIGN.md)。
> 其他所有页面都是它的导览或补充；内容冲突时以 DESIGN.md 为准（里程碑修订沿修订记录块追加，
> 不重写历史章节）。本文档只是索引，不承载规范。

## 文档列表

| 文档 | 一句话说明 |
| --- | --- |
| [DESIGN.md](DESIGN.md) | **唯一权威**：冻结的协议规范（Packet/Message/Frame）、架构、里程碑证据与验收记录（M0–M7-G） |
| [README.md](README.md) | 本页：docs 目录索引 |
| [ci.md](ci.md) | GitHub Actions CI：分层模型、触发矩阵、PR gate、产物/凭据策略与故障排查 |
| [architecture-overview.md](architecture-overview.md) | 高层架构速览（显示权威、双传输、输入逆链路） |
| [getting-started.md](getting-started.md) | 从零到跑通：构建、烧录、连接、四种显示模式 |
| [hardware.md](hardware.md) | 硬件要求与接线（ESP32 板、CH340、OLED I2C） |
| [uart.md](uart.md) | UART 链路：115200 8N1 baseline、端口与波特率 |
| [wifi.md](wifi.md) | Wi-Fi / TCP 模式与 provisioning（凭据 RAM-only） |
| [display-modes.md](display-modes.md) | VirtualOnly / PhysicalOnly / Mirror / Split 四种显示模式 |
| [oled.md](oled.md) | SSD1306 / SH1106 OLED：物理预览与诊断 |
| [input.md](input.md) | 输入逆链路（INPUT_KEY / INPUT_MOUSE 到 LVGL） |
| [troubleshooting.md](troubleshooting.md) | 常见故障排查：COM 掉线、RF-ON/CH340 阻塞、构建环境坑 |
| [testing.md](testing.md) | 测试分层与验证矩阵（host / Qt / ESP32 / 硬件 manual） |
| [development.md](development.md) | 开发者文档：构建环境、真实构建命令、profile 系统、CI 说明 |
| [contributing.md](contributing.md) | 贡献指南：分支/提交规范、测试要求、凭据红线、CI / PR gate |
| [security.md](security.md) | 凭据与安全策略（RAM-only、威胁模型、日志纪律） |
| [changelog.md](changelog.md) | 按里程碑的变更记录 |
| [faq.md](faq.md) | 常见问题汇总 |
| [scripts/README.md](../scripts/README.md) | 构建/烧录/验证脚本与硬件 harness 参考（位于 scripts/，一并收录到索引） |

## 阅读路径

- 新读者：`getting-started.md` → `architecture-overview.md` → DESIGN.md（按需章节）。
- 开发者：`development.md` → `contributing.md` → `ci.md`（CI gate 与本地模拟）。
- 协议 / 架构问题：直接查 [DESIGN.md](DESIGN.md)（唯一权威）。