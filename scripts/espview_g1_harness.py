#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView M7-G G1 -- hardware A/B/C/D profile harness (task book section 6).

Runs the OLED x RF matrix on real hardware over the UART protocol link:

  mode A: OLED OFF + RF OFF   (profile g1_a)
  mode B: OLED ON  + RF OFF   (profile g1_b)
  mode C: OLED OFF + RF ON    (profile g1_c)
  mode D: OLED ON  + RF ON    (profile g1_d)

Each mode records, in a fixed machine-parseable format:
  boot / CH340 connection / HELLO / RF start / scan / scan result /
  disconnect / reset / GOT_IP.

Per-mode flow (optional steps):
  1. build the profile (espview_profile_sdkconfig.py --apply = seed once +
     force-apply the profile's whitelisted Kconfig keys, so profiles can
     never drift; then idf.py -B build\\<profile> -D SDKCONFIG=... build;
     gated behind --build, skipped by default to avoid parallel-agent
     clashes);
  2. flash (scripts\\espview_flash.bat -b <profile> -p <port> --no-reset;
     gated behind --flash);
  3. optional probe: build\\win32_probe\\win32_com_probe.exe
     --port <port> --baud <baud> --pulse-reset (gated behind --probe);
  4. open the COM port -> DTR/RTS reset -> wait for ESP32 HELLO ->
     reply with PC HELLO -> session CONNECTED;
  5. RF-ON modes (C/D): send WIFI_SCAN_REQ (ACK_REQ, maxEntries=32) ->
     record ACK, WIFI_STATUS phases (RF start = phase scanning, GOT_IP =
     phase got_ip), WIFI_SCAN_RESULT;
     RF-OFF modes (A/B): observe only (rf_start_seen must stay 0);
  6. continuously record UART connect/disconnect, ReadFile errors, ESP32
     reset banners (rst:), session state, protocol statistics.

Output conventions (repeatable, diffable; same style as the M7-E harness):
  - events : [evt] t=+<rel seconds> kind=<kind> k=v ...
  - summary: # key=value (one block per mode, keys sorted)
  - verdict: == RESULT: A=PASS B=PASS C=PASS D=PASS ==
  - raw logs: --log-dir (default <repo>/build/g1_logs, gitignored):
    g1_<mode>_it<N>_raw.bin (raw serial bytes) + g1_<mode>_it<N>_events.txt

Security: never reads esp32/sdkconfig content (the profile manager copies
it whole without inspection); never sends or records Wi-Fi credentials
(SSID is non-secret metadata, same as the existing probe); never sends
WIFI_CONFIG (zero password involvement).

Usage:
  python espview_g1_harness.py [--port COM4] [--baud 115200]
                               [--modes A,B,C,D] [--iterations 1]
                               [--build] [--flash] [--probe]
                               [--probe-timeout-ms 8000] [--max-reopens 0]
                               [--strict] [--result-file PATH]
                               [--log-dir DIR] [--dry-run]
Exit codes: 0 all PASS / 1 any mode FAIL / 2 usage, environment or
build/flash/probe failure.
"""
from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Reuse the frozen protocol machinery of the M7-E harness.
from espview_e_ab_harness import (  # noqa: E402
    VER, TYPE_HELLO, TYPE_CAPABILITIES, TYPE_PING, TYPE_PONG, TYPE_ACK,
    TYPE_WIFI_STATUS, TYPE_WIFI_SCAN_RESULT, TYPE_PHYSICAL_PREVIEW,
    TYPE_FRAME_BEGIN, TYPE_ERROR, FLAG_ACK_REQ, SESSION_NAMES,
    DEFAULT_ESPIDF_PROFILE, ps_quote, build_packet, parse_hello,
    parse_ack, parse_wifi_status, parse_scan_result, parse_error_text,
    Stream, Tee,
)

from espview_profiles import get_profile, profile_line  # noqa: E402

try:
    import serial
except ImportError as exc:  # pragma: no cover
    sys.exit("missing pyserial: %s (py -3.10 -m pip install pyserial)" % exc)

# G1 modes -> whitelisted profile names.
MODE_PROFILES = {"A": "g1_a", "B": "g1_b", "C": "g1_c", "D": "g1_d"}

PC_NAME = b"espview-g1"


def find_com_probe(repo_root):
    """Locate win32_com_probe.exe: env override -> repo build dir -> PATH."""
    env = os.environ.get("WIN32_COM_PROBE")
    if env and os.path.isfile(env):
        return env
    cand = os.path.join(repo_root, "build", "win32_probe",
                        "win32_com_probe.exe")
    if os.path.isfile(cand):
        return cand
    import shutil
    return shutil.which("win32_com_probe.exe")


class G1Harness:
    def __init__(self, args):
        self.args = args
        self.repo_root = os.path.dirname(os.path.dirname(
            os.path.abspath(__file__)))
        self.scripts_dir = os.path.join(self.repo_root, "scripts")
        self.t0 = time.monotonic()
        self.events = []
        self.ser = None
        self.stream = Stream()
        self.rx = {"ping": 0, "pong": 0, "error": 0, "ack": 0,
                   "wifi_status": 0, "wifi_scan_result": 0,
                   "preview": 0, "frame_begin": 0, "hello": 0,
                   "capabilities": 0, "unexpected": 0}
        self.seq = 0
        self.session_state = 0
        self.session_states = [(0.0, "disconnected")]
        self.hello_info = None
        self.hello_ok = False
        self.hello_ms = None
        self.hello_t = None
        self.ack = None
        self.ack_ms = None
        self.scan_req_seq = None
        self.scan_req_t = None
        self.scan_result = None
        self.scan_result_t = None
        self.scan_phase1_first = None
        self.scan_phase1_last = None
        self.got_ip_t = None
        self.phases = []
        self.reboots = []
        self.uart_disconnects = 0
        self.readfile_errors = 0
        self.write_errors = 0
        self.peer_timeouts = 0
        self.reopens = 0
        self.last_rx = 0.0
        self.last_reboot_t = -10.0
        self.last_reset_t = -10.0
        self.raw_log = None
        self.log_prefix = ""

    # ---- output helpers ----
    def t_rel(self):
        return time.monotonic() - self.t0

    def event(self, kind, **kv):
        parts = ["[evt]", "t=+%.3f" % self.t_rel(), "kind=%s" % kind]
        for k in sorted(kv):
            parts.append("%s=%s" % (k, kv[k]))
        line = " ".join(parts)
        self.events.append(line)
        print(line, flush=True)
        if self.raw_log is not None:
            self.raw_log.write(line + "\n")

    def summary(self, kv):
        for k in sorted(kv):
            print("# %s=%s" % (k, kv[k]), flush=True)
            if self.raw_log is not None:
                self.raw_log.write("# %s=%s\n" % (k, kv[k]))

    # ---- raw log ----
    def _log_dir(self):
        return self.args.log_dir or os.path.join(
            self.repo_root, "build", "g1_logs")

    def open_log(self, mode, iteration):
        os.makedirs(self._log_dir(), exist_ok=True)
        self.log_prefix = "g1_%s_it%d" % (mode, iteration)
        self.raw_log = open(
            os.path.join(self._log_dir(),
                         "%s_events.txt" % self.log_prefix),
            "w", encoding="utf-8")

    def close_log(self):
        if self.raw_log is not None:
            self.raw_log.flush()
            self.raw_log.close()
            self.raw_log = None

    def log_raw(self, chunk):
        if self.raw_log is None:
            return
        path = os.path.join(self._log_dir(), "%s_raw.bin" % self.log_prefix)
        with open(path, "ab") as fh:
            fh.write(chunk)

    # ---- serial ----
    def _open_port(self):
        try:
            self.ser = serial.Serial(self.args.port, self.args.baud,
                                     timeout=0.05)
        except serial.SerialException as exc:
            print("[fail] cannot open %s: %s" % (self.args.port, exc),
                  flush=True)
            return False
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        self.last_rx = time.monotonic()
        self.event("uart_open", port=self.args.port, baud=self.args.baud)
        return True

    def _close_port(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
            self.event("uart_close")
            self.ser = None

    def reset_chip(self):
        """Normal boot reset: keep GPIO0(DTR) high, pulse EN(RTS) only."""
        self.ser.setDTR(False)
        self.ser.setRTS(True)
        time.sleep(0.1)
        self.ser.setRTS(False)
        time.sleep(0.2)
        self.last_reset_t = self.t_rel()
        self.event("reset", pulse="DTR=0 RTS=1->0")
        return True

    def _read_error(self, exc):
        self.readfile_errors += 1
        self.uart_disconnects += 1
        msg = str(exc).replace("\r", " ").replace("\n", " ")[:120]
        self.event("uart_read_error", err=msg)
        self.event("uart_disconnect", cause="readfile")
        if self.session_state == 3:
            self._set_session(0, "readfile_error")

    def _scan_raw_for_reboot(self, chunk):
        if b"rst:" in chunk or b"ets " in chunk or b"ets_" in chunk:
            now = self.t_rel()
            if now - self.last_reboot_t < 1.0:
                return
            self.last_reboot_t = now
            expected = (now - self.last_reset_t) < 2.0
            self.reboots.append({"t": now, "expected": expected})
            self.event("reboot", expected=1 if expected else 0,
                       marker="rst:" if b"rst:" in chunk else "ets")

    def _set_session(self, state, cause):
        if state != self.session_state:
            self.session_state = state
            self.session_states.append((self.t_rel(), SESSION_NAMES[state]))
            self.event("session", state=SESSION_NAMES[state], cause=cause)

    def _tx(self, ptype, flags, payload):
        seq = self.seq
        self.seq = (self.seq + 1) & 0xFFFF
        self.ser.write(build_packet(ptype, flags, seq, payload))
        return seq

    # ---- packet dispatch ----
    def _dispatch(self, payload, hdr):
        t = hdr["type"]
        if t == TYPE_HELLO:
            self.rx["hello"] += 1
            info = parse_hello(payload)
            if info is None:
                self.stream.stats["protocol_errors"] += 1
                return
            if self.hello_info is None:
                self.hello_info = info
                self.hello_t = self.t_rel()
            self.event("hello_rx", version=info["ver"],
                       size="%dx%d" % (info["width"], info["height"]),
                       name=info["name"])
        elif t == TYPE_CAPABILITIES:
            self.rx["capabilities"] += 1
        elif t == TYPE_PING:
            self.rx["ping"] += 1
            self._tx(TYPE_PONG, 0, payload)
        elif t == TYPE_PONG:
            self.rx["pong"] += 1
        elif t == TYPE_ACK:
            self.rx["ack"] += 1
            a = parse_ack(payload)
            if a is None:
                self.stream.stats["protocol_errors"] += 1
                return
            if self.ack is None:
                self.ack = a
                self.ack_ms = ((self.t_rel() - self.scan_req_t) * 1000
                               if self.scan_req_t is not None else None)
                self.event("ack_rx", seq=a["ack_seq"], status=a["status"],
                           err=a["err"])
        elif t == TYPE_WIFI_STATUS:
            self.rx["wifi_status"] += 1
            s = parse_wifi_status(payload)
            if s is None:
                self.stream.stats["protocol_errors"] += 1
                return
            if not self.phases or self.phases[-1][1] != s["phase"]:
                self.phases.append((self.t_rel(), s["phase"]))
            if s["phase"] == 1:
                if self.scan_phase1_first is None:
                    self.scan_phase1_first = self.t_rel()
                self.scan_phase1_last = self.t_rel()
            if s["phase"] == 5 and self.got_ip_t is None:
                self.got_ip_t = self.t_rel()
            self.event("status", phase=s["phase_name"], rssi=s["rssi"],
                       ch=s["channel"], err=s["err_code"])
        elif t == TYPE_WIFI_SCAN_RESULT:
            self.rx["wifi_scan_result"] += 1
            r = parse_scan_result(payload)
            if r is None:
                self.stream.stats["protocol_errors"] += 1
                return
            if self.scan_result is None:
                self.scan_result = r
                self.scan_result_t = self.t_rel()
            self.event("scan_result_rx", count=r["count"], total=r["total"],
                       truncated=1 if r["truncated"] else 0)
        elif t == TYPE_PHYSICAL_PREVIEW:
            self.rx["preview"] += 1
        elif t == TYPE_FRAME_BEGIN:
            self.rx["frame_begin"] += 1
        elif t == TYPE_ERROR:
            self.rx["error"] += 1
            e = parse_error_text(payload)
            if e is None:
                self.stream.stats["protocol_errors"] += 1
                return
            self.event("error_text", code=e["code"], text=e["text"])
        else:
            self.rx["unexpected"] += 1

    def _pump(self, timeout_s):
        """Read + dispatch. Returns "ok" or "disconnected"."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                n = self.ser.in_waiting
            except serial.SerialException as exc:
                self._read_error(exc)
                return "disconnected"
            if n > 0:
                try:
                    chunk = self.ser.read(n)
                except serial.SerialException as exc:
                    self._read_error(exc)
                    return "disconnected"
                if chunk:
                    self.log_raw(chunk)
                    self._scan_raw_for_reboot(chunk)
                    self.stream.feed(chunk)
                    self.last_rx = time.monotonic()
            while True:
                pkt = self.stream.next_packet()
                if pkt is None:
                    break
                self._dispatch(pkt[0], pkt[1])
            if self.session_state == 3 and self.last_rx > 0 and \
                    time.monotonic() - self.last_rx > self.args.peer_timeout:
                self.peer_timeouts += 1
                self.event("session_peer_timeout",
                           silent_s=round(time.monotonic() - self.last_rx, 1))
                self._set_session(0, "peer_timeout")
            time.sleep(0.002)
        return "ok"

    # ---- HELLO handshake ----
    def do_handshake(self, timeout_s):
        self._set_session(1, "handshake_start")
        deadline = time.monotonic() + timeout_s
        mine_sent = False
        hello_seen = False
        while time.monotonic() < deadline:
            rc = self._pump(0.3)
            if rc == "disconnected":
                self.handshake_fail_reason = "uart_disconnect"
                return False
            if self.hello_info is not None:
                hello_seen = True
                break
            if not mine_sent and self.t_rel() - self.last_reset_t >= 5.0:
                mine_sent = True
                payload = struct.pack("<BBHHBBB", VER, 0, 320, 240, 0, 0b111,
                                      len(PC_NAME)) + PC_NAME
                self._tx(TYPE_HELLO, 0, payload)
                self.event("hello_tx", name=PC_NAME.decode(),
                           name_len=len(PC_NAME))
                self.seq = 0
        if not hello_seen:
            self.handshake_fail_reason = "hello_timeout"
            self.event("fail", reason="hello_timeout", timeout=timeout_s)
            return False
        self.hello_ok = True
        self.hello_ms = (self.hello_t - self.last_reset_t) * 1000
        if not mine_sent:
            payload = struct.pack("<BBHHBBB", VER, 0, 320, 240, 0, 0b111,
                                  len(PC_NAME)) + PC_NAME
            self._tx(TYPE_HELLO, 0, payload)
            self.event("hello_tx", name=PC_NAME.decode(),
                       name_len=len(PC_NAME))
            self.seq = 0
        self._set_session(3, "handshake_ok")
        return True

    # ---- scan (RF-ON modes) ----
    def send_scan_req(self):
        payload = struct.pack("<BB", 0, self.args.max_entries)
        self.scan_req_t = self.t_rel()
        self.scan_req_seq = self._tx(TYPE_WIFI_SCAN_REQ, FLAG_ACK_REQ, payload)
        self.event("scan_req_tx", seq=self.scan_req_seq,
                   max_entries=self.args.max_entries)

    def one_scan_attempt(self, scan_timeout_s):
        self.ack = None
        self.ack_ms = None
        self.scan_result = None
        self.scan_result_t = None
        self.scan_phase1_first = None
        self.scan_phase1_last = None
        self.send_scan_req()
        ack_deadline = time.monotonic() + self.args.ack_timeout
        while self.ack is None and time.monotonic() < ack_deadline:
            rc = self._pump(0.2)
            if rc == "disconnected":
                return ("fail", "uart_disconnect")
        if self.ack is None:
            return ("fail", "ack_timeout")
        if self.ack["status"] != 0:
            return ("fail", "ack_err_%s" % self.ack["err"])
        scan_deadline = time.monotonic() + scan_timeout_s
        while self.scan_result is None and time.monotonic() < scan_deadline:
            rc = self._pump(0.2)
            if rc == "disconnected":
                return ("fail", "uart_disconnect")
        if self.scan_result is None:
            return ("fail", "scan_timeout")
        return ("ok", None)

    # ---- build / flash / probe ----
    def build_mode(self, mode):
        profile = MODE_PROFILES[mode]
        base, spec = get_profile(profile)
        if spec is None:
            print("[fail] unknown G1 profile %s" % profile, flush=True)
            return False
        esp32_dir = os.path.join(self.repo_root, "esp32")
        base_defaults = os.path.join(esp32_dir, "sdkconfig.defaults")
        if not os.path.exists(base_defaults):
            print("[fail] missing %s" % base_defaults, flush=True)
            return False
        idf_profile = os.environ.get("ESPIDF_PROFILE", DEFAULT_ESPIDF_PROFILE)
        if not os.path.exists(idf_profile):
            print("[fail] ESP-IDF profile not found: %s" % idf_profile,
                  flush=True)
            return False
        profile_sdkconfig = os.path.join(esp32_dir, "build", profile,
                                         "sdkconfig")
        py = [sys.executable,
              os.path.join(self.scripts_dir, "espview_profile_sdkconfig.py"),
              "--apply", profile, "--sdkconfig", profile_sdkconfig,
              "--seed", os.path.join(esp32_dir, "sdkconfig"),
              "--seed-defaults", base_defaults]
        self.event("sdkconfig_prep", profile=profile)
        proc = subprocess.run(py, cwd=self.repo_root)
        if proc.returncode != 0:
            print("[fail] profile sdkconfig prep failed (exit=%s)"
                  % proc.returncode, flush=True)
            return False
        ps = "\n".join([
            "$ErrorActionPreference = 'Stop'",
            "try {",
            "  . %s" % ps_quote(idf_profile),
            "  if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) "
            "{ Write-Host '[build] idf.py not available'; exit 3 }",
            "  Set-Location %s" % ps_quote(esp32_dir),
            "  idf.py -B build/%s -D SDKCONFIG=%s build"
            % (profile, ps_quote(profile_sdkconfig)),
            "  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }",
            "} catch { Write-Host ('[build] ERROR: ' + $_.Exception.Message); "
            "exit 1 }",
        ])
        self.event("build", mode=mode, profile=profile)
        print("  running: idf.py -B build/%s -D SDKCONFIG=... build"
              % profile, flush=True)
        proc = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy",
                               "Bypass", "-Command", ps], cwd=self.repo_root)
        if proc.returncode != 0:
            print("[fail] build failed (exit=%s)" % proc.returncode,
                  flush=True)
            if spec["attrs"][4] == "OFF":
                print("[hint] RF-OFF profiles need the ESP32 firmware to "
                      "build with CONFIG_ESP_WIFI_ENABLED=n (G1 esp32-side "
                      "work); see espview_g1_harness docs", flush=True)
            return False
        self.event("build_ok", mode=mode)
        return True

    def flash_mode(self, mode):
        profile = MODE_PROFILES[mode]
        bat = os.path.join(self.scripts_dir, "espview_flash.bat")
        if not os.path.exists(bat):
            print("[fail] missing %s" % bat, flush=True)
            return False
        self.event("flash", mode=mode, profile=profile, port=self.args.port)
        proc = subprocess.run(["cmd", "/c", bat, "-p", self.args.port,
                               "-b", profile, "--no-reset"],
                              cwd=self.repo_root)
        if proc.returncode != 0:
            print("[fail] flash failed (exit=%s)" % proc.returncode,
                  flush=True)
            return False
        self.event("flash_ok", mode=mode)
        return True

    def probe_port(self):
        exe = find_com_probe(self.repo_root)
        if exe is None:
            print("[fail] win32_com_probe.exe not found", flush=True)
            print("[hint] expected at build\\win32_probe\\"
                  "win32_com_probe.exe (build from pc/src/"
                  "win32_com_probe.cpp) or set WIN32_COM_PROBE", flush=True)
            return False
        cmd = [exe, "--port", self.args.port, "--baud", str(self.args.baud),
               "--pulse-reset", "--timeout-ms",
               str(self.args.probe_timeout_ms)]
        self.event("probe", exe=exe, port=self.args.port)
        print("  running: %s" % " ".join(cmd), flush=True)
        # MinGW-built probe needs the MSYS2 runtime DLLs on PATH (dev machine).
        env = dict(os.environ)
        for d in (r"C:\msys64\mingw64\bin", r"C:\msys64\usr\bin"):
            if os.path.isdir(d) and env.get("PATH") and d not in env["PATH"]:
                env["PATH"] = d + os.pathsep + env["PATH"]
        proc = subprocess.run(cmd, cwd=self.repo_root, env=env)
        if proc.returncode == 3:
            print("[fail] probe: COM port open failed: %s"
                  % self.args.port, flush=True)
            return False
        if proc.returncode != 0:
            print("[fail] probe failed (exit=%s)" % proc.returncode,
                  flush=True)
            return False
        self.event("probe_ok", port=self.args.port)
        return True

    # ---- per-mode experiment ----
    def run_mode(self, mode, iteration):
        profile = MODE_PROFILES[mode]
        base, spec = get_profile(profile)
        rf_on = spec["attrs"][4] == "ON"
        print("=" * 60, flush=True)
        print("== G1 mode %s: %s (profile=build/%s, RF=%s) =="
              % (mode, spec["label"], profile, "ON" if rf_on else "OFF"),
              flush=True)
        self.open_log(mode, iteration)
        try:
            if self.args.build and not self.build_mode(mode):
                return ("setup_fail", "build")
            if self.args.flash and not self.flash_mode(mode):
                return ("setup_fail", "flash")
            if self.args.probe and not self.probe_port():
                return ("setup_fail", "probe")
            if not self._open_port():
                return ("setup_fail", "uart_open")
            if not self.reset_chip():
                return ("fail", "uart_disconnect")
            try:
                self.ser.reset_input_buffer()
            except serial.SerialException:
                pass
            attempts = self.args.max_reopens + 1
            ok = False
            reason = ""
            for attempt in range(attempts):
                self.hello_info = None
                self.hello_ok = False
                self.hello_ms = None
                self.hello_t = None
                self.stream = Stream()
                self.phases = []
                self.scan_phase1_first = None
                self.scan_phase1_last = None
                if attempt > 0:
                    self.reopens += 1
                    self.event("uart_reopen", attempt=attempt)
                    self._close_port()
                    time.sleep(self.args.reopen_delay)
                    if not self._open_port():
                        return ("fail", "uart_reopen_open_failed")
                    if not self.reset_chip():
                        return ("fail", "uart_disconnect")
                if not self.do_handshake(self.args.hello_timeout):
                    reason = getattr(self, "handshake_fail_reason",
                                     "hello_timeout")
                    continue
                if rf_on:
                    res, why = self.one_scan_attempt(self.args.scan_timeout)
                    if res != "ok":
                        reason = why
                        if why != "uart_disconnect":
                            break
                        continue
                ok = True
                reason = ""
                break
            if not ok:
                self.event("fail", reason=reason)
                return ("fail", reason)
            self._pump(self.args.post_settle)
        finally:
            self._close_port()
            self.close_log()

        unexpected_reboots = sum(1 for rb in self.reboots
                                 if not rb["expected"])
        records = self.scan_result["records"] if self.scan_result else []
        rssis = [rec["rssi"] for rec in records]
        channels = sorted({rec["channel"] for rec in records})
        summary = {
            "mode": mode,
            "profile": profile,
            "rf_expected": "ON" if rf_on else "OFF",
            "boot_ok": 1 if (self.hello_ok or self.reboots) else 0,
            "ch340_open_ok": 1,
            "hello_ok": 1 if self.hello_ok else 0,
            "hello_ms": ("%.1f" % self.hello_ms) if self.hello_ms else "",
            "rf_start_seen": 1 if self.scan_phase1_first is not None else 0,
            "rf_start_ms": ("%.1f" % ((self.scan_phase1_first -
                                       self.last_reset_t) * 1000)
                            if self.scan_phase1_first is not None else ""),
            "got_ip_seen": 1 if self.got_ip_t is not None else 0,
            "got_ip_ms": ("%.1f" % ((self.got_ip_t -
                                     self.last_reset_t) * 1000)
                          if self.got_ip_t is not None else ""),
            "scan_req_tx": 1 if rf_on else 0,
            "ack_rx": self.rx["ack"],
            "ack_status": self.ack["status"] if self.ack else "",
            "scan_result_rx": self.rx["wifi_scan_result"],
            "scan_count": self.scan_result["count"] if self.scan_result else 0,
            "scan_total": self.scan_result["total"] if self.scan_result else 0,
            "scan_truncated": (1 if self.scan_result
                               and self.scan_result["truncated"] else 0),
            "rssi_min": min(rssis) if rssis else "",
            "rssi_max": max(rssis) if rssis else "",
            "channels": ",".join(str(c) for c in channels),
            "uart_disconnects": self.uart_disconnects,
            "readfile_errors": self.readfile_errors,
            "write_errors": self.write_errors,
            "reopens": self.reopens,
            "reboots_expected": sum(1 for rb in self.reboots
                                    if rb["expected"]),
            "reboots_unexpected": unexpected_reboots,
            "session_transitions": len(self.session_states) - 1,
            "peer_timeouts": self.peer_timeouts,
            "packets_rx": self.stream.stats["packets"],
            "crc_errors": self.stream.stats["crc_errors"],
            "bad_magic": self.stream.stats["bad_magic"],
            "protocol_errors": self.stream.stats["protocol_errors"],
        }
        print("## G1 mode %s summary" % mode, flush=True)
        self.summary(summary)
        # ---- verdict ----
        warnings = []
        if self.hello_ms and self.hello_ms > self.args.hello_warn_ms:
            warnings.append("hello_ms=%s" % summary["hello_ms"])
        if unexpected_reboots > 0:
            warnings.append("reboots_unexpected=%d" % unexpected_reboots)
        if self.stream.stats["crc_errors"] > 0:
            warnings.append("crc_errors=%d" % self.stream.stats["crc_errors"])
        if self.stream.stats["protocol_errors"] > 0:
            warnings.append("protocol_errors=%d"
                            % self.stream.stats["protocol_errors"])
        fail = False
        if not self.hello_ok:
            fail = True
        if rf_on:
            if self.scan_phase1_first is None:
                warnings.append("rf_start_seen=0 (expected for RF-ON)")
                fail = True
            if self.scan_result is None:
                warnings.append("scan_result_rx=0 (expected for RF-ON)")
                fail = True
        else:
            if self.scan_phase1_first is not None:
                warnings.append("rf_start_seen=1 (unexpected for RF-OFF)")
                fail = True
        if self.args.strict and warnings:
            fail = True
        if warnings:
            print("# warnings: %s" % ", ".join(warnings), flush=True)
        if fail:
            self.event("mode_fail", mode=mode)
            return ("fail", "; ".join(warnings) if warnings else "criteria")
        self.event("mode_pass", mode=mode)
        return ("pass", None)

    # ---- top level ----
    def run(self):
        print("== ESPView M7-G G1 harness (OLED x RF matrix) ==", flush=True)
        self.summary({
            "started_iso": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "port": self.args.port,
            "baud": self.args.baud,
            "modes": ",".join(self.args.modes),
            "iterations": self.args.iterations,
            "build": 1 if self.args.build else 0,
            "flash": 1 if self.args.flash else 0,
            "probe": 1 if self.args.probe else 0,
            "strict": 1 if self.args.strict else 0,
        })
        results = {}
        setup_failed = False
        for mode in self.args.modes:
            for it in range(1, self.args.iterations + 1):
                if self.args.iterations > 1:
                    print("---- G1 mode %s iteration %d/%d ----" %
                          (mode, it, self.args.iterations), flush=True)
                outcome, reason = self.run_mode(mode, it)
                results.setdefault(mode, []).append(outcome)
                if outcome == "setup_fail":
                    setup_failed = True
                    break
            if setup_failed:
                break
        for mode in self.args.modes:
            if mode not in results:
                continue
            outs = results[mode]
            if len(outs) > 1:
                print("## aggregate mode=%s iterations=%d" % (mode, len(outs)),
                      flush=True)
                print("# failures=%d" % sum(1 for o in outs if o != "pass"),
                      flush=True)
        verdicts = {}
        for mode in self.args.modes:
            outs = results.get(mode, [])
            if setup_failed and not outs:
                verdicts[mode] = "SETUP"
            elif outs and all(o == "pass" for o in outs):
                verdicts[mode] = "PASS"
            else:
                verdicts[mode] = "FAIL"
        line = " == RESULT: " + " ".join("%s=%s" % (m, verdicts[m])
                                         for m in self.args.modes) + " =="
        print(line, flush=True)
        if setup_failed:
            return 2
        return 0 if all(v == "PASS" for v in verdicts.values()) else 1


def dry_run(args):
    print("== ESPView M7-G G1 harness (dry-run) ==", flush=True)
    print("# port=%s baud=%d build=%s flash=%s probe=%s strict=%s" %
          (args.port, args.baud, args.build, args.flash, args.probe,
           args.strict), flush=True)
    for mode in args.modes:
        profile = MODE_PROFILES[mode]
        base, spec = get_profile(profile)
        print("mode %s -> profile build/%s (%s)" % (mode, profile,
                                                    spec["label"]),
              flush=True)
        print("  attrs: %s" % profile_line(profile), flush=True)
        if args.build:
            print("  build : espview_profile_sdkconfig.py --apply %s + "
                  "idf.py -B build/%s -D SDKCONFIG=build/%s/sdkconfig build"
                  % (profile, profile, profile), flush=True)
        if args.flash:
            print("  flash : espview_flash.bat -p %s -b %s --no-reset" %
                  (args.port, profile), flush=True)
        if args.probe:
            print("  probe : win32_com_probe.exe --port %s --baud %d "
                  "--pulse-reset --timeout-ms %d" %
                  (args.port, args.baud, args.probe_timeout_ms), flush=True)
        print("  test  : %s @ %d -> reset -> HELLO -> %s" %
              (args.port, args.baud,
               "WIFI_SCAN_REQ(maxEntries=%d)" % args.max_entries
               if spec["attrs"][4] == "ON" else "observe only (RF OFF)"),
              flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="ESPView M7-G G1 hardware harness: OLED x RF matrix "
                    "(profiles g1_a..g1_d)")
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--modes", default="A,B,C,D",
                    help="comma-separated mode list, default A,B,C,D")
    ap.add_argument("--iterations", type=int, default=1,
                    help="repeat each mode N times (default 1)")
    ap.add_argument("--build", action="store_true",
                    help="build each profile before testing (default: skip, "
                         "avoids parallel-agent clashes)")
    ap.add_argument("--flash", action="store_true",
                    help="flash each profile before testing (default: skip, "
                         "assume already flashed)")
    ap.add_argument("--probe", action="store_true",
                    help="run win32_com_probe --pulse-reset first "
                         "(CH340 link check)")
    ap.add_argument("--probe-timeout-ms", type=int, default=8000)
    ap.add_argument("--hello-timeout", type=float, default=15.0)
    ap.add_argument("--hello-warn-ms", type=float, default=3000.0,
                    help="warn if HELLO takes longer than this (ms)")
    ap.add_argument("--scan-timeout", type=float, default=30.0)
    ap.add_argument("--ack-timeout", type=float, default=5.0)
    ap.add_argument("--post-settle", type=float, default=3.0)
    ap.add_argument("--peer-timeout", type=float, default=6.5)
    ap.add_argument("--max-entries", type=int, default=32,
                    help="WIFI_SCAN_REQ maxEntries (1..64, default 32)")
    ap.add_argument("--max-reopens", type=int, default=0)
    ap.add_argument("--reopen-delay", type=float, default=2.0)
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--result-file", default=None,
                    help="append the structured output to this file "
                         "(default stdout only)")
    ap.add_argument("--log-dir", default=None,
                    help="raw log dir (default <repo>/build/g1_logs, "
                         "gitignored)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan only; touch nothing")
    args = ap.parse_args()

    if not (args.port.upper().startswith("COM") and
            args.port.upper()[3:].isdigit()):
        print("error: port must be COMx (e.g. COM4), got: %s" % args.port)
        return 2
    modes = [m.strip().upper() for m in args.modes.split(",") if m.strip()]
    bad = [m for m in modes if m not in MODE_PROFILES]
    if bad:
        print("error: unknown mode(s) %s (choose from A,B,C,D)" % bad)
        return 2
    if not modes:
        print("error: --modes is empty")
        return 2
    if args.iterations < 1 or args.max_entries < 1 or args.max_entries > 64:
        print("error: iterations>=1, max_entries in 1..64")
        return 2
    if args.max_reopens < 0:
        print("error: max_reopens>=0")
        return 2
    if args.probe_timeout_ms < 100:
        print("error: probe_timeout_ms>=100")
        return 2
    args.modes = modes

    if args.result_file:
        out = open(args.result_file, "a", encoding="utf-8")
        sys.stdout = Tee(sys.stdout, out)
    try:
        if args.dry_run:
            return dry_run(args)
        h = G1Harness(args)
        return h.run()
    except KeyboardInterrupt:
        print("\n== RESULT: ABORTED ==", flush=True)
        return 2
    finally:
        if args.result_file:
            sys.stdout.flush()
            sys.stdout = sys.__stdout__


if __name__ == "__main__":
    sys.exit(main())
