#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView M8-A6 -- benchmark CSV compare + non-regression gates.

Compares a fresh benchmark CSV against a committed baseline CSV and enforces
the allocation non-regression gate.

Usage:
    python3 scripts/bench_compare.py <baseline.csv> <current.csv> [options]

Options:
    --threshold <ratio>  regression alert threshold, default 0.25 (+25%).
                         Major regressions only (task book section 18: never
                         a strict +/-1% gate on noisy GitHub runners).
    --alloc-zero-ops <op[,op...]>
                         Ops whose alloc_count must stay 0, default stream_encode.
    --warn-only          report regressions but exit 0 (used by non-gating jobs).

Behavior:
  * Header-aware CSV parsing (both protocol and display bench schemas).
  * Median rows (trial == 5) are compared per (op, payload_bytes) key using
    elapsed_us_per_op; ratio = current/baseline - 1.
  * Every key with ratio > threshold is printed with baseline/current/ratio.
  * Missing keys in baseline are informational ("new key"); keys missing in
    current are informational ("missing key").
  * Alloc gate: for each alloc-zero op, every row of that op must have
    alloc_count == 0; failure exits 1 (streaming zero-alloc gate, section 19).
  * Exit codes: 0 clean, 1 regressions/alloc-gate failure, 2 usage/IO error.
"""
from __future__ import annotations

import argparse
import csv
import os
import sys


def load_rows(path):
    """Return (header, rows) with header = list of column names."""
    with open(path, "r", encoding="utf-8", errors="replace", newline="") as fh:
        reader = csv.reader(fh)
        header = next(reader)
        rows = list(reader)
    return header, rows


def col_index(header, name):
    for i, h in enumerate(header):
        if h.strip() == name:
            return i
    return None


def build_median_map(path):
    header, rows = load_rows(path)
    idx_op = col_index(header, "op")
    idx_payload = col_index(header, "payload_bytes")
    idx_trial = col_index(header, "trial")
    idx_per_op = col_index(header, "elapsed_us_per_op")
    idx_alloc = col_index(header, "alloc_count")
    if None in (idx_op, idx_payload, idx_trial, idx_per_op, idx_alloc):
        raise ValueError("CSV missing required columns: %s" % (path,))
    median = {}
    for row in rows:
        if len(row) <= max(idx_op, idx_payload, idx_trial, idx_per_op, idx_alloc):
            continue
        if row[idx_trial].strip() != "5":  # median row marker
            continue
        key = (row[idx_op].strip(), row[idx_payload].strip())
        try:
            per_op = float(row[idx_per_op])
        except ValueError:
            continue
        median[key] = (per_op, int(row[idx_alloc] or 0))
    return median


def alloc_zero_failures(path, alloc_zero_ops):
    """Return list of (op, payload, trial, alloc_count) rows that violate the gate."""
    header, rows = load_rows(path)
    idx_op = col_index(header, "op")
    idx_payload = col_index(header, "payload_bytes")
    idx_trial = col_index(header, "trial")
    idx_alloc = col_index(header, "alloc_count")
    if None in (idx_op, idx_payload, idx_trial, idx_alloc):
        return []
    bad = []
    for row in rows:
        if len(row) <= max(idx_op, idx_payload, idx_trial, idx_alloc):
            continue
        op = row[idx_op].strip()
        if op not in alloc_zero_ops:
            continue
        try:
            alloc = int(row[idx_alloc] or 0)
        except ValueError:
            alloc = -1
        if alloc != 0:
            bad.append((op, row[idx_payload].strip(), row[idx_trial].strip(), alloc))
    return bad


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument("--threshold", type=float, default=0.25)
    parser.add_argument("--alloc-zero-ops", default="stream_encode")
    parser.add_argument("--warn-only", action="store_true")
    args = parser.parse_args()

    if not os.path.isfile(args.baseline):
        print("bench_compare: baseline missing: %s" % args.baseline, file=sys.stderr)
        return 2
    if not os.path.isfile(args.current):
        print("bench_compare: current CSV missing: %s" % args.current, file=sys.stderr)
        return 2

    alloc_zero_ops = {op.strip() for op in args.alloc_zero_ops.split(",") if op.strip()}

    try:
        base = build_median_map(args.baseline)
        curr = build_median_map(args.current)
    except (ValueError, StopIteration) as exc:
        print("bench_compare: CSV parse error: %s" % exc, file=sys.stderr)
        return 2

    regressions = []
    info = []
    for key, (cur_per_op, _cur_alloc) in sorted(curr.items()):
        if key not in base:
            info.append("new key: %s/%s" % key)
            continue
        base_per_op, _base_alloc = base[key]
        if base_per_op <= 0.0:
            continue
        ratio = (cur_per_op - base_per_op) / base_per_op
        if ratio > args.threshold:
            regressions.append((key, base_per_op, cur_per_op, ratio))
    for key in sorted(base):
        if key not in curr:
            info.append("missing key: %s/%s" % key)

    print("bench_compare: baseline=%s current=%s threshold=+%.0f%%"
          % (os.path.basename(args.baseline), os.path.basename(args.current),
             args.threshold * 100.0))
    if info:
        for line in info:
            print("  info: %s" % line)
    if regressions:
        print("BENCH REGRESSIONS (%d):" % len(regressions))
        for (op, payload), base_us, cur_us, ratio in regressions:
            print("  %-16s payload=%-8s baseline=%.3fus current=%.3fus ratio=+%.1f%%"
                  % (op, payload, base_us, cur_us, ratio * 100.0))
    else:
        print("bench_compare: no regressions > threshold")

    alloc_bad = alloc_zero_failures(args.current, alloc_zero_ops)
    if alloc_bad:
        print("ALLOC GATE FAIL (alloc_count must be 0 for %s):"
              % ", ".join(sorted(alloc_zero_ops)))
        for op, payload, trial, alloc in alloc_bad:
            print("  %s payload=%s trial=%s alloc_count=%d"
                  % (op, payload, trial, alloc))
    else:
        print("alloc gate OK: %s alloc_count == 0"
              % ", ".join(sorted(alloc_zero_ops)))

    if alloc_bad:
        return 1
    if regressions and not args.warn_only:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
