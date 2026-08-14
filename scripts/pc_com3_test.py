#!/usr/bin/env python3
"""ESPView M1-1 PC 侧 COM3 测试工具（验证 UartTransport + 真实 UART）。

阶段 A：验证 ESP32 启动后发送的固定 pattern 序列：
    00 55 AA FF  00 01 02 .. FF(256B)  伪随机 1024B(xorshift32 seed=0x12345678)
阶段 B：接收 PING(TYPE=0x30) -> 回复 PONG(TYPE=0x31)，统计往返。

协议（shared/protocol，冻结）：
  Packet = 20B header + payload。
  header: MAGIC 'ESPV'(4) + VERSION(1)=1 + TYPE(1) + FLAGS(1) + RSVD(1)
          + SEQ(2, LE) + LENGTH(4, LE) + CRC32(4, LE) + RSVD2(2)
  CRC32 = zlib/IEEE, 覆盖 header[0:14] + payload，wire LE。

用法：
  python pc_com3_test.py [--port COM3] [--baud 921600]
                         [--phase-a-timeout 6] [--phase-b-duration 30]
                         [--no-reset]

流程：打开串口后默认先触发一次 DTR/RTS 复位让 ESP32 从头开始跑 pattern；
若 --no-reset 则假设 ESP32 即将自动复位（如刚完成烧录）。
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
TYPE_PING = 0x30
TYPE_PONG = 0x31
HEADER_LEN = 20
MAX_PAYLOAD = 4096

# 与 ESP32 main.cpp 相同的伪随机生成器。
def xorshift_bytes(n: int, seed: int = 0x12345678) -> bytes:
    out = bytearray()
    x = seed & 0xFFFFFFFF
    for _ in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= (x >> 17) & 0xFFFFFFFF
        x ^= (x << 5) & 0xFFFFFFFF
        x &= 0xFFFFFFFF
        out.append(x & 0xFF)
    return bytes(out)


def read_all(ser, duration_s: float) -> bytearray:
    """阻塞读取：数据到达立即返回（跟上 921600 突发），无数据时短超时。"""
    data = bytearray()
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        chunk = ser.read(4096)
        if chunk:
            data.extend(chunk)
        else:
            time.sleep(0.001)
    return data


def build_packet(msg_type: int, payload: bytes, seq: int) -> bytes:
    header = MAGIC + bytes([VER, msg_type, 0, 0]) + struct.pack("<H", seq) + struct.pack(
        "<I", len(payload))
    crc = zlib.crc32(header + payload) & 0xFFFFFFFF
    return header + struct.pack("<I", crc) + b"\x00\x00"


def parse_packet(buf: bytes):
    """从 buf 头部尝试解析一个包。返回 (consumed, type, seq, payload, ok_crc) 或 None。"""
    i = buf.find(MAGIC)
    if i < 0:
        return None  # 无 MAGIC，等更多数据
    if i > 0:
        # 丢弃 MAGIC 前的垃圾（如 ROM boot 输出）。
        return (i, None, None, None, None)  # 特殊标记：跳过 i 字节
    if len(buf) < HEADER_LEN:
        return None
    ver, typ, flags, rsvd = buf[4], buf[5], buf[6], buf[7]
    seq = struct.unpack("<H", buf[8:10])[0]
    length = struct.unpack("<I", buf[10:14])[0]
    crc = struct.unpack("<I", buf[14:18])[0]
    if ver != VER or typ < 1 or typ > 0x51 or length > MAX_PAYLOAD:
        # 头部不可信：丢弃 1 字节继续扫描。
        return (1, None, None, None, None)
    total = HEADER_LEN + length
    if len(buf) < total:
        return None  # 半包，等待更多
    payload = buf[HEADER_LEN:total]
    calc = zlib.crc32(buf[0:14] + payload) & 0xFFFFFFFF
    ok = calc == crc
    return (total, typ, seq, payload, ok)


def phase_a(ser, timeout_s: float) -> bool:
    """固定收集整个窗口（覆盖多轮 pattern，不做短静默 break），再滑动搜索完整 pattern。"""
    print(f"[A] collecting bytes (timeout {timeout_s:.0f}s) ...")
    data = read_all(ser, timeout_s)
    print(f"[A] received {len(data)} bytes")

    seq = bytes([0x00, 0x55, 0xAA, 0xFF]) + bytes(range(256)) + xorshift_bytes(1024)
    # 滑动搜索：可能多个候选起点（残留数据干扰），取第一个完整匹配。
    start = 0
    while True:
        idx = data.find(bytes([0x00, 0x55, 0xAA, 0xFF]), start)
        if idx < 0:
            break
        if len(data) - idx >= len(seq) and bytes(data[idx : idx + len(seq)]) == seq:
            print(f"[A] PASS: all {len(seq)} pattern bytes verified "
                  f"(candidate at {idx}, prefix {idx}B skipped)")
            return True
        start = idx + 1
    print(f"[A] FAIL: no complete pattern found in {len(data)} bytes")
    return False


def phase_b(ser, duration_s: float) -> bool:
    """接收 PING，回复 PONG，统计往返。"""
    print(f"[B] PING/PONG for {duration_s:.0f}s ...")
    buf = bytearray()
    local_seq = 0
    ping_rx = 0
    pong_tx = 0
    crc_errors = 0
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        chunk = ser.read(min(4096, max(1, int(remaining * 921600))))  # 阻塞读
        if chunk:
            buf.extend(chunk)
        # 循环解析缓冲。
        while True:
            r = parse_packet(bytes(buf))
            if r is None:
                break
            consumed, typ, _seq, payload, ok = r
            if typ is None:
                del buf[:consumed]  # 跳过垃圾
                continue
            del buf[:consumed]
            if not ok:
                crc_errors += 1
                print(f"[B] CRC error on type=0x{typ:02X}, skipping")
                continue
            if typ == TYPE_PING:
                ping_rx += 1
                pong = build_packet(TYPE_PONG, payload, local_seq)
                ser.write(pong)
                pong_tx += 1
                local_seq = (local_seq + 1) & 0xFFFF
                if ping_rx <= 5 or ping_rx % 10 == 0:
                    print(f"[B] PING#{ping_rx} -> PONG#{pong_tx}")
    ok = ping_rx > 0 and pong_tx == ping_rx and crc_errors == 0
    print(f"[B] ping_rx={ping_rx} pong_tx={pong_tx} crc_errors={crc_errors}")
    print(f"[B] {'PASS' if ok else 'FAIL'}")
    return ok


def reset_target(ser):
    """经典 ESP32 自动下载电路硬复位（RTS->EN 拉低再释放，正常运行，不进下载模式）。"""
    ser.setDTR(False)
    ser.setRTS(True)  # EN 拉低
    time.sleep(0.1)
    ser.setRTS(False)  # EN 释放 -> 正常运行
    time.sleep(0.3)
    print("[reset] ESP32 hard reset triggered")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=921600)
    ap.add_argument("--phase-a-timeout", type=float, default=6.0)
    ap.add_argument("--phase-b-duration", type=float, default=30.0)
    ap.add_argument("--no-reset", action="store_true", help="不主动复位（如刚烧录自动复位）")
    args = ap.parse_args()

    print(f"opening {args.port} @ {args.baud} 8N1")
    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    if not args.no_reset:
        reset_target(ser)

    ok_a = phase_a(ser, args.phase_a_timeout)
    ok_b = phase_b(ser, args.phase_b_duration)
    ser.close()
    print(f"== RESULT: phaseA={'PASS' if ok_a else 'FAIL'} phaseB={'PASS' if ok_b else 'FAIL'} ==")
    sys.exit(0 if (ok_a and ok_b) else 1)


if __name__ == "__main__":
    main()
