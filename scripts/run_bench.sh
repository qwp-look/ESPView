#!/usr/bin/env bash
# ============================================================
# ESPView M8-A6: local benchmark runner (host only, no hardware).
#
#   scripts/run_bench.sh [--quick]
#
# Builds both bench executables under build/bench, runs
# espview_protocol_bench (full, or --quick smoke) and
# espview_m8a4_bench, writes CSVs to build/bench/results/, then
# compares against the committed baselines (full mode only) and
# enforces the stream_encode alloc_count=0 gate.
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build/bench"
QUICK=""
if [[ "${1:-}" == "--quick" ]]; then
  QUICK="--quick"
fi

echo "[1/4] Configure + build bench targets"
cmake -S "$ROOT/shared/protocol" -B "$BUILD/protocol" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD/protocol" --target espview_protocol_bench espview_m8a4_bench -j "$(nproc)"

mkdir -p "$BUILD/results"

echo "[2/4] Run protocol benchmark (${QUICK:-full})"
"$BUILD/protocol/espview_protocol_bench" $QUICK > "$BUILD/results/protocol-bench.csv" 2> "$BUILD/results/protocol-bench-guards.log"

echo "[3/4] Run display/input/OLED benchmark"
"$BUILD/protocol/espview_m8a4_bench" > "$BUILD/results/display-bench.csv" 2> "$BUILD/results/display-bench-guards.log"

echo "[4/4] Compare against committed baselines"
if [[ -z "$QUICK" ]]; then
  python3 "$ROOT/scripts/bench_compare.py" \
    "$ROOT/shared/protocol/bench/results/m8a1_baseline.csv" \
    "$BUILD/results/protocol-bench.csv"
  python3 "$ROOT/scripts/bench_compare.py" \
    "$ROOT/shared/bench/results/m8a4_baseline.csv" \
    "$BUILD/results/display-bench.csv"
else
  python3 "$ROOT/scripts/bench_compare.py" --warn-only \
    "$ROOT/shared/protocol/bench/results/m8a1_baseline.csv" \
    "$BUILD/results/protocol-bench.csv"
fi

echo "run_bench: ALL PASS (results in $BUILD/results/)"
