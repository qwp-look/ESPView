#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView M7-G -- profile sdkconfig manager (whitelisted, credential-free).

Ensures a profile's build dir has its own isolated sdkconfig and force-applies
exactly the profile's whitelisted Kconfig keys, so profiles can never drift
(no more mode_c showing OLED=y / TCP=y from a shared sdkconfig).

Security (red lines):
  * NEVER prints any line read from a sdkconfig file. Only the profile's own
    table line (whitelisted keys + expected values) is ever printed.
  * The whitelist (espview_profiles.py) rejects credential key names
    (SSID/PASSWORD/PSK/TOKEN/SECRET) by assertion.
  * The seed file (esp32/sdkconfig) is copied whole, never inspected or
    printed; Wi-Fi credentials, UART pins and other machine settings stay
    untouched inside the isolated profile sdkconfig.

Subcommands:
  --list                  print every whitelisted profile (machine-parseable)
  --show NAME             print one profile summary line
  --check NAME            exit 0 if NAME is whitelisted, else exit 2
  --apply NAME --sdkconfig PATH [--seed PATH] [--seed-defaults PATH]
                          seed (if missing) + force-apply whitelist keys;
                          prints only key names + expected values
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import sys

from espview_profiles import (CHOICE_GROUPS, FORBIDDEN_KEY_PARTS, get_profile,
                              profile_line, PROFILES, ALIASES, TARGETS,
                              validate_target)


def set_key(text, key, value):
    """Force one bool Kconfig key in sdkconfig text to y or n.

    y -> line "KEY=y" present, "# KEY is not set" removed
    n -> "# KEY is not set" present, "KEY=y" removed
    """
    if value == "y":
        text = re.sub(r"^#\s*%s is not set[ \t]*$" % re.escape(key), "",
                      text, flags=re.M)
        text = re.sub(r"^%s=n[ \t]*$" % re.escape(key), "", text, flags=re.M)
        if not re.search(r"^%s=y[ \t]*$" % re.escape(key), text, flags=re.M):
            text = text.rstrip("\n") + "\n%s=y\n" % key
    else:
        text = re.sub(r"^%s=y[ \t]*$" % re.escape(key), "", text,
                      flags=re.M)
        text = re.sub(r"^#\s*%s is not set[ \t]*$" % re.escape(key), "",
                      text, flags=re.M)
        if not re.search(r"^#\s*%s is not set[ \t]*$" % re.escape(key),
                         text, flags=re.M):
            text = text.rstrip("\n") + "\n# %s is not set\n" % key
    return text


def apply_config(text, config):
    """Apply a profile config dict: first settle choice groups, then keys."""
    chosen = {k for k, v in config.items() if v == "y"}
    for group in CHOICE_GROUPS:
        picked = [k for k in group if k in chosen]
        if picked:
            for k in group:
                if k not in picked:
                    text = set_key(text, k, "n")
    for key in sorted(config):
        text = set_key(text, key, config[key])
    return text


def seed_text(seed_path, seed_defaults_path):
    """Return seed file text (binary-preserving), or "" if nothing available."""
    for path in (seed_path, seed_defaults_path):
        if path and os.path.isfile(path):
            with open(path, "r", encoding="utf-8",
                      errors="surrogateescape") as fh:
                return fh.read()
    return ""


def extract_target(text):
    """Return CONFIG_IDF_TARGET value from sdkconfig text, or None."""
    m = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"', text, flags=re.M)
    return m.group(1) if m else None


def check_target(text, expected):
    """Verify sdkconfig text matches the requested IDF target.

    M8-A7（A7-5）：profile 与 target 正交，但 seed/sdkconfig 绝不能静默混用
    target（本地 sdkconfig 漂移到 esp32s3 是 DESIGN AS.1 记录过的实时证据）。
    不匹配时明确报错并给出修复命令，绝不自动改写 CONFIG_IDF_TARGET
    （由 idf.py set-target 管理）。
    """
    current = extract_target(text)
    if current is not None and current != expected:
        print("error: sdkconfig CONFIG_IDF_TARGET=%r != requested target %r; "
              "run 'idf.py set-target %s' for this build dir" %
              (current, expected, expected), file=sys.stderr)
        return 2
    if current is None:
        print("warning: sdkconfig has no CONFIG_IDF_TARGET; target will be "
              "resolved by idf.py (requested: %s)" % expected)
    return 0


def apply_profile(name, sdkconfig_path, seed_path=None,
                  seed_defaults_path=None, target=None):
    base, spec = get_profile(name)
    if spec is None:
        print("error: unknown profile '%s' (whitelist: %s)"
              % (name, ", ".join(sorted(PROFILES))), file=sys.stderr)
        return 2
    if target is not None and not validate_target(target):
        print("error: unknown target '%s' (whitelist: %s)"
              % (target, ", ".join(TARGETS)), file=sys.stderr)
        return 2
    directory = os.path.dirname(os.path.abspath(sdkconfig_path))
    os.makedirs(directory, exist_ok=True)
    if not os.path.isfile(sdkconfig_path):
        # Seed once: whole-file copy (never inspected/printed), preserving
        # machine settings + credentials; falls back to tracked defaults.
        text = seed_text(seed_path, seed_defaults_path)
        if target is not None and text:
            rc = check_target(text, target)
            if rc != 0:
                return rc
        if seed_path and os.path.isfile(seed_path) and not text:
            shutil.copyfile(seed_path, sdkconfig_path)
        elif text:
            with open(sdkconfig_path, "w", encoding="utf-8",
                      errors="surrogateescape", newline="") as fh:
                fh.write(text)
        else:
            with open(sdkconfig_path, "w", encoding="utf-8",
                      errors="surrogateescape", newline="") as fh:
                fh.write("")
        print("[profile] seeded %s -> %s"
              % ("<seed>" if text or (seed_path and os.path.isfile(seed_path))
                 else "<empty>", sdkconfig_path))
    with open(sdkconfig_path, "r", encoding="utf-8",
              errors="surrogateescape") as fh:
        text = fh.read()
    if target is not None:
        rc = check_target(text, target)
        if rc != 0:
            return rc
    # Hard guard: whitelist keys must never look like credentials.
    for key in spec["config"]:
        assert not any(part in key for part in FORBIDDEN_KEY_PARTS), key
    text = apply_config(text, spec["config"])
    with open(sdkconfig_path, "w", encoding="utf-8",
              errors="surrogateescape", newline="") as fh:
        fh.write(text)
    print("[profile] applied %d whitelisted keys to %s"
          % (len(spec["config"]), sdkconfig_path))
    print("  keys: %s" % ", ".join(sorted(spec["config"])))
    print(profile_line(base))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="ESPView profile sdkconfig manager (M7-G)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--show", metavar="NAME")
    ap.add_argument("--check", metavar="NAME")
    ap.add_argument("--apply", metavar="NAME")
    ap.add_argument("--sdkconfig", metavar="PATH")
    ap.add_argument("--seed", metavar="PATH", default=None)
    ap.add_argument("--seed-defaults", metavar="PATH", default=None)
    ap.add_argument("--target", metavar="NAME", default=None,
                    help="Expected IDF target (esp32/esp32s3); verifies "
                         "seed/sdkconfig CONFIG_IDF_TARGET, never rewrites it.")
    args = ap.parse_args(argv)

    modes = sum(1 for m in (args.list, args.show, args.check, args.apply)
                if m)
    if modes != 1:
        ap.error("exactly one of --list/--show/--check/--apply required")
    if args.list:
        for name in sorted(PROFILES):
            print(profile_line(name))
        return 0
    if args.show:
        line = profile_line(args.show)
        if line is None:
            print("error: unknown profile '%s'" % args.show,
                  file=sys.stderr)
            return 2
        print(line)
        return 0
    if args.check:
        line = profile_line(args.check)
        if line is None:
            print("error: unknown profile '%s' (whitelist: %s)"
                  % (args.check, ", ".join(sorted(PROFILES))),
                  file=sys.stderr)
            return 2
        print(line)
        return 0
    # --apply
    if not args.sdkconfig:
        ap.error("--apply requires --sdkconfig PATH")
    return apply_profile(args.apply, args.sdkconfig, args.seed,
                         args.seed_defaults, args.target)


if __name__ == "__main__":
    sys.exit(main())