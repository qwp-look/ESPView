#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView GitHub Actions .bat line-ending checker.

For every git-tracked *.bat file (git ls-files '*.bat'):
  - any bare LF byte (0x0A not immediately preceded by CR 0x0D) is a
    violation (expected CRLF line endings);
  - a UTF-8 BOM (EF BB BF) is a violation (many cmd.exe versions
    mis-handle BOM-prefixed .bat files).

Report-only: files are never rewritten.

Output: "check_bat_crlf: OK (N .bat files, all CRLF)" when clean;
"file: violation: ..." lines otherwise.

Exit: 0 = OK, 1 = violations found, 2 = usage/IO error.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

UTF8_BOM = b"\xef\xbb\xbf"
CR = 0x0D
LF = 0x0A


def tracked_bat_files():
    try:
        proc = subprocess.run(
            ["git", "-C", REPO_ROOT, "ls-files", "*.bat"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except Exception as exc:
        raise IOError("git ls-files '*.bat' failed: %s" % exc)
    if proc.returncode != 0:
        raise IOError("git ls-files '*.bat' exited %d: %s"
                      % (proc.returncode,
                         proc.stderr.decode("utf-8", "replace").strip()))
    text = proc.stdout.decode("utf-8", "replace")
    return [p for p in text.splitlines() if p]


def check_file(rel):
    path = os.path.join(REPO_ROOT, rel.replace("/", os.sep))
    if not os.path.isfile(path):
        raise IOError("tracked file missing from working tree: %s" % rel)
    try:
        with open(path, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        raise IOError("cannot read %s: %s" % (rel, exc))
    violations = []
    if data.startswith(UTF8_BOM):
        violations.append(
            "%s: violation: UTF-8 BOM at offset 0 (cmd.exe may mis-handle BOM)"
            % rel)
    for i, byte in enumerate(data):
        if byte == LF and (i == 0 or data[i - 1] != CR):
            violations.append(
                "%s: violation: bare-LF at offset %d (expected CRLF)"
                % (rel, i))
    return violations


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="check_bat_crlf",
        description="Check tracked .bat files for CRLF line endings and no UTF-8 BOM.")
    parser.parse_args(argv)
    try:
        bats = tracked_bat_files()
        violations = []
        for rel in bats:
            violations.extend(check_file(rel))
    except IOError as exc:
        print("check_bat_crlf: error: %s" % exc, file=sys.stderr)
        return 2
    for v in violations:
        print(v)
    if violations:
        print("check_bat_crlf: %d violation(s) in %d .bat file(s)"
              % (len(violations), len(bats)))
        return 1
    print("check_bat_crlf: OK (%d .bat files, all CRLF)" % len(bats))
    return 0


if __name__ == "__main__":
    sys.exit(main())
