#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ESPView M7-E Agent D -- hardware A/B/C experiment harness (repeatable, diffable).

Runs the same over-the-air Wi-Fi scan against three firmware behaviors for the
OLED during the scan (protocol frozen: only WIFI_SCAN_REQ, never WIFI_CONFIG,
zero password involvement):

  mode A: OLED active    + scan (CONFIG_ESPVIEW_SCAN_SUSPEND_OLED=n)
  mode B: OLED suspended + scan (CONFIG_ESPVIEW_SCAN_SUSPEND_OLED=y, default)
  mode C: OLED disabled  + scan (CONFIG_ESPVIEW_OLED_ENABLE=n)

Per-mode flow (optional steps):
  1. build the profile (equivalent idf.py flow: PowerShell loads the ESP-IDF
     profile, then idf.py -B build\\<profile> -D SDKCONFIG_DEFAULTS="..." build;
     gated behind --build, skipped by default to avoid parallel-agent clashes);
  2. flash (scripts\\espview_flash.bat -b <profile> -p <port> --no-reset;
     gated behind --flash, skipped by default);
  3. open COM4 @ 115200 -> DTR/RTS reset -> wait for ESP32 HELLO ->
     reply with PC HELLO (includes the nameLen field, DESIGN.md message table)
     -> session CONNECTED;
  4. send WIFI_SCAN_REQ (ACK_REQ, maxEntries=32) -> record ACK, WIFI_STATUS
     phases (kScanning=1 start/end), WIFI_SCAN_RESULT (count/RSSI/channel);
  5. continuously record: OLED diagnostic lines (ERROR text channel
     "oled a=.. err=.. ok=.."), PHYSICAL_PREVIEW counts (A streams during
     scan / B pauses while suspended / C none), UART connect/disconnect,
     ReadFile errors, ESP32 reset banner (rst:), session state, ACK, protocol
     statistics.

Security constraints:
  - never reads esp32/sdkconfig (build only uses a whitelisted override
    defaults file under %TEMP%);
  - never sends or records Wi-Fi credentials (SSID is non-secret metadata,
    same as the existing probe);
  - test results are printed to stdout / the user-supplied --result-file only
    (no repo writes by default).

Output conventions (repeatable, diffable):
  - events : [evt] t=+<rel seconds> kind=<kind> k=v ...
  - summary: # key=value (one block per mode, keys sorted, one field per line)
  - verdict: == RESULT: A=PASS B=PASS C=PASS ==
  Use 'grep -v " t=+"' to strip timing and diff only the structural fields.

Usage:
  python espview_e_ab_harness.py [--port COM4] [--baud 115200]
                                 [--modes A,B,C] [--iterations 1]
                                 [--build] [--flash]
                                 [--max-reopens 0] [--strict]
                                 [--result-file PATH] [--dry-run]
Exit codes: 0 all PASS / 1 any mode FAIL / 2 usage, environment or build/flash
failure.
"""

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import time
import zlib

try:
    import serial
except ImportError as exc:  # pragma: no cover
    sys.exit("missing pyserial: %s (py -3.10 -m pip install pyserial)" % exc)

# ---- protocol constants (shared/protocol frozen, same as pc_com3_*.py) ----
MAGIC = b"ESPV"
VER = 1
HEADER_LEN = 20
MAX_PAYLOAD = 4096

TYPE_HELLO = 0x01
TYPE_CAPABILITIES = 0x02
TYPE_SET_MODE = 0x03
TYPE_WIFI_SCAN_REQ = 0x06
TYPE_WIFI_SCAN_RESULT = 0x07
TYPE_WIFI_CONFIG = 0x08
TYPE_WIFI_STATUS = 0x09
TYPE_FRAME_BEGIN = 0x10
TYPE_FRAME_RECT = 0x11
TYPE_FRAME_END = 0x12
TYPE_PHYSICAL_PREVIEW = 0x13
TYPE_PING = 0x30
TYPE_PONG = 0x31
TYPE_ERROR = 0x50
TYPE_ACK = 0x51

FLAG_ACK_REQ = 0x02

SCAN_RESULT_RECORD_BYTES = 42

WIFI_STATUS_PHASES = {0: "idle", 1: "scanning", 2: "config_applying",
                      3: "wifi_connecting", 4: "wifi_connected", 5: "got_ip",
                      6: "tcp_connecting", 7: "tcp_connected", 8: "error",
                      9: "cleared"}

SESSION_NAMES = {0: "disconnected", 1: "connecting", 2: "handshake",
                 3: "connected"}

# ---- A/B/C mode definitions (whitelisted Kconfig keys only; values y/n) ----
ALLOWED_CONFIG_KEYS = ("CONFIG_ESPVIEW_OLED_ENABLE",
                       "CONFIG_ESPVIEW_SCAN_SUSPEND_OLED")

MODE_SPECS = {
    "A": {
        "profile": "mode_a",
        "label": "OLED active + scan (SCAN_SUSPEND_OLED=n)",
        "oled_config": "active",
        "config": {
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_SCAN_SUSPEND_OLED": "n",
        },
    },
    "B": {
        "profile": "mode_b",
        "label": "OLED suspended + scan (SCAN_SUSPEND_OLED=y, default)",
        "oled_config": "suspended",
        "config": {
            "CONFIG_ESPVIEW_OLED_ENABLE": "y",
            "CONFIG_ESPVIEW_SCAN_SUSPEND_OLED": "y",
        },
    },
    "C": {
        "profile": "mode_c",
        "label": "OLED disabled + scan (OLED_ENABLE=n)",
        "oled_config": "disabled",
        "config": {
            "CONFIG_ESPVIEW_OLED_ENABLE": "n",
        },
    },
}

DEFAULT_ESPIDF_PROFILE = r"C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"


def ps_quote(path):
    """Embed a Windows path safely into a PowerShell single-quoted string."""
    return "'" + path.replace("'", "''") + "'"
def build_packet(ptype, flags, seq, payload):
    hdr = struct.pack("<4sBBBBH", MAGIC, VER, ptype, flags, 0, seq)
    hdr += struct.pack("<I", len(payload))
    crc = zlib.crc32(hdr + payload) & 0xFFFFFFFF
    hdr += struct.pack("<I", crc)
    hdr += struct.pack("<H", 0)  # RSVD2
    return hdr + payload


def parse_hello(payload):
    """ESP32 HELLO: ver(1) class(1) w(2) h(2) fmt(1) mask(1) nameLen(1) name(..)."""
    if len(payload) < 9:
        return None
    ver, cls = payload[0], payload[1]
    width, height = struct.unpack_from("<HH", payload, 2)
    fmt, mask, name_len = payload[6], payload[7], payload[8]
    if name_len > 32 or len(payload) < 9 + name_len:
        return None
    return {"ver": ver, "cls": cls, "width": width, "height": height,
            "fmt": fmt, "mask": mask,
            "name": payload[9:9 + name_len].decode("utf-8", errors="replace")}


def parse_ack(payload):
    """ACK: ackSeq(2 LE) status(1) errorCode(2 LE)."""
    if len(payload) < 5:
        return None
    ack_seq, status = struct.unpack_from("<HB", payload, 0)
    err = struct.unpack_from("<H", payload, 3)[0]
    return {"ack_seq": ack_seq, "status": status, "err": err}


def parse_wifi_status(payload):
    """WIFI_STATUS: phase(1) errCode(2) flags(1) rssi(1) ch(1) ip(4) sip(4)
    port(2) ssidLen(1) ssid(..)."""
    if len(payload) < 17:
        return None
    phase = payload[0]
    err_code = struct.unpack_from("<H", payload, 1)[0]
    flags = payload[3]
    rssi = struct.unpack_from("<b", payload, 4)[0]
    channel = payload[5]
    ssid_len = payload[16]
    if ssid_len > 32 or len(payload) < 17 + ssid_len:
        return None
    ssid = payload[17:17 + ssid_len].decode("utf-8", errors="replace")
    return {"phase": phase, "phase_name": WIFI_STATUS_PHASES.get(phase, "?"),
            "err_code": err_code, "flags": flags, "rssi": rssi,
            "channel": channel, "ssid": ssid}


def parse_scan_result(payload):
    """WIFI_SCAN_RESULT: scanSeq(1) count(1) flags(1) total(2 LE) + records*42.
    record: ssid[32] bssid[6] rssi(1 @38) channel(1 @39) authmode(1 @40) rsvd(1)."""
    if len(payload) < 5:
        return None
    count = payload[1]
    if count > 64 or len(payload) < 5 + count * SCAN_RESULT_RECORD_BYTES:
        return None
    total = struct.unpack_from("<H", payload, 3)[0]
    records = []
    for i in range(count):
        o = 5 + i * SCAN_RESULT_RECORD_BYTES
        ssid = payload[o:o + 32].split(b"\x00", 1)[0].decode(
            "utf-8", errors="replace")
        rssi = struct.unpack_from("<b", payload, o + 38)[0]
        channel = payload[o + 39]
        auth = payload[o + 40]
        records.append({"ssid": ssid, "rssi": rssi, "channel": channel,
                        "auth": auth})
    return {"scan_seq": payload[0], "count": count,
            "truncated": bool(payload[2] & 0x01), "total": total,
            "records": records}


def parse_error_text(payload):
    """ERROR: errCode(2 LE) msgLen(1) text(..)."""
    if len(payload) < 3:
        return None
    code = struct.unpack_from("<H", payload, 0)[0]
    msg_len = payload[2]
    text = payload[3:3 + msg_len].decode("utf-8", errors="replace").strip()
    return {"code": code, "text": text}


class Stream:
    """Packet parser: resync + CRC check + stats (same as pc_com3_session_test.py)."""

    def __init__(self):
        self.buf = bytearray()
        self.stats = {"bad_magic": 0, "crc_errors": 0, "protocol_errors": 0,
                      "packets": 0}

    def feed(self, data):
        self.buf.extend(data)

    def next_packet(self):
        while True:
            if len(self.buf) < HEADER_LEN:
                return None
            if self.buf[:4] != MAGIC:
                self.stats["bad_magic"] += 1
                del self.buf[0]
                continue
            (_, version, ptype, flags, _rsvd, seq, length, crc, _r2) = \
                struct.unpack("<4sBBBBHIIH", bytes(self.buf[:HEADER_LEN]))
            if version != VER or ptype < 1 or ptype > 0x51 or length > MAX_PAYLOAD:
                self.stats["protocol_errors"] += 1
                del self.buf[0]
                continue
            total = HEADER_LEN + length
            if len(self.buf) < total:
                return None
            payload = bytes(self.buf[HEADER_LEN:total])
            calc = zlib.crc32(bytes(self.buf[:14]) + payload) & 0xFFFFFFFF
            if calc != crc:
                self.stats["crc_errors"] += 1
                del self.buf[0]
                continue
            del self.buf[:total]
            self.stats["packets"] += 1
            return payload, {"type": ptype, "flags": flags, "seq": seq,
                             "length": length}
class Harness:
    def __init__(self, args):
        self.args = args
        self.repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
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
        self.session_state = 0  # kDisconnected
        self.session_states = [(0.0, "disconnected")]
        self.hello_info = None
        self.hello_ok = False
        self.hello_ms = None
        self.scan_req_seq = None
        self.ack = None
        self.ack_ms = None
        self.scan_result = None
        self.scan_req_t = None
        self.scan_result_t = None
        self.scan_phase1_first = None
        self.scan_phase1_last = None
        self.phases = []
        self.oled = {"lines": 0, "err_first": None, "err_last": None,
                     "ok_last": None, "first_t": None, "last_t": None}
        self.mem_lines = 0
        self.trx_last = ""
        self.preview_times = []
        self.reboots = []
        self.uart_disconnects = 0
        self.readfile_errors = 0
        self.write_errors = 0
        self.peer_timeouts = 0
        self.reopens = 0
        self.last_rx = 0.0
        self.last_tx_ping = 0.0
        self.tx_ping = 0
        self.last_reboot_t = -10.0
        self.last_reset_t = -10.0
        self.scan_window = None  # (t_rel_start, t_rel_end)

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

    def summary(self, kv):
        for k in sorted(kv):
            print("# %s=%s" % (k, kv[k]), flush=True)

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
            if info["ver"] != VER:
                self.event("hello_rx", version=info["ver"], ok=0,
                           why="version_mismatch")
                return
            self.event("hello_rx", version=info["ver"],
                       size="%dx%d" % (info["width"], info["height"]),
                       name=info["name"])
        elif t == TYPE_CAPABILITIES:
            self.rx["capabilities"] += 1
        elif t == TYPE_PING:
            self.rx["ping"] += 1
            self._tx(TYPE_PONG, 0, payload)  # echo PONG, keep ESP32 heartbeat
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
            self.preview_times.append(self.t_rel())
        elif t == TYPE_FRAME_BEGIN:
            self.rx["frame_begin"] += 1
        elif t == TYPE_ERROR:
            self.rx["error"] += 1
            e = parse_error_text(payload)
            if e is None:
                self.stream.stats["protocol_errors"] += 1
                return
            self._handle_diag(e["text"])
        elif t in (TYPE_SET_MODE, TYPE_WIFI_SCAN_REQ, TYPE_WIFI_CONFIG):
            pass  # PC->ESP echoes should not appear; ignore
        else:
            self.rx["unexpected"] += 1

    def _handle_diag(self, text):
        if text.startswith("oled"):
            self.oled["lines"] += 1
            vals = {}
            for tok in text.split():
                for key in ("a=", "c=", "err=", "ok="):
                    if tok.startswith(key):
                        vals[key] = tok[len(key):]
            now = self.t_rel()
            if self.oled["first_t"] is None:
                self.oled["first_t"] = now
            self.oled["last_t"] = now
            if "err=" in vals:
                try:
                    cur = int(vals["err="])
                    if self.oled["err_first"] is None:
                        self.oled["err_first"] = cur
                    self.oled["err_last"] = cur
                except ValueError:
                    pass
            if "ok=" in vals:
                self.oled["ok_last"] = vals["ok="]
            self.event("oled_diag", text=text)
        elif text.startswith("trx"):
            self.trx_last = text
            self.event("trx_diag", text=text)
        elif text.startswith("mem"):
            self.mem_lines += 1
            self.event("mem_diag", text=text)
        else:
            self.event("error_text", text=text)

    # ---- heartbeat ----
    def _heartbeat_tick(self):
        if self.session_state != 3:
            return
        now = self.t_rel()
        if now - self.last_tx_ping >= self.args.heartbeat:
            self.last_tx_ping = now
            self.tx_ping += 1
            ts = int(time.monotonic() * 1000) & 0xFFFFFFFFFFFFFFFF
            self._tx(TYPE_PING, 0, struct.pack("<Q", ts))

    # ---- main read loop ----
    def _pump(self, timeout_s):
        """Read the serial port and dispatch packets.
        Returns "ok" or "disconnected" (a ReadFile-level failure occurred)."""
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
            self._heartbeat_tick()
            time.sleep(0.002)
        return "ok"
    # ---- HELLO handshake ----
    def do_handshake(self, timeout_s):
        """Wait for the ESP32 HELLO (send ours after 5s if not seen), then reply
        with the PC HELLO (includes the nameLen field). Returns True/False."""
        self._set_session(1, "handshake_start")  # kConnecting
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
                name = b"espview-ab"
                payload = struct.pack("<BBHHBBB", VER, 0, 320, 240, 0, 0b111,
                                      len(name)) + name
                self._tx(TYPE_HELLO, 0, payload)
                self.event("hello_tx", name=name.decode(), name_len=len(name))
                self.seq = 0  # handshake done: both sides reset seq
        if not hello_seen:
            self.handshake_fail_reason = "hello_timeout"
            self.event("fail", reason="hello_timeout", timeout=timeout_s)
            return False
        self.hello_ok = True
        self.hello_ms = (self.t_rel() - self.last_reset_t) * 1000
        if not mine_sent:
            name = b"espview-ab"
            payload = struct.pack("<BBHHBBB", VER, 0, 320, 240, 0, 0b111,
                                  len(name)) + name
            self._tx(TYPE_HELLO, 0, payload)
            self.event("hello_tx", name=name.decode(), name_len=len(name))
            self.seq = 0
        self._set_session(3, "handshake_ok")  # kConnected
        return True

    # ---- WIFI_SCAN_REQ + result ----
    def send_scan_req(self):
        payload = struct.pack("<BB", 0, self.args.max_entries)
        self.scan_req_t = self.t_rel()
        self.scan_req_seq = self._tx(TYPE_WIFI_SCAN_REQ, FLAG_ACK_REQ, payload)
        self.event("scan_req_tx", seq=self.scan_req_seq,
                   max_entries=self.args.max_entries)

    def one_scan_attempt(self, scan_timeout_s):
        """One scan attempt: REQ -> ACK -> RESULT.
        Returns ("ok", None) or ("fail", reason)."""
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
        if self.scan_phase1_first is not None:
            self.scan_window = (self.scan_phase1_first, self.scan_phase1_last)
        else:
            self.scan_window = (self.scan_req_t, self.scan_result_t)
        return ("ok", None)

    # ---- build / flash ----
    def _write_config_overrides(self, mode):
        spec = MODE_SPECS[mode]
        lines = ["# ESPView M7-E A/B/C harness -- whitelisted config override "
                 "(temp file, never committed)"]
        for key in sorted(spec["config"]):
            val = spec["config"][key]
            if key not in ALLOWED_CONFIG_KEYS or val not in ("y", "n"):
                print("[fail] illegal override: %s=%s" % (key, val), flush=True)
                return None
            lines.append("%s=%s" % (key, val))
        content = "\n".join(lines) + "\n"
        digest = hashlib.sha1(content.encode("utf-8")).hexdigest()[:8]
        path = os.path.join(tempfile.gettempdir(),
                            "espview_abharness_mode%s_%s.defaults" %
                            (mode, digest))
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(content)
        return path

    def build_mode(self, mode):
        spec = MODE_SPECS[mode]
        esp32_dir = os.path.join(self.repo_root, "esp32")
        base_defaults = os.path.join(esp32_dir, "sdkconfig.defaults")
        if not os.path.exists(base_defaults):
            print("[fail] missing %s" % base_defaults, flush=True)
            return False
        overrides = self._write_config_overrides(mode)
        if overrides is None:
            return False
        idf_profile = os.environ.get("ESPIDF_PROFILE", DEFAULT_ESPIDF_PROFILE)
        if not os.path.exists(idf_profile):
            print("[fail] ESP-IDF profile not found: %s" % idf_profile,
                  flush=True)
            return False
        defaults_join = "%s;%s" % (base_defaults, overrides)
        ps = "\n".join([
            "$ErrorActionPreference = 'Stop'",
            "try {",
            "  . %s" % ps_quote(idf_profile),
            "  if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) "
            "{ Write-Host '[build] idf.py not available'; exit 3 }",
            "  Set-Location %s" % ps_quote(esp32_dir),
            "  $env:ESPVIEW_SDKCONFIG = %s" % ps_quote(defaults_join),
            "  idf.py -B build/%s -D SDKCONFIG_DEFAULTS=\"$env:ESPVIEW_SDKCONFIG\" build"
            % spec["profile"],
            "  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }",
            "} catch { Write-Host ('[build] ERROR: ' + $_.Exception.Message); "
            "exit 1 }",
        ])
        self.event("build", mode=mode, profile=spec["profile"],
                   config=",".join("%s=%s" % (k, v) for k, v in
                                   sorted(spec["config"].items())))
        print("  running: idf.py -B build/%s -D SDKCONFIG_DEFAULTS=... build"
              % spec["profile"], flush=True)
        proc = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy",
                               "Bypass", "-Command", ps], cwd=self.repo_root)
        try:
            os.unlink(overrides)
        except OSError:
            pass
        if proc.returncode != 0:
            print("[fail] build failed (exit=%s)" % proc.returncode, flush=True)
            return False
        self.event("build_ok", mode=mode)
        return True

    def flash_mode(self, mode):
        spec = MODE_SPECS[mode]
        bat = os.path.join(self.scripts_dir, "espview_flash.bat")
        if not os.path.exists(bat):
            print("[fail] missing %s" % bat, flush=True)
            return False
        self.event("flash", mode=mode, profile=spec["profile"],
                   port=self.args.port)
        proc = subprocess.run(["cmd", "/c", bat, "-p", self.args.port,
                               "-b", spec["profile"], "--no-reset"],
                              cwd=self.repo_root)
        if proc.returncode != 0:
            print("[fail] flash failed (exit=%s)" % proc.returncode, flush=True)
            return False
        self.event("flash_ok", mode=mode)
        return True
    # ---- per-mode experiment ----
    def run_mode(self, mode):
        spec = MODE_SPECS[mode]
        print("=" * 60, flush=True)
        print("== mode %s: %s (profile=build/%s) ==" % (mode, spec["label"],
                                                        spec["profile"]),
              flush=True)
        if self.args.build and not self.build_mode(mode):
            return ("setup_fail", "build")
        if self.args.flash and not self.flash_mode(mode):
            return ("setup_fail", "flash")
        if not self._open_port():
            return ("setup_fail", "uart_open")
        try:
            if not self.reset_chip():
                self.event("fail", reason="uart_disconnect")
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
                self.stream = Stream()
                self.preview_times = []
                self.phases = []
                self.scan_phase1_first = None
                self.scan_phase1_last = None
                self.scan_window = None
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
                res, why = self.one_scan_attempt(self.args.scan_timeout)
                if res == "ok":
                    ok = True
                    reason = ""
                    break
                reason = why
                if why != "uart_disconnect":
                    break
            if not ok:
                self.event("fail", reason=reason)
                return ("fail", reason)
            # settle window: collect post-scan OLED / preview evidence
            self._pump(self.args.post_settle)
        finally:
            self._close_port()

        r = self.scan_result
        rssis = [rec["rssi"] for rec in r["records"]]
        chans = sorted({rec["channel"] for rec in r["records"]})
        before = [t for t in self.preview_times
                  if self.scan_window and t < self.scan_window[0]]
        during = [t for t in self.preview_times
                  if self.scan_window and self.scan_window[0] <= t <= self.scan_window[1]]
        after = [t for t in self.preview_times
                 if self.scan_window and t > self.scan_window[1]]
        oled_delta = None
        if self.oled["err_first"] is not None and self.oled["err_last"] is not None:
            oled_delta = self.oled["err_last"] - self.oled["err_first"]
        # OLED-state evidence
        if spec["oled_config"] == "disabled":
            observed = ("disabled" if self.oled["lines"] == 0
                        else "unexpected-present")
        else:
            if self.oled["lines"] > 0:
                if spec["oled_config"] == "active":
                    observed = "active" if len(during) > 0 else "active-no-preview"
                else:
                    observed = ("suspended" if len(during) == 0
                                else "suspended-with-preview")
            else:
                observed = "absent"
        unexpected_reboots = sum(1 for rb in self.reboots if not rb["expected"])
        warnings = []
        if self.readfile_errors > 0:
            warnings.append("readfile_errors=%d" % self.readfile_errors)
        if unexpected_reboots > 0:
            warnings.append("unexpected_reboots=%d" % unexpected_reboots)
        if self.stream.stats["crc_errors"] > 0:
            warnings.append("crc_errors=%d" % self.stream.stats["crc_errors"])
        if self.stream.stats["protocol_errors"] > 0:
            warnings.append("protocol_errors=%d" %
                            self.stream.stats["protocol_errors"])
        fail = False
        if self.args.strict and warnings:
            fail = True
            reason = "strict:" + ",".join(warnings)
        elif reason:
            fail = True
        result = "FAIL" if fail else "PASS"
        if reason:
            result += "(%s)" % reason
        kv = {
            "mode": mode,
            "profile": spec["profile"],
            "label": spec["label"],
            "result": result,
            "reason": reason or "ok",
            "hello_ok": 1 if self.hello_ok else 0,
            "hello_version": self.hello_info["ver"] if self.hello_info else "",
            "hello_device": self.hello_info["name"] if self.hello_info else "",
            "hello_wxh": ("%dx%d" % (self.hello_info["width"],
                                     self.hello_info["height"])
                          if self.hello_info else ""),
            "hello_ms": "%.0f" % self.hello_ms if self.hello_ms is not None else "",
            "ack_ok": 1 if (self.ack and self.ack["status"] == 0) else 0,
            "ack_seq": self.ack["ack_seq"] if self.ack else "",
            "ack_status": self.ack["status"] if self.ack else "",
            "ack_err_code": self.ack["err"] if self.ack else "",
            "ack_ms": "%.1f" % self.ack_ms if self.ack_ms is not None else "",
            "scan_seq": r["scan_seq"],
            "scan_count": r["count"],
            "scan_total": r["total"],
            "scan_truncated": 1 if r["truncated"] else 0,
            "scan_duration_ms": "%.1f" %
                ((self.scan_result_t - self.scan_req_t) * 1000),
            "rssi_min": min(rssis) if rssis else "",
            "rssi_max": max(rssis) if rssis else "",
            "rssi_avg": "%.1f" % (sum(rssis) / len(rssis)) if rssis else "",
            "channels": ",".join(str(c) for c in chans),
            "wifi_status_phases": ",".join(str(p[1]) for p in self.phases),
            "scan_esp_phase1_ms": ("%.1f" % ((self.scan_phase1_last -
                                              self.scan_phase1_first) * 1000)
                                   if self.scan_phase1_first is not None else ""),
            "oled_config": spec["oled_config"],
            "oled_lines": self.oled["lines"],
            "oled_err_first": (self.oled["err_first"]
                               if self.oled["err_first"] is not None else ""),
            "oled_err_last": (self.oled["err_last"]
                              if self.oled["err_last"] is not None else ""),
            "oled_err_delta": oled_delta if oled_delta is not None else "",
            "oled_ok_last": self.oled["ok_last"] or "",
            "oled_state_observed": observed,
            "preview_before": len(before),
            "preview_during": len(during),
            "preview_after": len(after),
            "uart_disconnects": self.uart_disconnects,
            "readfile_errors": self.readfile_errors,
            "write_errors": self.write_errors,
            "reopens": self.reopens,
            "reboots_expected": sum(1 for rb in self.reboots if rb["expected"]),
            "reboots_unexpected": unexpected_reboots,
            "session_transitions": ",".join(s for _, s in self.session_states),
            "peer_timeouts": self.peer_timeouts,
            "rx_packets": self.stream.stats["packets"],
            "rx_ping": self.rx["ping"],
            "rx_pong": self.rx["pong"],
            "tx_ping": self.tx_ping,
            "bad_magic": self.stream.stats["bad_magic"],
            "crc_errors": self.stream.stats["crc_errors"],
            "protocol_errors": self.stream.stats["protocol_errors"],
            "mem_lines": self.mem_lines,
            "trx_last": self.trx_last or "",
        }
        self.summary(kv)
        self.event("mode_result", mode=mode, result=result)
        return ("pass" if not fail else "fail", reason)
    def run(self):
        print("== ESPView M7-E A/B/C harness ==", flush=True)
        self.summary({
            "started_iso": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "port": self.args.port,
            "baud": self.args.baud,
            "modes": ",".join(self.args.modes),
            "iterations": self.args.iterations,
            "build": 1 if self.args.build else 0,
            "flash": 1 if self.args.flash else 0,
            "strict": 1 if self.args.strict else 0,
            "scan_timeout": self.args.scan_timeout,
            "max_reopens": self.args.max_reopens,
        })
        results = {}
        setup_failed = False
        for mode in self.args.modes:
            for it in range(1, self.args.iterations + 1):
                if self.args.iterations > 1:
                    print("---- mode %s iteration %d/%d ----" %
                          (mode, it, self.args.iterations), flush=True)
                outcome, reason = self.run_mode(mode)
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
    print("== ESPView M7-E A/B/C harness (dry-run) ==", flush=True)
    print("# port=%s baud=%d build=%s flash=%s strict=%s" %
          (args.port, args.baud, args.build, args.flash, args.strict),
          flush=True)
    for mode in args.modes:
        spec = MODE_SPECS[mode]
        print("mode %s -> profile build/%s (%s)" % (mode, spec["profile"],
                                                    spec["label"]), flush=True)
        print("  config overrides: %s" %
              ", ".join("%s=%s" % (k, v) for k, v in
                        sorted(spec["config"].items())), flush=True)
        if args.build:
            print("  build : idf.py -B build/%s "
                  "-D SDKCONFIG_DEFAULTS=<repo sdkconfig.defaults>;<temp override> build"
                  % spec["profile"], flush=True)
        if args.flash:
            print("  flash : espview_flash.bat -p %s -b %s --no-reset" %
                  (args.port, spec["profile"]), flush=True)
        print("  test  : %s @ %d -> reset -> HELLO -> WIFI_SCAN_REQ(maxEntries=%d) -> record"
              % (args.port, args.baud, args.max_entries), flush=True)
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="ESPView M7-E hardware A/B/C experiment "
                    "(OLED active/suspended/disabled + Wi-Fi scan)")
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--modes", default="A,B,C",
                    help="comma-separated mode list, default A,B,C")
    ap.add_argument("--iterations", type=int, default=1,
                    help="repeat each mode N times (default 1)")
    ap.add_argument("--build", action="store_true",
                    help="build the profile before testing (default: skip, "
                         "avoids parallel-agent clashes)")
    ap.add_argument("--flash", action="store_true",
                    help="flash the profile before testing (default: skip, "
                         "assume already flashed)")
    ap.add_argument("--hello-timeout", type=float, default=12.0)
    ap.add_argument("--scan-timeout", type=float, default=30.0)
    ap.add_argument("--ack-timeout", type=float, default=5.0)
    ap.add_argument("--post-settle", type=float, default=3.0,
                    help="seconds of extra observation after the scan "
                         "(OLED/preview evidence)")
    ap.add_argument("--heartbeat", type=float, default=2.0,
                    help="PC heartbeat PING interval seconds (peer timeout=5s)")
    ap.add_argument("--peer-timeout", type=float, default=6.5,
                    help="record a session timeout after RX silence (s)")
    ap.add_argument("--max-entries", type=int, default=32,
                    help="WIFI_SCAN_REQ maxEntries (1..64, default 32)")
    ap.add_argument("--max-reopens", type=int, default=0,
                    help="max reopen+re-handshake attempts after UART "
                         "disconnect (default 0 = fail on first disconnect)")
    ap.add_argument("--reopen-delay", type=float, default=2.0)
    ap.add_argument("--strict", action="store_true",
                    help="fail on any ReadFile error / unexpected reboot / "
                         "CRC / protocol error")
    ap.add_argument("--result-file", default=None,
                    help="append the structured output to this file "
                         "(recommend an out-of-repo temp path; default stdout only)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan only; touch nothing")
    args = ap.parse_args()

    if not (args.port.upper().startswith("COM") and
            args.port.upper()[3:].isdigit()):
        print("error: port must be COMx (e.g. COM4), got: %s" % args.port)
        return 2
    modes = [m.strip().upper() for m in args.modes.split(",") if m.strip()]
    bad = [m for m in modes if m not in MODE_SPECS]
    if bad:
        print("error: unknown mode(s) %s (choose from A,B,C)" % bad)
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
    args.modes = modes

    if args.result_file:
        out = open(args.result_file, "a", encoding="utf-8")
        tee = Tee(sys.stdout, out)
        sys.stdout = tee
    try:
        if args.dry_run:
            return dry_run(args)
        h = Harness(args)
        return h.run()
    except KeyboardInterrupt:
        print("\n== RESULT: ABORTED ==", flush=True)
        return 2
    finally:
        if args.result_file:
            sys.stdout.flush()
            sys.stdout = sys.__stdout__


class Tee:
    def __init__(self, a, b):
        self.a = a
        self.b = b

    def write(self, data):
        self.a.write(data)
        self.b.write(data)

    def flush(self):
        self.a.flush()
        self.b.flush()


if __name__ == "__main__":
    sys.exit(main())
