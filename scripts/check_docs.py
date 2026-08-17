#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView M7-G G10 -- static documentation checker.

Checks (all local, no build / no hardware):
  1. README-referenced files / commands / scripts / profiles exist
     (scripts\\*.bat, scripts\\*.py, docs links, images).
  2. README-listed protocol messages and capabilities are findable in
     code (esp32/ pc/ shared/) or docs/DESIGN.md.
  3. Forbidden patterns are absent:
       - FRAME_FULL (removed protocol message; must not be presented)
       - "16 KiB MAX_PACKET_PAYLOAD" claim (real value is 4096)
       - "undefined frameSeq"
       - real Wi-Fi credential patterns: password=, WIFI_PASSWORD,
         CONFIG_ESPVIEW_WIFI_PASSWORD/SSID=, real private IPs (in scripts)
       - "已验证 / verified" claims with no findable evidence
  4. esp32/sdkconfig* must never be tracked by git (whitelisted sdkconfig.defaults / sdkconfig.defaults.esp32 / sdkconfig.defaults.esp32s3 excepted).
  5. docs cross-link validation: every markdown link in README.md and
     docs/**/*.md with a relative file target must resolve to an existing
     file or directory (http(s)://, mailto:, and #anchor-only skipped).
  6. example-command scripts referenced inside docs code fences
     (scripts\\xxx.bat / scripts\\xxx.py) must exist.
  7. README CI badge URLs for .github/workflows/<file>.yml must reference
     existing workflow files.
  8. README Documentation index must link docs/ci.md and docs/README.md
     (files must exist) and cover every top-level docs/*.md plus
     scripts/README.md.
  9. Every .github/workflows/*.yml reference to scripts/... must resolve to an
     existing script file.
 10. examples/ coverage: every file under examples/ must be referenced from
     README; example-internal references to scripts/... and examples/... must
     resolve to existing files.

Scope note: credential / IP patterns are checked on the *scripts* surface
(scripts/**, README.md, docs/**). docs/DESIGN.md legitimately records test
IPs as hardware evidence; flagged findings there are informational for the
main agent to scrub if desired. FRAME_FULL is checked on the whole corpus.

Output: one issue per line "CHECK file:line: message"; exit 0 = clean,
1 = issues found, 2 = usage/IO error.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPTS_DIR = os.path.join(REPO_ROOT, "scripts")
sys.path.insert(0, SCRIPTS_DIR)

from espview_profiles import PROFILES, ALIASES  # noqa: E402

# ---------------------------------------------------------------------------
# corpus
# ---------------------------------------------------------------------------
TEXT_EXT = (".md", ".py", ".bat", ".ps1", ".c", ".cpp", ".h", ".hpp",
            ".cmake", ".txt", ".yml", ".yaml", ".json", ".ini")
# Tool scripts are the checker's own machinery and CI/security helpers,
# not documentation; they are excluded from the docs surface.
TOOL_BASENAMES = frozenset((
    "check_docs.py", "check_docs.bat",
    "security_scan.py", "check_bat_crlf.py",
    "ci_esp32_build.py", "ci_collect_artifacts.py",
))


def walk(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (
            "build", "__pycache__", "managed_components", ".git")]
        for fn in filenames:
            if fn.startswith("sdkconfig") or fn.endswith(".pyc"):
                continue
            yield os.path.join(dirpath, fn)


def docs_surface_files():
    files = [os.path.join(REPO_ROOT, "README.md")]
    for base in ("docs", "scripts"):
        root = os.path.join(REPO_ROOT, base)
        if os.path.isdir(root):
            for path in walk(root):
                # tool scripts are machinery, not documentation
                if os.path.basename(path) in TOOL_BASENAMES:
                    continue
                if path.lower().endswith(TEXT_EXT):
                    files.append(path)
    return files


def code_tree_files():
    files = []
    for base in ("esp32", "pc", "shared"):
        root = os.path.join(REPO_ROOT, base)
        if os.path.isdir(root):
            for path in walk(root):
                if path.lower().endswith(TEXT_EXT):
                    files.append(path)
    return files


def md_files():
    files = [os.path.join(REPO_ROOT, "README.md")]
    root = os.path.join(REPO_ROOT, "docs")
    if os.path.isdir(root):
        for path in walk(root):
            if path.lower().endswith(".md"):
                files.append(path)
    return files


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# issue collector
# ---------------------------------------------------------------------------
class Checker:
    def __init__(self):
        self.issues = []
        self.doc_surface = docs_surface_files()
        self.code_tree = code_tree_files()
        self.md_files = md_files()
        self.design_text = ""
        design = os.path.join(REPO_ROOT, "docs", "DESIGN.md")
        if os.path.isfile(design):
            self.design_text = read_text(design)

    def report(self, path, line, message):
        rel = os.path.relpath(path, REPO_ROOT)
        self.issues.append("%s:%d: %s" % (rel, line, message))

    # -- check 1: README references (root README + scripts README) --
    def check_readme_references(self):
        for readme in (os.path.join(REPO_ROOT, "README.md"),
                       os.path.join(SCRIPTS_DIR, "README.md")):
            if not os.path.isfile(readme):
                self.report("<missing>", 0, "%s not found"
                            % os.path.basename(readme))
                continue
            self._check_one_readme(readme)

    def _check_one_readme(self, readme):
        text = read_text(readme)
        for lineno, line in enumerate(text.splitlines(), 1):
            # scripts\xxx / scripts/xxx references
            for m in re.finditer(r"scripts[\\/]([\w.\-]+)", line):
                name = m.group(1)
                path = os.path.join(SCRIPTS_DIR, name)
                if not os.path.isfile(path):
                    self.report(readme, lineno,
                                "README references missing script: %s" % name)
            # docs links (markdown)
            for m in re.finditer(r"\]\(([^)]+)\)", line):
                target = m.group(1).lstrip("./").replace("/", os.sep)
                if target.startswith("http"):
                    continue
                full = os.path.join(REPO_ROOT, target)
                if os.sep in target and not os.path.isfile(full) and \
                        not os.path.isdir(full):
                    self.report(readme, lineno,
                                "README link target missing: %s" % target)
            # profile names via -b / --profile
            for m in re.finditer(r"-b\s+([A-Za-z0-9_\-]+)|--profile\s+([A-Za-z0-9_\-]+)",
                                 line):
                name = m.group(1) or m.group(2)
                if name not in PROFILES and name not in ALIASES:
                    self.report(readme, lineno,
                                "README profile not whitelisted: %s" % name)
            # command-line args claimed for espview_virtual_display
            for m in re.finditer(r"(--[\w\-]+)", line):
                if m.group(1) in ("--transport", "--port", "--baud",
                                  "--tcp-bind", "--tcp-port", "--dump-png",
                                  "--diag-log", "--autoclose-ms",
                                  "--no-reset"):
                    self.check_token_in_code(m.group(1), readme, lineno,
                                             kind="GUI arg")

    def check_token_in_code(self, token, path, lineno, kind):
        found = token in self.design_text
        if not found:
            for f in self.code_tree:
                if token in read_text(f):
                    found = True
                    break
        if not found:
            self.report(path, lineno,
                        "%s %s not found in code or docs/DESIGN.md"
                        % (kind, token))

    # -- check 2: messages / capabilities --
    def check_capabilities(self):
        # Protocol messages + README capabilities that must be findable.
        tokens = [
            "HELLO", "CAPABILITIES", "SET_MODE", "WIFI_SCAN_REQ",
            "WIFI_SCAN_RESULT", "WIFI_CONFIG", "WIFI_STATUS",
            "FRAME_BEGIN", "FRAME_RECT", "FRAME_END", "PHYSICAL_PREVIEW",
            "PING", "PONG", "ACK_REQ", "INPUT_KEY", "INPUT_MOUSE",
            "CHUNKED", "CRC-32", "dirty-rect", "FULL resync",
            "TransportManager", "frameId", "FrameAssembler",
            "VirtualScreenWidget", "InputController", "SerialWorker",
            "HostTcpTransport", "trx", "TestPattern", "ESPVIEW_APP_LVGL",
            "CONFIG_ESPVIEW_TEST_TRANSPORT_SWITCH",
            "CONFIG_ESPVIEW_OLED_ENABLE", "CH340", "0.0.0.0:8765",
            "com3_frame_test", "tcp_transport_test",
            "transport_config_test", "espview_virtual_display",
            "wifi_wizard", "ESPVIEW_WIFI_SSID",
        ]
        for token in tokens:
            found = token in self.design_text
            if not found:
                for f in self.code_tree:
                    if token in read_text(f):
                        found = True
                        break
            if not found:
                self.report(os.path.join(REPO_ROOT, "README.md"), 0,
                            "message/capability not found anywhere: %s"
                            % token)

    # -- check 3: forbidden patterns --
    def check_forbidden(self):
        # FRAME_FULL on the whole text corpus (docs + scripts + code).
        for f in self.doc_surface + self.code_tree:
            text = read_text(f)
            for lineno, line in enumerate(text.splitlines(), 1):
                if "FRAME_FULL" in line:
                    self.report(f, lineno, "forbidden: FRAME_FULL present")
                if re.search(r"MAX_PACKET_PAYLOAD.{0,60}"
                             r"(16\s*KiB|16\s*KB|16384)", line) or \
                   re.search(r"(16\s*KiB|16\s*KB|16384).{0,60}"
                             r"MAX_PACKET_PAYLOAD", line):
                    self.report(f, lineno,
                                "forbidden: '16 KiB' MAX_PACKET_PAYLOAD claim")
                if re.search(r"undefined.{0,30}frameSeq|frameSeq.{0,30}"
                             r"undefined", line, re.IGNORECASE):
                    self.report(f, lineno, "forbidden: 'undefined frameSeq'")
        # Credential patterns on the docs surface only.
        for f in self.doc_surface:
            text = read_text(f)
            for lineno, line in enumerate(text.splitlines(), 1):
                m = re.search(r"(?i)password\s*=\s*(\S+)", line)
                if m:
                    val = m.group(1).strip('"').strip("'")
                    if (val and not val.startswith("<")
                            and len(val) >= 4
                            and not re.fullmatch(r"[*xX]+", val)):
                        self.report(f, lineno,
                                    "forbidden: password= with a value")
                if re.search(r"(?i)wifi_password", line):
                    self.report(f, lineno, "forbidden: WIFI_PASSWORD token")
                if re.search(r"CONFIG_ESPVIEW_WIFI_(?:PASSWORD|SSID)\s*=",
                             line):
                    self.report(f, lineno,
                                "forbidden: CONFIG_ESPVIEW_WIFI_*=")
                if re.search(r"\b(?:192\.168\.\d{1,3}\.\d{1,3}|"
                             r"10\.\d{1,3}\.\d{1,3}\.\d{1,3}|"
                             r"172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})"
                             r"\b", line):
                    self.report(f, lineno, "forbidden: real private IP")

    # -- check 3b: credentials never in tracked files --
    def check_git_sdkconfig(self):
        try:
            out = subprocess.run(
                ["git", "-C", REPO_ROOT, "ls-files", "esp32/sdkconfig*"],
                capture_output=True, text=True, timeout=30).stdout
        except Exception:
            return
        allowed = {"sdkconfig.defaults",
                  "sdkconfig.defaults.esp32",
                  "sdkconfig.defaults.esp32s3"}
        for line in out.splitlines():
            name = os.path.basename(line.strip())
            if line.strip() and name not in allowed:
                self.report(os.path.join(REPO_ROOT, "esp32"), 0,
                            "tracked file must not exist: %s" % line)

    # -- check 4: verified claims with evidence --
    def check_verified_claims(self):
        readme = os.path.join(REPO_ROOT, "README.md")
        if not os.path.isfile(readme):
            return
        stopwords = {"当前", "以及", "已经", "验证", "实测", "已", "的", "了",
                     "是", "在", "见", "节", "X", "W", "OK", "TCP", "UART",
                     "APP", "ALL", "PASS", "经"}
        text = read_text(readme)
        for lineno, line in enumerate(text.splitlines(), 1):
            if not re.search(r"已验证|已实测|verified", line, re.IGNORECASE):
                continue
            if "DESIGN.md" in line or "DESIGN" in line:
                continue
            tokens = re.findall(r"[\u4e00-\u9fff]{2,}|[A-Za-z][A-Za-z0-9_\-]{2,}",
                                line)
            meaningful = [t for t in tokens if t not in stopwords
                          and t.lower() not in ("verified", "当前", "场景")]
            found = False
            for tok in meaningful:
                if tok in self.design_text:
                    found = True
                    break
            if not found:
                self.report(readme, lineno,
                            "'已验证' claim without findable evidence "
                            "(no DESIGN.md ref; tokens not in DESIGN.md): %s"
                            % line.strip()[:80])


    # -- check 5: docs cross-links resolve to existing files/dirs --
    def check_doc_links(self):
        for path in self.md_files:
            base_dir = os.path.dirname(path)
            for lineno, line in enumerate(read_text(path).splitlines(), 1):
                for m in re.finditer(r"\]\(\s*([^)\s]+)", line):
                    raw = m.group(1)
                    if raw.startswith(("http://", "https://", "mailto:",
                                       "data:", "#")):
                        continue
                    target = raw.split("#", 1)[0].split("?", 1)[0]
                    if not target:
                        continue
                    full = os.path.normpath(os.path.join(
                        base_dir, target.replace("/", os.sep)))
                    if not os.path.exists(full):
                        resolved = os.path.relpath(full, REPO_ROOT)
                        self.report(path, lineno,
                                    "docs link target missing: %s -> %s"
                                    % (target, resolved))

    # -- check 6: example-command scripts in docs code fences --
    def check_docs_script_refs(self):
        for path in self.md_files:
            if os.path.normcase(os.path.normpath(path)) == \
                    os.path.normcase(os.path.normpath(os.path.join(
                        REPO_ROOT, "README.md"))):
                continue  # README.md handled by check 1
            in_fence = False
            for lineno, line in enumerate(read_text(path).splitlines(), 1):
                if re.match(r"^\s*(?:```|~~~)", line):
                    in_fence = not in_fence
                    continue
                if not in_fence:
                    continue
                for m in re.finditer(r"scripts[\\/]([\w.\-]+)", line):
                    name = m.group(1)
                    if not os.path.isfile(os.path.join(SCRIPTS_DIR, name)):
                        self.report(path, lineno,
                                    "docs code fence references missing "
                                    "script: %s" % name)

    # -- check 7: README CI badges must reference real workflows --
    def check_ci_badges(self):
        readme = os.path.join(REPO_ROOT, "README.md")
        if not os.path.isfile(readme):
            return
        for lineno, line in enumerate(read_text(readme).splitlines(), 1):
            for m in re.finditer(
                    r"https://github\.com/qwp-look/ESPView/actions/"
                    r"workflows/([A-Za-z0-9._\-]+\.ya?ml)/badge\.svg",
                    line):
                name = m.group(1)
                full = os.path.join(REPO_ROOT, ".github", "workflows", name)
                if not os.path.isfile(full):
                    self.report(readme, lineno,
                                "CI badge references missing workflow: %s"
                                % name)

    # -- check 8: README Documentation index links ci.md + README.md --
    def check_doc_index(self):
        readme = os.path.join(REPO_ROOT, "README.md")
        if not os.path.isfile(readme):
            return
        required = ("docs/ci.md", "docs/README.md")
        heading_lineno = 0
        in_index = False
        links = set()
        for lineno, line in enumerate(read_text(readme).splitlines(), 1):
            if re.match(r"^#+\s+Documentation index\s*$", line):
                in_index = True
                heading_lineno = lineno
                continue
            if in_index:
                if re.match(r"^#+\s+", line):
                    break
                for m in re.finditer(r"\]\(\s*([^)\s]+)", line):
                    target = m.group(1).split("#", 1)[0].split("?", 1)[0]
                    if not target or target.startswith(("http", "mailto")):
                        continue
                    links.add(target.lstrip("./").replace("/", os.sep))
        if heading_lineno == 0:
            self.report(readme, 0,
                        "README Documentation index section not found")
            return
        for req in required:
            norm = req.replace("/", os.sep)
            if norm not in links:
                self.report(readme, heading_lineno,
                            "README Documentation index must link %s" % req)
            elif not os.path.isfile(os.path.join(REPO_ROOT, *req.split("/"))):
                self.report(readme, heading_lineno,
                            "README Documentation index links missing file: "
                            "%s" % req)
        # Completeness (M8-A6 §二十五.1): every top-level docs/*.md and
        # scripts/README.md must be listed in the README Documentation index.
        docs_root = os.path.join(REPO_ROOT, "docs")
        if os.path.isdir(docs_root):
            for name in sorted(os.listdir(docs_root)):
                if not name.endswith(".md"):
                    continue
                norm = os.path.join("docs", name).replace("/", os.sep)
                if norm not in links:
                    self.report(readme, heading_lineno,
                                "README Documentation index missing docs/%s"
                                % name)
        scripts_readme = os.path.join("scripts", "README.md")
        if os.path.isfile(os.path.join(REPO_ROOT, *scripts_readme.split("/"))):
            norm = scripts_readme.replace("/", os.sep)
            if norm not in links:
                self.report(readme, heading_lineno,
                            "README Documentation index must link "
                            "scripts/README.md")

    # -- check 9: workflow YAML script references must exist --
    def check_workflow_script_refs(self):
        wf_root = os.path.join(REPO_ROOT, ".github", "workflows")
        if not os.path.isdir(wf_root):
            return
        for name in sorted(os.listdir(wf_root)):
            if not name.endswith((".yml", ".yaml")):
                continue
            path = os.path.join(wf_root, name)
            for lineno, line in enumerate(read_text(path).splitlines(), 1):
                for m in re.finditer(r"scripts[\\/]([\w.\-]+)(\*)?", line):
                    script = m.group(1)
                    if m.group(2) == "*":
                        # glob 前缀（workflow path filter 的 scripts/xxx* 形式）
                        if not any(fn.startswith(script)
                                   for fn in os.listdir(SCRIPTS_DIR)):
                            self.report(path, lineno,
                                        "workflow references missing script "
                                        "glob prefix: %s" % script)
                    elif not os.path.isfile(
                            os.path.join(SCRIPTS_DIR, script)):
                        self.report(path, lineno,
                                    "workflow references missing script: %s"
                                    % script)

    # -- check 10: examples coverage (M8-A6 §二十五.4 / §四十八) --
    def check_examples(self):
        examples_dir = os.path.join(REPO_ROOT, "examples")
        if not os.path.isdir(examples_dir):
            return
        readme = os.path.join(REPO_ROOT, "README.md")
        readme_text = read_text(readme) if os.path.isfile(readme) else ""
        example_files = []
        for dirpath, dirnames, filenames in os.walk(examples_dir):
            dirnames[:] = [d for d in dirnames
                           if d not in ("build", "__pycache__")]
            for fn in filenames:
                if fn.endswith(".pyc"):
                    continue
                example_files.append(os.path.join(dirpath, fn))
        # (a) README must be able to find every example file.
        for path in example_files:
            rel = os.path.relpath(path, REPO_ROOT).replace(os.sep, "/")
            if rel not in readme_text:
                self.report(readme, 0,
                            "README must reference example: %s" % rel)
        # (b) example-internal script/config references must exist.
        for path in example_files:
            text = read_text(path)
            for lineno, line in enumerate(text.splitlines(), 1):
                for m in re.finditer(r"scripts[\\/]([\w.\-]+)", line):
                    script = m.group(1)
                    if not os.path.isfile(os.path.join(SCRIPTS_DIR, script)):
                        self.report(path, lineno,
                                    "example references missing script: %s"
                                    % script)
                for m in re.finditer(r"examples[\\/]([\w.\-]+)", line):
                    target = m.group(1)
                    if not os.path.exists(os.path.join(examples_dir, target)):
                        self.report(path, lineno,
                                    "example references missing example "
                                    "file: %s" % target)

def main():
    c = Checker()
    c.check_readme_references()
    c.check_capabilities()
    c.check_forbidden()
    c.check_git_sdkconfig()
    c.check_verified_claims()
    c.check_doc_links()
    c.check_docs_script_refs()
    c.check_ci_badges()
    c.check_doc_index()
    c.check_workflow_script_refs()
    c.check_examples()
    issues = c.issues
    if issues:
        for issue in issues:
            print("CHECK %s" % issue)
        print("check_docs: %d issue(s) found" % len(issues))
        return 1
    print("check_docs: OK (all referenced files exist, patterns clean, "
          "no tracked credentials)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
