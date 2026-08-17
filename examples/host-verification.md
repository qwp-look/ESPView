# Example: Host verification（无硬件）

> **Hardware required: 无**（仅需 MSYS2 MinGW64 + CMake；Qt GUI 构建可选，不需要 COM 口）。

## 1. Host 单测 + ctest + pc 工具

```bat
scripts\verify_host.bat
```

期望：末尾 `verify_host: ALL PASS`，退出码 0（协议套件 + `transport_config_test` +
`com3_frame_test --selftest-queue` + TCP loopback 127.0.0.1 全部 0 failures；
数值基线见 `docs/testing.md`）。

## 2. Qt GUI 构建检查（只构建，不运行）

```bat
scripts\verify_qt.bat
```

期望：`verify_qt: ALL PASS (espview_virtual_display.exe)`。

## 3. 也可以直接跑 benchmark

见 [benchmark.md](benchmark.md)（同一工具链，`scripts\run_bench.bat`）。

参考：`docs/testing.md`、`docs/development.md`、`scripts/README.md`
