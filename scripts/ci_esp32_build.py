#!/usr/bin/env python3
"""ESPView CI build orchestrator (pure-python, credential-safe).

Mirrors the esp32-ci workflow steps: validate the profile against the
whitelist, force-apply the profile sdkconfig (seeded from sdkconfig.defaults,
never from the untracked local sdkconfig), run the ESP-IDF build, verify the
three firmware artifacts, and write SHA256SUMS.txt.

The build log and this script's output never include sdkconfig contents.
Exit codes: 0 = success, 1 = build/operation failure, 2 = usage error.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
import sys

# 全部可构建 profile（含历史 g1_*；g1_* 仅供 dispatch/显式构建，
# 不进 CI 默认矩阵 —— 见 espview_profiles.HISTORICAL_PROFILES）。
PROFILES = (
    "uart", "tcp", "oled", "oled-off", "diagnostic",
    "g1_a", "g1_b", "g1_c", "g1_d",
)

# M8-A7（A7-5/7-6）：target 白名单 + 历史 profile 标记单一来源 =
# espview_profiles（脚本目录在 sys.path，与 espview_profile_sdkconfig.py 同模式）。
from espview_profiles import TARGETS, HISTORICAL_PROFILES

ARTIFACTS = (
    "espview_esp32.bin",
    "bootloader/bootloader.bin",
    "partition_table/partition-table.bin",
)


def run(cmd, cwd=None):
    print("[ci_esp32_build] $ %s" % " ".join(cmd))
    result = subprocess.run(cmd, cwd=cwd)
    return result.returncode


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build(repo_root, profile, target="esp32"):
    esp32_dir = os.path.join(repo_root, "esp32")
    scripts_dir = os.path.join(repo_root, "scripts")
    build_dir = os.path.join(esp32_dir, "build", profile)
    sdkconfig_path = os.path.join(build_dir, "sdkconfig")
    # M8-A7（A7-7）：target-specific seed defaults（存在时优先）；profile 与
    # target 正交。esp32s3 的 defaults 由 A7-7 提供。
    seed_defaults = os.path.join(esp32_dir, "sdkconfig.defaults.%s" % target)
    if not os.path.isfile(seed_defaults):
        seed_defaults = os.path.join(esp32_dir, "sdkconfig.defaults")
    python = sys.executable
    sdkconfig_manager = os.path.join(scripts_dir, "espview_profile_sdkconfig.py")

    rc = run([python, sdkconfig_manager, "--check", profile])
    if rc != 0:
        print("[ci_esp32_build] ERROR: profile validation failed", file=sys.stderr)
        return 1

    rc = run([python, sdkconfig_manager, "--apply", profile,
              "--sdkconfig", sdkconfig_path, "--seed-defaults", seed_defaults,
              "--target", target])
    if rc != 0:
        print("[ci_esp32_build] ERROR: sdkconfig apply failed", file=sys.stderr)
        return 1

    idf = shutil.which("idf.py") or "idf.py"
    # M8-A7（A7-5）：目录布局仍为 build/<profile>（与 esp32-ci.yml 一致）；
    # target 轴目录（build/<target>/<profile>）由 A7-7 连同 collector 一起落地。
    rc = run([idf, "-B", os.path.join("build", profile),
              "-D", "SDKCONFIG=%s" % sdkconfig_path, "build"], cwd=esp32_dir)
    if rc != 0:
        print("[ci_esp32_build] ERROR: idf.py build failed", file=sys.stderr)
        return 1

    for rel in ARTIFACTS:
        path = os.path.join(build_dir, rel)
        if not os.path.isfile(path):
            print("[ci_esp32_build] ERROR: missing artifact %s" % path, file=sys.stderr)
            return 1

    sums_path = os.path.join(build_dir, "SHA256SUMS.txt")
    with open(sums_path, "w", encoding="utf-8") as fh:
        for rel in ARTIFACTS:
            fh.write("%s  %s\n" % (sha256_file(os.path.join(build_dir, rel)), rel))
    print("[ci_esp32_build] wrote %s" % sums_path)
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=("ESPView ESP32 CI build orchestrator "
                     "(credential-safe: never prints sdkconfig contents)."))
    parser.add_argument("--profile", required=True, choices=PROFILES,
                        help="ESPView profile to build (whitelisted).")
    parser.add_argument("--target", default="esp32", choices=TARGETS,
                        help="IDF target (default: esp32).")
    parser.add_argument("--repo-root", default=os.getcwd(),
                        help="ESPView repository root (default: current directory).")
    args = parser.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root)
    manager = os.path.join(repo_root, "scripts", "espview_profile_sdkconfig.py")
    if not os.path.isfile(manager) or not os.path.isdir(os.path.join(repo_root, "esp32")):
        print("[ci_esp32_build] ERROR: %s is not the ESPView repo root" % repo_root,
              file=sys.stderr)
        return 2

    return build(repo_root, args.profile, args.target)


if __name__ == "__main__":
    sys.exit(main())
