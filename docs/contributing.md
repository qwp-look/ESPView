# ESPView — 贡献指南（Contributing）

> 本文档说明如何为 ESPView 仓库贡献代码、测试与文档。协议与架构的**唯一权威**是
> [docs/DESIGN.md](DESIGN.md)；开发环境与真实构建命令见
> [docs/development.md](development.md)；脚本用法以 [scripts/README.md](../scripts/README.md)
> 最新为准；文档索引见 [docs/README.md](README.md)，项目总览见 [../README.md](../README.md)。

## 1. 获取代码（Fork / Clone）

1. 在 GitHub 上 fork 上游仓库（`https://github.com/qwp-look/ESPView`）。
2. clone 你的 fork 并添加上游 remote：

```powershell
git clone https://github.com/<your-account>/ESPView.git
cd ESPView
git remote add upstream https://github.com/qwp-look/ESPView.git
git fetch upstream
```

3. 基于最新的 `upstream/main` 建分支（见 §2）。

仓库当前默认分支为 `main`（集成主干，保持可构建、可测试；HEAD 为章节式提交，以 `git rev-parse HEAD` 为准）。

## 2. 分支约定

- **`main` 只接受已通过验证的章节提交**：每个里程碑/章节完成后独立
  commit + push + clean（提交后 `git status` 干净）。
- 功能/修复/文档分支建议命名 `codex/<chapter>-<slug>`（并行代理默认使用 `codex/` 前缀），
  例如 `codex/m7-g8-docs`、`codex/m7-h-fix-wizard`。
- 一次分支只做一件事（一个章节或一个修复），保持与上游同步：

```powershell
git fetch upstream
git switch -c codex/m7-g8-docs upstream/main
```

## 3. 提交信息规范

格式：`<type>(<scope>): <章节> <一句话描述>`，章节（如 `M7-D3`、`M7-F F4`）帮助追溯里程碑。

- `feat:` 新功能（协议、UI、固件、工具链）。示例：`feat(protocol): M7-D3 wifi provisioning`
- `fix:` 缺陷修复。示例：`fix(scripts): M7-F F4 build/flash UX + profile sdkconfig isolation`
- `docs:` 文档/设计修订。示例：`docs: M7-F F5 hardware evidence matrix`
- `test:` 仅测试/验收（通常并入 feat/fix 提交，独立时用）。

规则：

- 一个提交只包含一个逻辑变更；协议/固件/UI/工具链变更与对应文档、测试放同一章节提交。
- 正文（可选）说明动机与验证方式（host checks 数量、硬件配置、命令）。
- 提交前用 `git status` 确认没有误入 `esp32/sdkconfig*`、构建产物（`build/`、`esp32/build/`、
  `pc/build*/`、`shared/*/build*/`、`__pycache__/`）或本地凭据（§6）。
- 不要重写已推送的历史（章节提交已 push 后保持线性追加）。

## 4. 测试要求

提交前必须通过与你改动相关的测试层（见 [development.md](development.md) §4）：

- **Host（强制，最小门槛）**：`scripts\verify_host.bat`——协议套件 ctest（20 万+ checks）+
  `com3_frame_test --selftest-queue` + TCP loopback + transport config。任何 shared/pc 改动
  都必须全绿；新增逻辑应在对应 `shared/*/tests/` 或 `pc/src/*_test.cpp` 补 host 单测。
- **Qt（涉及 GUI 时）**：`scripts\verify_qt.bat`（构建 `espview_virtual_display.exe`）。
- **ESP32（涉及固件时）**：`scripts\verify_lvgl.bat`（host + `idf.py build`）；有真机时
  加 `set ESPVIEW_COM3=COM4` 跑 LVGL sanity。
- **硬件验收（manual，有真机时）**：COM 帧管线全模式、Wi-Fi/TCP、OLED 探针（命令见
  development.md §4.4）；结果必须记录到 DESIGN.md 对应章节（含日期、硬件配置、证据文件）。
- 硬件测试不进入普通 ctest；不要为了"过 CI"跳过必须的 manual 验收。

## 5. 文档要求

- **DESIGN.md 是唯一权威**：wire format 与已冻结语义（Packet Header、CRC32、HELLO、SET_MODE、
  Frame、消息表）**禁止在未先更新 DESIGN.md 的情况下修改**；新增消息/字段必须是 additive
  扩展并先在 DESIGN.md 定稿（参考 AD/AE/AF/AJ 各节的做法）。
- 文档随章节提交：协议/架构变更、实测数据、新工具脚本都要在同一章节提交里同步
  `docs/DESIGN.md` 修订记录（时间戳块，不重写历史章节）。
- Markdown 用 LF 行尾；文档引用用仓库内相对链接（如 `docs/DESIGN.md`、`scripts/README.md`）。
## 6. 凭据纪律（安全红线）

- **Wi-Fi SSID / 密码只存在于本机未跟踪的 `esp32/sdkconfig`**（`.gitignore` 已排除
  `esp32/sdkconfig` 与 `esp32/sdkconfig.*`，保留 `sdkconfig.defaults`）。**任何情况下都不得
  提交它们**；提交前检查 `git status` 不出现 `esp32/sdkconfig*`（`sdkconfig.defaults` 除外）。
- 默认 **RAM-only**（DESIGN.md AF.4）：密码断电即失；PC 侧只经对话框输入（**禁止 CLI 参数
  传密码**），ACK 成功或耗尽后 secureErase，不持久化；SSID 属非秘密 metadata，可选保存，
  默认不保存。
- 密码永不进入：日志 / ERROR 文本遥测 / RuntimeStats / 诊断行 / QSettings（白名单键
  结构性保证）/ DESIGN.md / git / PNG dump / UI（只显示 SSID 长度）。
- 明文风险：SSID/password 经 UART（及未来 TCP）为明文载荷（CRC32 仅完整性非加密）；本仓库
  只做**局域网开发工具**用途，不宣称安全 provisioning、不实现 TLS。UART bootstrap 配网完成
  后建议断开物理通道；威胁模型限于可信开发局域网。
- 示例一律用占位符（如 `YOUR_SSID` / `YOUR_PASSWORD` / `COM4`），禁止真实凭据进文档、issue、
  PR 描述或日志附件（日志先脱敏再贴）。
- 工具链脚本（`espview_build.bat` / `espview_flash.bat` / `espview_e_ab_harness.py`）**从不
  读取或打印 `esp32\sdkconfig`**；新脚本沿用这一约束，不得把凭据写入文件。

## 7. 报告问题 / 提 PR

### 报告问题（Issue）

请包含以下信息（硬件类问题务必完整，缺项会被退回补充）：

- 章节/组件：涉及 DESIGN.md 哪一节（如 M7-E AJ 节）、哪个脚本/目标。
- 环境：Windows 版本、MSYS2/Qt/ESP-IDF 版本、固件 profile 与构建目录。
- 硬件（如适用）：板型（ESP32-D0WDQ6）、CH340 端口（COMx）、波特率、OLED/Wi-Fi 状态。
- 复现步骤 + 期望/实际行为。
- 证据：host checks 数量与失败项、sanitized 日志（**去掉 SSID/密码**）、DESIGN.md 已有
  验收记录对照；优先贴 `verify_host.bat` / `verify_qt.bat` / `verify_lvgl.bat` 输出与
  退出码（约定见 scripts/README.md AH 节）。

### 提 PR

- 目标分支：`main`（上游）。一个 PR 一个章节/修复，标题与提交信息同格式
  （`feat(scope): 章节 描述`）。
- PR 描述包含：变更文件清单（应只含该章节文件）、验证命令与结果、DESIGN.md 章节引用、
  硬件验收记录（如有）。
- 检查清单：host 全绿（`verify_host.bat`）✓、涉及 GUI/固件时对应构建过 ✓、无凭据/构建产物
  入库 ✓、wire format 变更已先更新 DESIGN.md ✓、Markdown LF ✓。
- CI gate：见 §9（fast-ci / windows-ci / docs-security，esp32 相关 PR 加 esp32-ci）。
- 不要在你的 PR 里夹带并行代理独占的文件（如其他代理正在维护的 `scripts/README.md`、
  `README.md`、docs 其他页面）——如需协作改动，先在 issue/任务里声明。

## 8. 与并行代理协作（ESPView 多人/多代理工作流）

- 仓库由主代理协调多个并行子代理：每个代理只修改自己独占的文件清单，其他文件只读。
- 改动前先读最新版 DESIGN.md 相关章节与 `scripts/README.md`（脚本代理可能正在重构）。
- ESP32 构建/烧录串行执行（`esp32/build/` 共享），避免与并行代理同时构建；用
  `scripts\espview_build.bat --check` 先做 preflight。
- 章节完成后立即独立 commit + push + clean，方便主代理合并与下一章衔接。
- 争议点（协议语义、profile 命名、文档结构）以主代理裁决与 DESIGN.md / scripts/README.md
  最新状态为准。

## 9. CI / PR gate（GitHub Actions）

仓库已接入 GitHub Actions CI，分层与触发矩阵详见 [docs/ci.md](ci.md)。CI 是**自动兜底**，
**不是手工验证的替代品**；硬件验收仍是 manual gate（§4）。

**提交 PR 前，至少在本机执行**（与改动相关的等价命令）：

- `scripts\verify_host.bat` —— host 协议套件（shared/pc 改动必须全绿）
- `scripts\verify_qt.bat` —— 涉及 Qt GUI 目标时
- `py scripts\check_docs.py` —— 文档静态检查（引用 / 禁词 / 凭据模式）
- `py scripts\security_scan.py` —— 凭据与私网地址扫描（随 CI 一并加入仓库，本地即可跑）

**CI 自动运行**：

- `fast-ci`：ubuntu + Windows MSYS2 host 测试 + 静态检查；
- `windows-ci`：Qt 6 构建 + offscreen 自关闭冒烟（不连硬件）；
- `docs-security`：check_docs / security_scan / check_bat_crlf / YAML lint，始终运行；
- 命中 esp32 相关路径的 PR 追加 `esp32-ci`（`espressif/idf:v6.0.2` 容器内 9 个 profile 构建，
  绝不 flash）。

**host-only PR 不需要 ESP32 / CH340 / 真 Wi-Fi**：不要求 COM 口、不要求硬件；CI passed ≠
hardware passed（RF-ON / CH340 硬件阻塞见 [docs/DESIGN.md](DESIGN.md) AL.3，真机 handoff 未闭环，
不得宣称已验证）。CI 失败先按 [docs/ci.md](ci.md) §10 区分代码失败与环境失败，再决定修复
代码还是重跑。
