# Example: Benchmark（纯 host）

> **Hardware required: 无**（仅需 MSYS2 MinGW64 + CMake）。

```bat
scripts\run_bench.bat --quick    :: 快速冒烟（protocol bench quick + display/input/OLED bench）
scripts\run_bench.bat            :: 完整模式：对比提交的基线 + stream_encode alloc_count=0 gate
```

- 结果 CSV 输出到 `build\bench\results\`（`protocol-bench.csv` / `display-bench.csv`）。
- 完整模式会调用 `scripts\bench_compare.py` 与 `shared/protocol/bench/results/` 下的
  提交基线对比，超限即失败。
- 期望：两条 bench 均退出码 0；完整模式基线对比 PASS。

参考：`scripts/README.md`（benchmark 章节）、`docs/testing.md`
