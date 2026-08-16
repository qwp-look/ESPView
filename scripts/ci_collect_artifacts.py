#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView CI release artifact collector (pure Python 3, no third-party deps).

Collects the three ESP32 firmware binaries for one or more build profiles
from arbitrary source locations, copies them into a flat release directory
under release-safe names, and writes a SHA256SUMS.txt checksum manifest in
sha256sum(1) format covering every file it produced.

Interface (see --help):
    python3 scripts/ci_collect_artifacts.py \
        --sha <commit-sha> --tag <release-tag> --out <dir> \
        --bin "uart:path/to/espview_esp32.bin" \
        --bin "uart:path/to/bootloader.bin" \
        --bin "uart:path/to/partition-table.bin" \
        ...

    Entries may also be supplied through --manifest <file>, a plain text
    file with one "profile:path" entry per line (blank lines and lines
    starting with '#' are ignored).

Naming (short-sha = first 8 characters of --sha):
    espview_esp32.bin      -> espview-<profile>-<short-sha>.bin
    bootloader.bin         -> bootloader-<profile>-<short-sha>.bin
    partition-table.bin    -> partition-table-<profile>-<short-sha>.bin

Security red lines (never violated):
    * Refuses any path whose text contains 'sdkconfig' or 'logs'
      (case-insensitive). The gitignored esp32/sdkconfig may hold Wi-Fi
      credentials; logs are build noise -- neither may ever be packaged.
    * Never opens, reads, copies, lists or otherwise touches log files.
    * Writes nothing but the renamed firmware copies and SHA256SUMS.txt.

Exit codes:
    0  success
    1  packaging failure (missing/unreadable file, policy refusal, IO error)
    2  usage error (bad flags, missing required arguments, malformed
       entry, unknown bin type)
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import sys

# Accepted firmware bin filenames -> release-name stem.
BIN_KINDS = {
    "espview_esp32.bin": "espview",
    "bootloader.bin": "bootloader",
    "partition-table.bin": "partition-table",
}

# Profile names may contain only [A-Za-z0-9_-] (see espview_profiles.py).
PROFILE_NAME_RE = re.compile(r"^[A-Za-z0-9_-]+$")

# Path substrings that are never allowed to be packaged.
FORBIDDEN_SUBSTRINGS = ("sdkconfig", "logs")

CHUNK_SIZE = 1024 * 1024
SHORT_SHA_LEN = 8


class UsageError(Exception):
    """Invalid command-line data (exit code 2)."""


class PolicyError(Exception):
    """Packaging refusal for security reasons (exit code 1)."""


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        while True:
            block = fh.read(CHUNK_SIZE)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def assert_safe(path):
    """Refuse paths that could carry credentials or logs."""
    folded = path.replace("\\", "/").lower()
    for bad in FORBIDDEN_SUBSTRINGS:
        if bad in folded:
            raise PolicyError(
                "refusing to package %r: path contains %r" % (path, bad))


def parse_entry(text):
    """Parse one 'profile:path' entry (the path may itself contain ':' on
    Windows drive letters, so split on the first colon only)."""
    profile, sep, path = text.partition(":")
    profile = profile.strip()
    path = path.strip()
    if not sep or not profile or not path:
        raise UsageError(
            "malformed --bin entry %r (expected PROFILE:PATH)" % text)
    if not PROFILE_NAME_RE.match(profile):
        raise UsageError(
            "invalid profile name %r in entry %r" % (profile, text))
    return profile, path


def read_manifest(path):
    entries = []
    with open(path, "r", encoding="utf-8") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            try:
                entries.append(parse_entry(line))
            except UsageError as exc:
                raise UsageError("%s:%d: %s" % (path, lineno, exc))
    if not entries:
        raise UsageError("manifest %r contains no entries" % path)
    return entries


def plan(entries, short):
    """Validate every entry and return [(source, dest_name), ...] pairs."""
    planned = []
    seen = {}
    for profile, src in entries:
        assert_safe(src)
        if not os.path.isfile(src):
            raise PolicyError(
                "bin file not found or not a regular file: %r" % src)
        kind = BIN_KINDS.get(os.path.basename(src))
        if kind is None:
            raise UsageError(
                "unknown bin type %r for profile %r (expected one of: %s)"
                % (os.path.basename(src), profile,
                   ", ".join(sorted(BIN_KINDS))))
        dest = "%s-%s-%s.bin" % (kind, profile, short)
        assert_safe(dest)
        if dest in seen:
            raise UsageError(
                "duplicate destination %r from entries %r and %r"
                % (dest, seen[dest], src))
        seen[dest] = src
        planned.append((src, dest))
    return planned


def write_checksums(out_dir, copied):
    """copied: [(dest_name, source_path), ...] already copied to out_dir."""
    lines = []
    for dest_name, _src in sorted(copied, key=lambda item: item[0]):
        digest = sha256_file(os.path.join(out_dir, dest_name))
        lines.append("%s  %s" % (digest, dest_name))
    manifest = os.path.join(out_dir, "SHA256SUMS.txt")
    with open(manifest, "w", encoding="ascii") as fh:
        for line in lines:
            fh.write(line + "\n")
    return manifest, lines


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="ci_collect_artifacts.py",
        description=(
            "Collect ESP32 firmware bins per profile into a flat release "
            "directory with release-safe names and write SHA256SUMS.txt. "
            "Refuses any path containing 'sdkconfig' or 'logs'; never "
            "packages credentials or logs."))
    ap.add_argument("--sha", required=True,
                    help="full commit SHA (first 8 chars are used in names)")
    ap.add_argument("--tag", required=True,
                    help="release tag/branch name used in the summary")
    ap.add_argument("--out", required=True,
                    help="output directory for renamed bins + SHA256SUMS.txt")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--bin", action="append", metavar="PROFILE:PATH",
        help="firmware bin for a profile; repeatable, e.g. "
             "'uart:esp32-uart/espview_esp32.bin'")
    group.add_argument(
        "--manifest", metavar="FILE",
        help="text file with one 'PROFILE:PATH' entry per line")
    args = ap.parse_args(argv)

    if not re.fullmatch(r"[0-9a-fA-F]{7,64}", args.sha or ""):
        ap.error("--sha must be a hex commit SHA (7-64 chars)")
    short = args.sha[:SHORT_SHA_LEN]

    try:
        if args.manifest:
            entries = read_manifest(args.manifest)
        else:
            entries = [parse_entry(b) for b in args.bin]
        if not entries:
            raise UsageError("no --bin entries supplied")
        out_dir = os.path.abspath(args.out)
        assert_safe(out_dir)
        os.makedirs(out_dir, exist_ok=True)
        copied = []
        for src, dest_name in plan(entries, short):
            dest_path = os.path.join(out_dir, dest_name)
            shutil.copyfile(src, dest_path)
            copied.append((dest_name, src))
            print("packaged %s -> %s" % (src, dest_path))
        manifest_path, checksum_lines = write_checksums(out_dir, copied)
        print("wrote %s (%d file%s)" % (
            manifest_path, len(checksum_lines),
            "s" if len(checksum_lines) != 1 else ""))
        print("ci_collect_artifacts: OK -- %d file(s) for tag %r"
              % (len(copied), args.tag))
        return 0
    except UsageError as exc:
        print("ci_collect_artifacts: usage error: %s" % exc, file=sys.stderr)
        return 2
    except (PolicyError, OSError, IOError) as exc:
        print("ci_collect_artifacts: error: %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())