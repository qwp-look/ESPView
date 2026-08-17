# ESPView — 资源预算（RESOURCE_BUDGET）

> 本章回答「ESP32 的资源到底花在哪里」，供未来 ESP32-S3 移植与内存审计使用。
> 每个数字尽量带来源；没有实测的数字明确写 **unmeasured / requires hardware measurement**，不编造。
> 协议、架构与验收记录的唯一权威是 [DESIGN.md](DESIGN.md)；本文件是 DESIGN 的资源视角摘要，不承载协议规范。

## 1. 范围与口径

- 芯片：ESP32-D0WDQ6（classic ESP32，2 核 Xtensa LX6，无 PSRAM）。
- Flash：4 MiB；分区见 `esp32/partitions.csv`（M6-B 冻结，DESIGN U 章 / X.16）。
- 固件尺寸口径：`esp32/build/<profile>/espview_esp32.bin` 文件大小（本机构建产物，2026-08-17/18 期间）；
  历史尺寸注明里程碑与来源。
- RAM 口径：静态内存取自 `espview_esp32.map` section 尺寸；堆数字取自 DESIGN 实测记录。

## 2. Flash

### 2.1 分区布局（esp32/partitions.csv，M6-B 冻结）

| 分区 | 偏移 | 大小 | 说明 |
|------|------|------|------|
| bootloader | 0x1000（ESP-IDF 默认区域） | 32 KiB 区域；bootloader.bin 实测 26,272 B | ESP-IDF 引导（esp32/build/bootloader/bootloader.bin） |
| nvs | 0x9000 | 0x6000 = 24 KiB | Wi-Fi / NVS 校准数据（保持历史偏移） |
| phy_init | 0xf000 | 0x1000 = 4 KiB | PHY 校准数据 |
| factory | 0x10000 | 0x200000 = 2 MiB | 唯一 app 分区，**无 OTA**（partitions.csv；DESIGN U/X.16） |

来源：`esp32/partitions.csv`；bootloader.bin 大小 = 本机 build 产物（esp32/build/bootloader/bootloader.bin）。

### 2.2 各 profile 固件大小（esp32/build/<profile>/espview_esp32.bin）

| profile | 大小 | 来源 / 日期 |
|---------|------|-------------|
| uart | 1,134,624 B | 本机 build（2026-08-17；README 引用值） |
| tcp | 1,133,408 B | 本机 build（esp32/build 默认构建） |
| oled | 1,133,424 B | 本机 build |
| oled-off | 1,096,896 B | 本机 build（目录名 oled-off） |
| diagnostic | 923,280 B | 本机 build |
| g1_a / g1_c（历史 harness） | 1,091,056 B | 本机 build（保留证据；A7-6 落地后移出一等 CI 与默认矩阵，见 DESIGN AS.2 D9） |
| g1_b / g1_d（历史 harness） | 1,127,248 B | 本机 build（保留证据） |
| M6-B 历史固件 | 1,027,488 B | DESIGN U.2（2026-08-14 实测，4 MiB / 2 MiB 扩容后首版） |
| ESP32-S3 compile smoke | 1,096,224 B | DESIGN AR.6（2026-08-17 本机 set-target esp32s3 编译产物） |

- factory app 容量 2 MiB（2,097,152 B）；uart 固件占用后余量 ≈ 962,528 B（≈46% headroom）。
- **无 OTA**：分区表没有 ota_0 / ota_1（partitions.csv；DESIGN X.16）。
- 未分配 data 空间：除上述 bootloader/nvs/phy_init/factory 外无其他分区。

### 2.3 ELF section 尺寸（esp32/build/espview_esp32.map，TCP profile 默认构建，2026-08-17/18）

| section | 大小 | 说明 |
|---------|------|------|
| .flash.text | 869,594 B | 可执行代码（flash 映射） |
| .flash.rodata | 151,396 B | 只读数据（字符串 / 常量） |
| .dram0.data | 17,955 B | 已初始化 RAM 数据 |
| .dram0.bss | 77,096 B | 零初始化 RAM（tcp）；uart 77,144 B / oled-off 77,056 B / diagnostic 27,744 B（对应 profile map） |
| .iram0.text | 92,879 B | 常驻 IRAM 代码（中断 / 时序关键路径） |

## 3. RAM（静态分配）

| 项 | 大小 | 位置 / 来源 |
|----|------|-------------|
| LVGL draw buffer | 15,360 B（320×24 = 1/10 屏，静态 .bss） | `esp32/components/lvgl_port/src/lvgl_port.cpp:34-36` |
| LVGL 堆 | 32 KiB | `CONFIG_LV_MEM_SIZE_KILOBYTES=32`（esp32/sdkconfig:3716） |
| RemoteDisplay staging | 2 × 15,360 B = 30,720 B（2 槽，packet-sized） | `shared/display/remote_display.h:83` |
| OLED framebuffer | 1,024 B（128×64 1bpp，8 pages × 128 B） | `shared/oled/oled_fb.h:1` |
| UART RX / TX ring | 各 8,192 B | `CONFIG_ESPVIEW_UART_RX_BUF` / `CONFIG_ESPVIEW_UART_TX_BUF=8192`（esp32/sdkconfig:3638-3640） |
| 协议 packet staging | ≤ 4,096 B（MAX_PACKET_PAYLOAD，共享缓冲 / 流式） | DESIGN E 节 / M1-3C（无 153,600 B 整帧连续分配） |

- **ESPView 不持有整屏 framebuffer**：像素所有权在 Application / LVGL（RGB565 权威画面）；
  RemoteDisplay 只经有界 staging 槽转发（remote_display.h:8-10；DESIGN M5-A / J 节冻结）。
- Input manager / diagnostics：有界小结构（InputEvent 固定布局、RuntimeStats 计数、
  DiagnosticsRing 有界环形），无大缓冲（shared/input、shared/display；DESIGN M4）。
- 协议热路径零堆分配：M8-A1 流式 Encoder 热路径 alloc_count=0（DESIGN AN 章 / AR.8）。

## 4. Heap（实测记录）

| 指标 | 值 | 来源 |
|------|----|------|
| 稳定 free heap（长稳 hb/ha） | 231,352 B / 231,352 B | DESIGN X.13（M6-E 长稳唯一二元组） |
| min-free watermark | 121,236 B（起始）→ 97,356 B（首帧 FULL 后恒定） | DESIGN X.13 |
| 最大空闲块 | 110,592 B | DESIGN M 表 M5-A 验收记录（真实硬件；largest=110592B） |
| 首帧 FULL 附近瞬时分配 | LVGL flush 区域 + staging 队列（见 §3） | DESIGN X.13（一次降至 97,356 B 后不再下降，非泄漏） |
| startup free heap | **unmeasured**（DESIGN 未记录启动前堆快照；需串口日志量测） | — |

## 5. CPU / 任务

| 任务 | 栈（字） | 优先级 | 来源 |
|------|---------|--------|------|
| espview_sess（会话 / 心跳） | 4,096 | 5 | esp32/main/main.cpp:1405 |
| espview_stat（统计） | 4,096 | 3 | esp32/main/main.cpp:1406 |
| lvgl_ui | 4,096 | 4 | lvgl_port.cpp:192 |
| lvgl_tx | 4,096 | 4 | lvgl_port.cpp:193 |
| espview_uart_rx | 4,096 | 5 | uart_transport.cpp:146 |
| espview_tcp_link | 4,096 | 6 | tcp_transport.cpp:120 |
| espview_tcp_rx | 4,096 | 7 | tcp_transport.cpp:648 |
| espview_tp（TestPattern） | 4,096 | 4 | test_pattern.cpp:116 |
| espview_oled | 4,096（CONFIG_ESPVIEW_OLED_TASK_STACK） | 2（CONFIG_ESPVIEW_OLED_TASK_PRIORITY） | oled_display.cpp / esp32/sdkconfig:3670-3672 |
| esp_event（Wi-Fi 事件，系统默认任务） | 2,304 B 栈 = 576 字（CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE） | 20（ESP-IDF ESP_TASKD_EVENT_PRIO = configMAX_PRIORITIES−5；ESP32=25→20） | esp-idf esp_task.h；esp32/sdkconfig:1989 |

- **CPU 利用率：unmeasured / requires hardware measurement。**
  `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` 未启用（esp32/sdkconfig:2389），无 runtime stats；
  需启用后配合串口诊断输出实测各任务占用（DESIGN AR.8 未包含 CPU 数据）。

## 6. ESP32-S3 预留（仅 build / 配置层，M8-A7 边界）

- 本文件为 S3 提供资源口径，但 **S3 N16R8 尚未到货、无硬件测量**（DESIGN AR.6 仅 compile smoke）。
- 已知差异预留（DESIGN AS.2 D7/D8/D10 → A7-5..A7-7）：Kconfig GPIO range 参数化、新增
  `sdkconfig.defaults.esp32s3`（UART0 引脚 / flash 尺寸按目标配置，A7-7 落地）、flash / partition
  不再强制 4 MiB / 2 MiB 布局。S3 N16R8 的 UART0 默认引脚与 16 MiB flash 为芯片规格事实，
  等 A7-7 的 sdkconfig.defaults.esp32s3 落地后在本文 §2 补充实测来源。
- 内存策略保持现状：**不因 S3 有 PSRAM 就迁移大缓冲**；默认保持经典 ESP32 行为
  （M8-A7 任务书 §21：PSRAM allocation policy 仅预留，默认不变）。

## 7. 测量 Gate（禁止编造）

- 本文件每个数字都来自：`esp32/build/*` 构建产物、`esp32/partitions.csv`、`esp32/sdkconfig`、
  `shared/*` / `esp32/components/*` 代码、DESIGN.md（U.2 / X.13 / M 表 / AR.6 / AR.8）。
- 未知或未测量项明确写 `unmeasured / requires hardware measurement`，例如：startup free heap、
  CPU 利用率（runtime stats 未启用）。
- 新数字写入前必须注明来源；不得把理论值写成实测（M8-A7 任务书 §40 资源预算 Gate）。
