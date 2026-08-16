# ESPView — CI 说明（GitHub Actions）

> 本文档是 ESPView GitHub Actions CI 的说明：分层模型、触发矩阵、环境要求、PR gate、
> 产物与凭据策略、本地模拟与故障排查。协议与架构的**唯一权威**是
> [docs/DESIGN.md](DESIGN.md)；构建/验证入口与本地命令见
> [docs/development.md](development.md)；提交纪律见 [docs/contributing.md](contributing.md)；
> 文档索引见 [docs/README.md](README.md)。
>
> 相关 workflow（由 CI 代理在 `.github/workflows/` 下并行落地）：`fast-ci.yml`、
> `windows-ci.yml`、`esp32-ci.yml`、`docs-security.yml`、`release.yml`、`hardware-smoke.yml`。

## 1. 概览：CI 分层模型

CI 按「速度从快到慢、环境从薄到厚」分四层，外加两条特殊触发通道：

| 层 | Workflow | 内容 | 触发 |
| --- | --- | --- | --- |
| Layer 1 快速宿主 CI | `fast-ci.yml` | ubuntu-latest 上 shared/protocol host 测试（cmake + ctest）；windows-latest 上 MSYS2 MinGW64 跑 `scripts\verify_host.bat`；另有静态检查 job | 每次 PR / push main |
| Layer 2 Windows Qt CI | `windows-ci.yml` | windows-latest + MSYS2 MinGW64 + Qt 6（base / serialport 包），构建 `espview_virtual_display.exe` 并做 offscreen 自关闭冒烟（`--autoclose-ms`，不连硬件） | 按 pc / shared / scripts / docs / README 路径过滤 |
| Layer 3 ESP32 构建 CI | `esp32-ci.yml` | `espressif/idf:v6.0.2` 容器内构建固件（默认 matrix：uart / tcp / diagnostic；手动可触发全部 9 个 profile）；**只 build，绝不 flash** | 按 esp32 / shared / scripts-profile 路径过滤；`workflow_dispatch` 全 9 profile |
| Layer 4 docs + security | `docs-security.yml` | `scripts\check_docs.py` + `scripts\security_scan.py` + `scripts\check_bat_crlf.py` + workflow YAML lint | 每次 PR / push（paths 不过滤，始终运行） |
| Release（tag 通道） | `release.yml` | tag `v*` 触发（可手动），收集固件产物与校验和发布到 GitHub Release | 仅 tag `v*` 或手动 |
| Hardware smoke（手动通道） | `hardware-smoke.yml` | 手动触发的真机冒烟，运行在 self-hosted runner（有硬件环境） | 仅手动 + self-hosted |

- **设计原则**：CI 只做「可复现、无硬件」的验证；真机行为不属于 CI 断言（见 §6 硬件 gate 边界）。
- **CI 不改变本地工作流**：构建/验证入口仍是 `scripts\verify_host.bat`、`scripts\verify_qt.bat`、
  `scripts\verify_lvgl.bat` 与 [docs/development.md](development.md) §3 / §4 的命令；CI 只是自动兜底。

## 2. 触发矩阵

| Workflow | 触发事件 | paths 过滤 | 何时运行 |
| --- | --- | --- | --- |
| `fast-ci` | pull_request（main）+ push（main）+ workflow_dispatch | 无（全仓库） | 每次 PR、每次 main push |
| `windows-ci` | pull_request + push | pc/**、shared/**、scripts/**、docs/**、README.md 等（以 workflow 文件为准） | 命中 pc / shared / scripts / docs / README 路径时 |
| `esp32-ci` | pull_request + push + workflow_dispatch | esp32/**、shared/**、scripts/espview_profile*、scripts/ci_esp32*、examples/**、.github/workflows/esp32-ci.yml | 命中 esp32 / shared / scripts-profile 路径时；手动触发全部 9 个 profile |
| `docs-security` | pull_request + push + workflow_dispatch | 无（始终运行） | 每次 PR、每次 push |
| `release` | push tag `v*` + workflow_dispatch | 无 | 仅 v* tag 或手动 |
| `hardware-smoke` | workflow_dispatch（仅手动） | 无（runs-on self-hosted） | 仅手动，且有 self-hosted runner 在线 |

注：上述为约定值；并行 CI 代理落地时以 `.github/workflows/*.yml` 实际配置为准，行为变化时
本文档同步更新（§12）。

## 3. 环境要求

| 环境 | 提供 | 用途 | 说明 |
| --- | --- | --- | --- |
| ubuntu-latest | g++、cmake、ctest（GitHub-hosted runner 自带） | `fast-ci` host-ubuntu：shared/protocol host 测试 | 无 Qt、无 ESP-IDF 依赖 |
| windows-latest | MSYS2 MinGW64（mingw-w64-x86_64-gcc / cmake / ninja / make）+ Qt 6（base / serialport 包） | `fast-ci` host-windows 跑 `scripts\verify_host.bat`；`windows-ci` Qt 6 构建 + offscreen 冒烟 | MSYS2 PATH 必须前置（见 §10） |
| ESP32 构建 | container `espressif/idf:v6.0.2`（跑在 ubuntu-latest 上） | `esp32-ci`：`idf.py` 构建 9 个 profile | **版本固定 v6.0.2，不漂移**（与本地 ESP-IDF v6.0.2 对齐）；组件走 `esp32/managed_components` 缓存 |

## 4. PR gate

- PR 至少需要 **`fast-ci`、`windows-ci`、`docs-security`** 通过（required checks，见 §5）。
- 命中 esp32-ci 路径过滤的 PR（esp32/**、shared/**、scripts/espview_profile*、
  scripts/ci_esp32*、examples/**）还需 **`esp32-ci`** 通过。
- **host-only PR 不要求 ESP32 硬件 / COM 口 / 真 Wi-Fi**：CI 不 flash、不碰串口、不要求无线；
  真机 gate 是独立的（§6）。无硬件也能提 PR，CI 全绿即满足 gate。
- CI 是自动兜底，不是手工验证的替代品：host 全绿（`scripts\verify_host.bat`）与硬件验收
  （[docs/development.md](development.md) §4.4）的要求不变；不要为了「过 CI」跳过 manual 验收
  （[docs/contributing.md](contributing.md) §4）。

## 5. Main 分支保护建议（需用户手动开启）

- **本仓库不直接修改 GitHub branch protection 设置**——该设置属于仓库级配置，需要仓库管理员
  在 GitHub 网页端（Settings → Branches）手动开启；CI workflow 没有权限、也不应该去改它。
  本文档只给建议，管理员按需采纳。
- 建议配置（`main` 分支）：
  - Required status checks：`fast-ci` / `windows-ci` / `docs-security`（全部 PR）；
    esp32 相关 PR 追加 `esp32-ci`。若 GitHub 端无法按路径区分 required checks，直接全部 PR
    要求四项也可接受（esp32-ci 未命中路径时不会运行，不会阻塞无关 PR——只有真正运行过的
    checks 才会计入 required）。
  - Require branches to be up to date before merging；禁止直接 push `main`（或要求走 PR）。
  - Require PR 通过后再合并；合并策略（squash / merge）由维护者定，不影响 CI。
- Release 与分支保护正交：`release.yml` 由 tag `v*` 推送触发，版本 tag 由维护者打。

## 6. 硬件 gate 边界（CI passed ≠ hardware passed）

- **CI 只 build**：`esp32-ci` 在容器内编译固件、校验产物与生成校验和；**绝不 flash、绝不访问
  COM 口、绝不需要真实 Wi-Fi**。`fast-ci` / `windows-ci` 更是纯 host（Qt 冒烟也是 offscreen）。
- **CI passed ≠ hardware passed**：CI 绿只说明「无硬件的可复现验证」通过；真机行为（UART 握手、
  Wi-Fi/TCP handoff、OLED 物理预览、断电保持等）仍需手工验收（[docs/development.md](development.md)
  §4.4），结果记录在 DESIGN.md 对应章节。
- 真实硬件验证是**手工 gate**（现状）；未来可经 `hardware-smoke.yml`（手动 + self-hosted）在
  有真机的 runner 上跑冒烟——它仍是手动触发通道，不是默认 PR gate。
- **RF-ON / CH340 硬件 blocker（[docs/DESIGN.md](DESIGN.md) AL.3）**：Wi-Fi 扫描期外置 CH340
  掉线，硬件验证受环境阻塞（blocked by hardware/environment）；固件侧 TCP handoff 代码完成且
  `esp32-ci` 构建通过，但真机全链路未闭环。证据按 AL.3 分级，**严禁升级措辞**：
  - **Observed**：掉线发生在 RF-ON 扫描期（HELLO 后 `WIFI_SCAN_REQ` → `esp_wifi_start` PASSIVE
    scan 期间），触发点收窄到扫描期；OLED 从根因排除。
  - **High confidence**：RF 上电与 CH340 掉线强相关（方向与 AK.2 一致）。
  - **Hypothesis（未证实）**：供电余量 / EMI 耦合 / CH340 驱动 / 复位源等物理机制**未直接测量**，
    不得写成「已证实供电不足」。
  - **Unknown**：具体物理机制未知；待稳定供电硬件复测后再定论。
  - 任何文档 / README / CI 徽标文案都**不得声称「Wi-Fi/TCP handoff 已真机验证」**。

## 7. 产物策略（Artifact policy）

`esp32-ci` 与 `release.yml` 的固件产物按以下约定命名（`<profile>` ∈ {uart, tcp, oled,
oled-off, diagnostic, g1_a, g1_b, g1_c, g1_d}；`<sha>` = 触发 commit 短 SHA）：

| 产物 | 命名 |
| --- | --- |
| 固件 | `espview-<profile>-<sha>.bin` |
| bootloader | `bootloader-<profile>-<sha>.bin` |
| partition table | `partition-table-<profile>-<sha>.bin` |
| 校验和 | `SHA256SUMS.txt`（逐行 `sha256sum`，含以上三个文件） |

- **禁止上传**：`esp32/sdkconfig*`（`sdkconfig.defaults` 除外，且它不含凭据）、构建日志
  （`idf_build.log` 等只打印到 Actions 输出，不归档为固件 artifact）、任何 secrets / 凭据文件。
- host 测试日志如由 `fast-ci` / `windows-ci` 归档，必须是脱敏测试输出（不得含凭据或私网地址）。
- **artifact 名称不含 SSID / IP / 密码**：artifact 名在 Actions 列表页公开可见，只允许纯技术名
  （如 `esp32-<profile>`、`host-*-test-logs`）。
- `release.yml` 复用同一命名，把三个 bin + `SHA256SUMS.txt` 附到 GitHub Release。

## 8. 凭据策略（Credentials policy）

- CI 的 workflow、步骤、日志与产物中**永不出现真实 SSID / 密码 / 内网 IP**。本机真实凭据只存在
  未跟踪的 `esp32/sdkconfig`（`.gitignore` 排除）；CI 从 `esp32/sdkconfig.defaults` + profile
  生成构建配置，**从不接触、不读取、不打印本机 sdkconfig 内容**。
- 文档与示例一律用占位符：`127.0.0.1`（loopback）、`192.0.2.0/24`（RFC 5737 文档网段）、
  `<pc-lan-ip>`（真实局域网地址占位，只出现在示例里）。
- `scripts/security_scan.py` 的 allowlist 语义：只放行**占位符语义**的地址——Kconfig 出厂默认的
  示例 IP 与测试向量（它们是 Kconfig 默认值与测试 fixture，不是真实网络地址）；**真实私网地址
  （RFC 1918 网段）永远禁入**，在 scripts / README / docs 中出现即报错，没有例外。
- 因此：PR 里出现占位符之外的网络地址，`docs-security` 层直接红；本地提交前先跑
  `py scripts\security_scan.py`（§9）。

## 9. 本地模拟表

| 检查项 | 本地等价命令 | GitHub-only？ |
| --- | --- | --- |
| host 测试 | `scripts\verify_host.bat` | 否 |
| Qt 构建 + offscreen 冒烟 | `scripts\verify_qt.bat`（冒烟可手动 `espview_virtual_display.exe --autoclose-ms N`） | 否 |
| 文档静态检查 | `scripts\check_docs.bat`（或 `py scripts\check_docs.py`） | 否 |
| 安全扫描 | `py scripts\security_scan.py`（随 CI 一并加入仓库，本地即可跑） | 否 |
| CRLF 检查 | `py scripts\check_bat_crlf.py`（随 CI 一并加入仓库） | 否 |
| workflow YAML 语法 | 本地 `python -c "import yaml; yaml.safe_load(open('.github/workflows/<name>.yml'))"` | 否 |
| ESP32 固件构建 | 本地等价：`scripts\verify_lvgl.bat` 或 `scripts\espview_build.bat -esp32 -b <profile>`（与并行代理串行） | 容器内 `espressif/idf:v6.0.2` 构建本身是 GitHub-only |
| workflow 触发 / PR 状态 | `gh run list` / `gh run view` / `gh run watch` | 是（事件由 GitHub 触发） |
| badge 状态 | — | 是（状态由 GitHub 渲染） |
| release 上传 | — | 是（GitHub Release 仅 GitHub 侧） |

## 10. 故障排查

**先区分「代码失败」与「CI 环境失败」**：

- 构建 / 测试 / 静态检查步骤失败 → 大概率是**代码问题**：本地跑 §9 等价命令应能复现，
  修复代码而不是绕过检查。
- 安装依赖 / 缓存 / runner 超时 / 容器拉取失败 / 找不到 runner → **CI 环境问题**：重跑
  （Re-run jobs）或让 CI 代理修 workflow；不要为了变绿而改代码掩盖。

**常用 `gh` 命令**（GitHub CLI）：

```powershell
gh run list --workflow fast-ci.yml          # 最近运行列表
gh run view <run-id>                        # 详情 + 失败 job/step
gh run watch <run-id>                       # 实时跟随直到结束
gh run list --branch <branch>               # 看某分支上哪些 workflow 被触发
```

**常见失败**：

1. **MSYS2 PATH**：windows 步骤找不到 g++ / cmake → `msys2/setup-msys2` 未安装对应包，或
   `scripts\verify_host.bat` 的 PATH 前置逻辑未生效；本地复现：
   `set PATH=C:\msys64\mingw64\bin;%PATH%`（[docs/development.md](development.md) §8）。
2. **容器缓存**：`esp32-ci` 的 `esp32/managed_components` 缓存 key 变化导致首次构建慢或超时 →
   重跑；镜像固定 v6.0.2，但组件下载仍走网络，冷缓存首次运行会慢。
3. **路径过滤导致未运行**：PR 只改 README 却期望 `esp32-ci` 跑 → 看 §2 触发矩阵；esp32-ci
   只按 esp32 / shared / scripts-profile / examples 路径触发。workflow 没注册（Actions 页看不到）
   则多半是 YAML 语法问题。
4. **Qt 包缺失**：`windows-ci` 构建失败先确认 setup-msys2 是否装了 Qt 6 的 base 与 serialport
   包（`verify_qt.bat` 依赖 Qt Widgets + SerialPort）。

## 11. Badge 说明

README 顶部的四个 badge 对应四个常驻 workflow（默认分支 `main` 最近一次运行的状态）：

| Badge | Workflow | 层 |
| --- | --- | --- |
| fast-ci | `.github/workflows/fast-ci.yml` | Layer 1 |
| windows-ci | `.github/workflows/windows-ci.yml` | Layer 2 |
| esp32-ci | `.github/workflows/esp32-ci.yml` | Layer 3 |
| docs-security | `.github/workflows/docs-security.yml` | Layer 4 |

`release.yml` 与 `hardware-smoke.yml` 非常驻运行（tag / 手动触发），不出 badge。

## 12. 章节 checkpoint 纪律

- 遵循仓库章节式提交惯例（[docs/development.md](development.md) §6、
  [docs/contributing.md](contributing.md) §2）：**每个 CI 章节独立 commit + push + clean**——
  新增/修改 workflow、落地静态检查脚本、更新本文档各自成章；提交后 `git status` 干净再进下一章。
- workflow 行为变化必须与本文档同步（改 `fast-ci.yml` 后 `docs/ci.md` 对应小节一起提交，反之亦然）。
- 并行代理边界：`.github/workflows/` 与静态检查脚本（`security_scan.py`、`check_bat_crlf.py`）
  由 CI 代理独占；`docs/ci.md` 由文档代理维护；交叉改动先沟通再落笔。