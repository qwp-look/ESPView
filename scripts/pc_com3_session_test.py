#!/usr/bin/env python3
"""ESPView M1-2 PC 侧会话验收脚本（验证 ProtocolEndpoint + 真实 UART）。

流程：
  1. HELLO 握手：本端发 HELLO，接收 ESP32 HELLO，校验 protocolVersion；
     握手完成后双方 seq 清零（DESIGN.md 连接状态机）。
  2. PING/PONG：连续 100 次 PC PING -> ESP32 PONG（要求 100/100、0 CRC 错误、
     0 协议错误、无意外断开）；期间对 ESP32 主动 PING 回 PONG（保持心跳）。
  3. SET_MODE：WINDOW/DEVICE/MIRROR 每种 10 次，要求 ACK 全部成功（status=0）。

协议（shared/protocol 冻结，与 C++ 实现一致）：
  Packet = 20B header + payload。
  header: MAGIC 'ESPV'(4) + VERSION(1) + TYPE(1) + FLAGS(1) + RSVD(1)
          + SEQ(2,LE) + LENGTH(4,LE) + CRC32(4,LE) + RSVD2(2)。
  CRC32 = zlib/IEEE，覆盖 header[0:14)+payload，wire LE。
  TYPE: HELLO=0x01 SET_MODE=0x03 PING=0x30 PONG=0x31 ERROR=0x50 ACK=0x51。
  FLAGS: bit0=CHUNKED bit1=ACK_REQ。

用法：
  python pc_com3_session_test.py --port COM3 --baud 115200 [--no-reset]
"""

import argparse
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError as exc:  # pragma: no cover
    sys.exit(f"缺少 pyserial: {exc}")

MAGIC = b"ESPV"
VER = 1
HEADER_LEN = 20
MAX_PAYLOAD = 4096

TYPE_HELLO = 0x01
TYPE_SET_MODE = 0x03
TYPE_PING = 0x30
TYPE_PONG = 0x31
TYPE_ERROR = 0x50
TYPE_ACK = 0x51

FLAG_CHUNKED = 0x01
FLAG_ACK_REQ = 0x02

MODES = [0, 1, 2]  # WINDOW / DEVICE / MIRROR


def build_packet(ptype, flags, seq, payload):
    hdr = struct.pack("<4sBBBBH", MAGIC, VER, ptype, flags, 0, seq)
    hdr += struct.pack("<I", len(payload))
    crc = zlib.crc32(hdr + payload) & 0xFFFFFFFF
    hdr += struct.pack("<I", crc)
    hdr += struct.pack("<H", 0)  # RSVD2
    return hdr + payload


def parse_packet(buf, stats):
    """从 buf 头部尝试解析一个包。返回 (packet, header) 或 None（数据不足）。
    失败（坏 MAGIC/长度/CRC）时消费 1 字节并统计。"""
    while True:
        if len(buf) < HEADER_LEN:
            return None
        if buf[:4] != MAGIC:
            stats["bad_magic"] += 1
            del buf[0]
            continue
        (_, version, ptype, flags, _rsvd, seq, length, crc, _r2) = struct.unpack(
            "<4sBBBBHIIH", bytes(buf[:HEADER_LEN])
        )
        if version != VER:
            stats["protocol_errors"] += 1
            del buf[0]
            continue
        if length > MAX_PAYLOAD:
            stats["protocol_errors"] += 1
            del buf[0]
            continue
        total = HEADER_LEN + length
        if len(buf) < total:
            return None
        payload = bytes(buf[HEADER_LEN:total])
        calc = zlib.crc32(bytes(buf[:14]) + payload) & 0xFFFFFFFF
        if calc != crc:
            stats["crc_errors"] += 1
            del buf[0]
            continue
        del buf[:total]
        hdr = {"type": ptype, "flags": flags, "seq": seq, "length": length}
        return payload, hdr


def read_available(ser, buf, stats):
    n = ser.in_waiting
    if n > 0:
        buf.extend(ser.read(n))
    return buf


def wait_packet(ser, buf, stats, timeout_s, expect_type, what):
    """等待指定类型包（期间自动回 PONG 给 ESP32 PING、处理 HELLO/ACK）。"""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        read_available(ser, buf, stats)
        pkt = parse_packet(buf, stats)
        if pkt is None:
            time.sleep(0.002)
            continue
        payload, hdr = pkt
        if hdr["type"] == expect_type:
            return payload, hdr
        if hdr["type"] == TYPE_PING:
            # 回 PONG（回显时间戳），保持 ESP32 心跳
            stats["rx_ping"] += 1
            tx_packet(ser, stats, TYPE_PONG, 0, payload)
            continue
        if hdr["type"] == TYPE_PONG:
            stats["rx_pong"] += 1
            continue
        if hdr["type"] == TYPE_HELLO:
            stats["rx_hello"] += 1
            handle_hello(payload, stats)
            continue
        if hdr["type"] == TYPE_ACK:
            handle_ack(payload, stats)
            continue
        if hdr["type"] == TYPE_ERROR:
            stats["rx_error"] += 1
            continue
        stats["unexpected"] += 1
        stats["unexpected_types"].append(hdr["type"])
    raise TimeoutError(f"等待 {what} 超时 ({timeout_s}s)")


def tx_packet(ser, stats, ptype, flags, payload):
    seq = stats["seq"]
    stats["seq"] = (stats["seq"] + 1) & 0xFFFF
    stats["tx_count"] += 1
    ser.write(build_packet(ptype, flags, seq, payload))


def handle_hello(payload, stats):
    if len(payload) < 9:
        stats["protocol_errors"] += 1
        return
    ver, dev_class = payload[0], payload[1]
    width, height = struct.unpack_from("<HH", payload, 2)
    pixfmt, mode_mask, name_len = payload[6], payload[7], payload[8]
    if ver != VER:
        stats["protocol_errors"] += 1
        stats["hello_ok"] = False
        return
    if len(payload) != 9 + name_len:
        stats["protocol_errors"] += 1
        stats["hello_ok"] = False
        return
    stats["hello_ok"] = True
    stats["hello"] = {
        "ver": ver,
        "class": dev_class,
        "width": width,
        "height": height,
        "pixfmt": pixfmt,
        "mode_mask": mode_mask,
        "name": payload[9:9 + name_len].decode("utf-8", "replace"),
    }
    # 注意：本端 seq 不在收 HELLO 时清零——DESIGN.md「握手完成双方 seq 清零」指
    # 握手后的下一包从 0 开始，由 main() 在本端 HELLO 发送后显式重置。


def handle_ack(payload, stats):
    if len(payload) < 5:
        stats["protocol_errors"] += 1
        return
    ack_seq, status, err = struct.unpack_from("<H B H", payload, 0)
    stats["acks"].append((ack_seq, status, err))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--no-reset", action="store_true", help="不触发 DTR/RTS 复位")
    ap.add_argument("--ping-count", type=int, default=100)
    ap.add_argument("--setmode-per-mode", type=int, default=10)
    args = ap.parse_args()

    stats = {
        "seq": 0,
        "tx_count": 0,
        "rx_hello": 0,
        "rx_ping": 0,
        "rx_pong": 0,
        "rx_error": 0,
        "crc_errors": 0,
        "bad_magic": 0,
        "protocol_errors": 0,
        "unexpected": 0,
        "unexpected_types": [],
        "hello_ok": False,
        "hello": None,
        "acks": [],
    }

    print(f"opening {args.port} @ {args.baud} 8N1")
    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    ser.reset_input_buffer()
    if not args.no_reset:
        # 正常启动复位：GPIO0 必须保持高（DTR=False）；仅脉冲 EN（RTS）。
        # DTR=True + RTS 脉冲会拉低 GPIO0 进入下载模式（boot:0x3）。
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)
        time.sleep(0.2)

    buf = bytearray()

    # ---- HELLO 握手 ----
    # 先等 ESP32 的 HELLO，再发本端 HELLO：固件启动后 Transport 一就绪即发 HELLO；
    # 若先发 PC HELLO，可能落在 ESP32 UART driver 就绪前被丢弃，导致 ESP32 握手超时。
    print("[H] waiting for ESP32 HELLO ...")
    try:
        payload, hdr = wait_packet(ser, buf, stats, 10.0, TYPE_HELLO, "HELLO")
    except TimeoutError as exc:
        print(f"[H] FAIL: {exc}")
        ser.close()
        return 1
    # wait_packet 命中 expect_type 时直接返回、不会调用 handle_hello，这里显式校验。
    handle_hello(payload, stats)
    if not stats["hello_ok"]:
        print("[H] FAIL: HELLO 校验失败")
        ser.close()
        return 1
    h = stats["hello"]
    print(f"[H] PASS: ver={h['ver']} {h['width']}x{h['height']} "
          f"fmt={h['pixfmt']} modeMask=0x{h['mode_mask']:02X} name={h['name']!r}")
    print("[H] sending HELLO ...")
    hello_payload = struct.pack("<BBHHBBB", VER, 0, 320, 240, 0, 0b111, len(b"esptest")) + b"esptest"
    tx_packet(ser, stats, TYPE_HELLO, 0, hello_payload)
    # 握手完成：双方 packet.seq 清零（DESIGN.md），握手后的第一包 PING 从 seq=0 开始。
    stats["seq"] = 0

    # ---- PING/PONG ×100 ----
    print(f"[P] PING/PONG x{args.ping_count} ...")
    ok = 0
    deadline_total = time.monotonic() + args.ping_count * 0.5 + 15
    for i in range(1, args.ping_count + 1):
        ts = int(time.monotonic() * 1000) & 0xFFFFFFFFFFFFFFFF
        tx_packet(ser, stats, TYPE_PING, 0, struct.pack("<Q", ts))
        try:
            payload, hdr = wait_packet(ser, buf, stats, 1.0, TYPE_PONG, f"PING#{i} PONG")
        except TimeoutError:
            print(f"[P] FAIL at PING#{i}: no PONG")
            ser.close()
            return 1
        ok += 1
        if i % 10 == 0:
            print(f"[P] PING#{i} -> PONG#{i}")
        if time.monotonic() > deadline_total:
            break
    print(f"[P] ping_ok={ok}/{args.ping_count}")
    if ok != args.ping_count:
        print("[P] FAIL: 未达到 100/100")
        ser.close()
        return 1

    # ---- SET_MODE 每模式 ×10 ----
    print(f"[M] SET_MODE x{args.setmode_per_mode} per mode ...")
    mode_ok = {}
    for mode in MODES:
        mode_ok[mode] = 0
        for j in range(1, args.setmode_per_mode + 1):
            stats["acks"].clear()
            setmode_seq = stats["seq"]
            tx_packet(ser, stats, TYPE_SET_MODE, FLAG_ACK_REQ, struct.pack("<B", mode))
            try:
                payload, hdr = wait_packet(ser, buf, stats, 1.0, TYPE_ACK, f"SET_MODE({mode})#{j} ACK")
            except TimeoutError:
                print(f"[M] FAIL: mode={mode} #{j} 无 ACK")
                ser.close()
                return 1
            # ACK.ackSeq 应等于刚发出的 SET_MODE 包 seq
            ack_seq, status, err = struct.unpack_from("<H B H", payload, 0)
            if status != 0:
                print(f"[M] FAIL: mode={mode} #{j} ACK status={status} err={err}")
                ser.close()
                return 1
            if ack_seq != setmode_seq:
                print(f"[M] FAIL: mode={mode} #{j} ackSeq={ack_seq} != setmode_seq={setmode_seq}")
                ser.close()
                return 1
            mode_ok[mode] += 1
        print(f"[M] mode={mode}: {mode_ok[mode]}/{args.setmode_per_mode} ACK OK")

    # ---- 汇总 ----
    print("==" * 20)
    print(f"== RESULT: "
          f"hello=PASS ping={ok}/{args.ping_count} "
          f"setmode=" + ",".join(f"{m}:{mode_ok[m]}" for m in MODES))
    print(f"== stats: crc_errors={stats['crc_errors']} bad_magic={stats['bad_magic']} "
          f"protocol_errors={stats['protocol_errors']} unexpected={stats['unexpected']} "
          f"rx_ping={stats['rx_ping']} rx_pong={stats['rx_pong']} "
          f"rx_hello={stats['rx_hello']} rx_error={stats['rx_error']}")
    ser.close()
    if (ok == args.ping_count and all(mode_ok[m] == args.setmode_per_mode for m in MODES)
            and stats["crc_errors"] == 0 and stats["protocol_errors"] == 0
            and stats["unexpected"] == 0):
        print("== ALL PASS ==")
        return 0
    print("== SOME CHECKS FAILED ==")
    return 1


if __name__ == "__main__":
    sys.exit(main())
