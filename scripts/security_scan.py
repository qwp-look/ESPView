#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView GitHub Actions security scanner.

Scans git-tracked files only (git ls-files -z from the repo root; when the
tree has no .git -- the fresh-clone gate / tarball -- falls back to a
filesystem walk of the repo tree) and
flags hardcoded secrets and private networking material:

  a) GitHub tokens: ghp_/gho_/ghs_/ghu_ + github_pat_ forms.
  b) AWS access key IDs: AKIA[0-9A-Z]{16}.
  c) PEM private key blocks: -----BEGIN [A-Z ]*PRIVATE KEY-----.
  d) JWTs: eyJ... . eyJ... (JWS compact form).
  e) Wi-Fi credential assignments: WIFI SSID/PASSWORD config keys
     (CONFIG_ESPVIEW_WIFI_* family) assigned a non-empty quoted value,
     and generic password assignments with a quoted 4+ char value
     (docs surface style). Values that are documented placeholders are
     allowed: <...>, "your-...", or REPLACE_WITH_* (the
     examples/sdkconfig.wifi-tcp.defaults.example header documents
     REPLACE_WITH_* as placeholders).
  f) Private IPs (192.168.x.x / 10.x.x.x / 172.16-31.x.x) except the
     documented allowlist:
       - CIDR allowlist: 127.0.0.0/8 (loopback), 192.0.2.0/24
         (TEST-NET-1), 198.51.100.0/24 (TEST-NET-2), 203.0.113.0/24
         (TEST-NET-3) -- all documentation ranges;
       - exact generic values: 0.0.0.0 (wildcard bind),
         255.255.255.255 (broadcast);
       - exact placeholders / test vectors found in this repo:
         192.168.1.100  Kconfig default
                        (esp32/components/espview/Kconfig:63), UI
                        placeholder (pc/src/wifi_wizard_dialog.cpp:302),
                        example (examples/sdkconfig.wifi-tcp.defaults.example:21);
         192.168.1.1    protocol/UI test vector (*_test.cpp);
         10.0.0.1       protocol/UI test vector (*_test.cpp);
         10.0.0.2       protocol/UI test vector (*_test.cpp);
         10.0.0.9       protocol/UI test vector (*_test.cpp).
       192.168.3.x (the real dev LAN) is NEVER allowlisted.
  g) Tracked esp32/sdkconfig* files other than esp32/sdkconfig.defaults
     and esp32/sdkconfig.defaults.*.

Binary files (NUL byte in the first 4 KiB, or an extension in
.png/.jpg/.bin/.elf/.o/.a/.so/.dll/.exe/.pyc) are skipped. Text is read
with UTF-8 and errors="replace".

Output: one finding per line "file:line: LABEL: description"; on clean
prints "security_scan: clean (N tracked files scanned)".

Exit: 0 = clean, 1 = findings, 2 = usage/IO error.
"""
from __future__ import annotations

import argparse
import ipaddress
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BINARY_EXTS = (".png", ".jpg", ".bin", ".elf", ".o", ".a", ".so",
               ".dll", ".exe", ".pyc")
NUL_SNIFF_LEN = 4096

# Documented allowlist (provenance in the module docstring).
ALLOWED_CIDRS = (
    ("127.0.0.0/8", "loopback"),
    ("192.0.2.0/24", "TEST-NET-1 documentation range"),
    ("198.51.100.0/24", "TEST-NET-2 documentation range"),
    ("203.0.113.0/24", "TEST-NET-3 documentation range"),
)
ALLOWED_EXACT = {
    "0.0.0.0": "wildcard bind address",
    "255.255.255.255": "broadcast address",
    "192.168.1.100": "Kconfig default (esp32/components/espview/Kconfig:63); "
                     "UI placeholder (pc/src/wifi_wizard_dialog.cpp:302); "
                     "example (examples/sdkconfig.wifi-tcp.defaults.example:21)",
    "192.168.1.1": "protocol/UI test vector (*_test.cpp)",
    "10.0.0.1": "protocol/UI test vector (*_test.cpp)",
    "10.0.0.2": "protocol/UI test vector (*_test.cpp)",
    "10.0.0.9": "protocol/UI test vector (*_test.cpp)",
}
ALLOWED_NETS = [ipaddress.ip_network(cidr) for cidr, _ in ALLOWED_CIDRS]

PRIVATE_IP_RE = re.compile(
    r"(?<!\d)(?:192\.168\.\d{1,3}\.\d{1,3}|"
    r"10\.\d{1,3}\.\d{1,3}\.\d{1,3}|"
    r"172\.(?:1[6-9]|2[0-9]|3[01])\.\d{1,3}\.\d{1,3})(?!\d)"
)
GITHUB_TOKEN_RE = re.compile(
    r"\b(?:ghp|gho|ghs|ghu)_[A-Za-z0-9]{20,}\b|"
    r"github_pat_[A-Za-z0-9_]{20,}"
)
AWS_KEY_RE = re.compile(r"AKIA[0-9A-Z]{16}")
PRIVATE_KEY_RE = re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----")
JWT_RE = re.compile(r"eyJ[A-Za-z0-9_-]{10,}\.eyJ[A-Za-z0-9_-]{10,}")
WIFI_ASSIGN_RE = re.compile(
    r"(?:WIFI_|WIFI_SSID|WIFI_PASSWORD|CONFIG_ESPVIEW_WIFI_(?:SSID|PASSWORD))"
    r"\s*=\s*[\"'][^\"']+[\"']"
)
PASSWORD_ASSIGN_RE = re.compile(r"password\s*=\s*[\"'][^\"']{4,}[\"']")


def is_documented_placeholder(value):
    """True for documented placeholder values (<...>, your-..., REPLACE_WITH_*)."""
    v = value.strip().strip("\"'")
    if len(v) > 1 and v.startswith("<") and v.endswith(">"):
        return True
    if v.startswith("your-"):
        return True
    if v.startswith("REPLACE_WITH_"):
        return True
    return False


def is_allowed_ip(ip_str):
    if ip_str in ALLOWED_EXACT:
        return True
    try:
        addr = ipaddress.ip_address(ip_str)
    except ValueError:
        return False
    return any(addr in net for net in ALLOWED_NETS)


def _git_ls_files(args):
    # git ls-files -z <args> output, or None when git is missing / not a repo.
    try:
        proc = subprocess.run(
            ["git", "-C", REPO_ROOT, "ls-files", "-z"] + args,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    except Exception:
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout.decode("utf-8", "replace")


def _walk_fallback():
    # Enumerate repo files without git (fresh-clone gate / tarball trees).
    out = []
    for root, dirs, files in os.walk(REPO_ROOT):
        dirs[:] = [d for d in dirs if d not in (".git", "build")]
        for name in files:
            full = os.path.join(root, name)
            rel = os.path.relpath(full, REPO_ROOT).replace(os.sep, "/")
            out.append(rel)
    return sorted(out)


def git_tracked_files():
    text = _git_ls_files([])
    if text is None:
        print("security_scan: git unavailable (not a repo); scanning filesystem tree",
              file=sys.stderr)
        return _walk_fallback()
    return [p for p in text.split("\0") if p]


def check_tracked_sdkconfig():
    findings = []
    text = _git_ls_files(["esp32/sdkconfig*"])
    if text is None:
        names = [p for p in _walk_fallback() if p.startswith("esp32/sdkconfig")]
    else:
        names = [n for n in text.split("\0") if n.strip()]
    for name in names:
        name = name.strip()
        if not name:
            continue
        if name == "esp32/sdkconfig.defaults":
            continue
        if name.startswith("esp32/sdkconfig.defaults."):
            continue
        findings.append("%s:0: TRACKED_SDKCONFIG: tracked sdkconfig file "
                        "must not exist" % name)
    return findings


def scan_line_findings(rel, lineno, line):
    findings = []
    for _ in GITHUB_TOKEN_RE.finditer(line):
        findings.append("%s:%d: GITHUB_TOKEN: GitHub token pattern"
                        % (rel, lineno))
    for _ in AWS_KEY_RE.finditer(line):
        findings.append("%s:%d: AWS_ACCESS_KEY: AWS access key pattern"
                        % (rel, lineno))
    for _ in PRIVATE_KEY_RE.finditer(line):
        findings.append("%s:%d: PRIVATE_KEY: private key block"
                        % (rel, lineno))
    for _ in JWT_RE.finditer(line):
        findings.append("%s:%d: JWT: JWT token pattern" % (rel, lineno))
    for m in WIFI_ASSIGN_RE.finditer(line):
        value = m.group(0).split("=", 1)[1]
        if not is_documented_placeholder(value):
            findings.append(
                "%s:%d: WIFI_CREDENTIAL: Wi-Fi credential assignment"
                % (rel, lineno))
    for m in PASSWORD_ASSIGN_RE.finditer(line):
        value = m.group(0).split("=", 1)[1]
        if not is_documented_placeholder(value):
            findings.append(
                "%s:%d: WIFI_CREDENTIAL: password assignment"
                % (rel, lineno))
    for m in PRIVATE_IP_RE.finditer(line):
        ip_str = m.group(0)
        if not is_allowed_ip(ip_str):
            findings.append(
                "%s:%d: PRIVATE_IP: private IP %s not on the documented "
                "allowlist" % (rel, lineno, ip_str))
    return findings


def run_scan():
    messages = []
    seen = set()
    scanned = 0
    for rel in git_tracked_files():
        path = os.path.join(REPO_ROOT, rel.replace("/", os.sep))
        if not os.path.isfile(path):
            raise IOError("tracked file missing from working tree: %s" % rel)
        if os.path.splitext(rel)[1].lower() in BINARY_EXTS:
            continue
        try:
            with open(path, "rb") as fh:
                data = fh.read()
        except OSError as exc:
            raise IOError("cannot read %s: %s" % (rel, exc))
        if b"\x00" in data[:NUL_SNIFF_LEN]:
            continue
        scanned += 1
        text = data.decode("utf-8", "replace")
        for lineno, line in enumerate(text.split("\n"), 1):
            for msg in scan_line_findings(rel, lineno, line.rstrip("\r")):
                if msg not in seen:
                    seen.add(msg)
                    messages.append(msg)
    messages.extend(check_tracked_sdkconfig())
    return messages, scanned


def print_allowlist():
    print("security_scan documented allowlist")
    print("CIDR ranges:")
    for cidr, note in ALLOWED_CIDRS:
        print("  %-16s %s" % (cidr, note))
    print("exact generic addresses:")
    for addr in sorted(ALLOWED_EXACT):
        print("  %-16s %s" % (addr, ALLOWED_EXACT[addr]))
    print("192.168.3.x (the real dev LAN) is never allowlisted.")


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="security_scan",
        description="Scan git-tracked ESPView files for hardcoded secrets "
                    "and private networking material.")
    parser.add_argument("--list-allowlist", action="store_true",
                        help="print the documented private-IP allowlist and exit")
    args = parser.parse_args(argv)
    if args.list_allowlist:
        print_allowlist()
        return 0
    try:
        messages, scanned = run_scan()
    except IOError as exc:
        print("security_scan: error: %s" % exc, file=sys.stderr)
        return 2
    for msg in messages:
        print(msg)
    if messages:
        print("security_scan: %d finding(s) in %d tracked files scanned"
              % (len(messages), scanned))
        return 1
    print("security_scan: clean (%d tracked files scanned)" % scanned)
    return 0


if __name__ == "__main__":
    sys.exit(main())
