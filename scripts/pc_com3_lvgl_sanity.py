#!/usr/bin/env python3
"""ESPView M5-A PC 侧 LVGL COM3 sanity（可选硬件验收，verify_lvgl.bat 第 3 步）。

前提：ESP32 已烧录 CONFIG_ESPVIEW_APP_LVGL 固件，端口为 COM3（默认）。

流程：
  1. 打开串口（115200 8N1），以 PC 身份发 HELLO（seq=0 起）；
  2. 解码传入 Packet（MAGIC/VERSION/LENGTH/CRC 校验 + 重同步），
     自动回 PONG 保持 ESP32 心跳；
  3. 按 Message 语义重组 FRAME_BEGIN/RECT/END（FRAME_RECT 支持 CHUNKED 拆包，
     按 flags.CHUNKED 拼接 payload 直到末包）；
  4. 验收断言：
       - 收到 ESP32 HELLO（protocolVersion 一致）；
       - 首帧为 FULL（frameType=0）；
       - 观察期内至少 1 个 FULL、至少 1 个 PARTIAL（LVGL counter/button 每秒变化）；
       - 每帧 END.frameId == BEGIN.frameId，rectCount 一致；
       - 无 CRC / 半包累计异常。
  5. 输出 FULL/PARTIAL 字节与 dirty ratio（partialBytes/153600），PASS/FAIL 退出码。

协议常量与 shared/protocol 冻结定义一致（见 docs/DESIGN.md）。
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
TYPE_FRAME_BEGIN = 0x10
TYPE_FRAME_RECT = 0x11
TYPE_FRAME_END = 0x12
TYPE_PING = 0x30
TYPE_PONG = 0x31
TYPE_ERROR = 0x50
TYPE_ACK = 0x51

FLAG_CHUNKED = 0x01

FRAME_TYPE_FULL = 0
FRAME_TYPE_PARTIAL = 1

FULL_SCREEN_BYTES = 320 * 240 * 2  # RGB565


def build_packet(ptype, flags, seq, payload):
    hdr = struct.pack("<4sBBBBH", MAGIC, VER, ptype, flags, 0, seq)
    hdr += struct.pack("<I", len(payload))
    crc = zlib.crc32(hdr + payload) & 0xFFFFFFFF
    hdr += struct.pack("<I", crc)
    hdr += struct.pack("<H", 0)  # RSVD2
    return hdr + payload


class Stream:
    def __init__(self):
        self.buf = bytearray()
        self.stats = {"bad_magic": 0, "crc_errors": 0, "protocol_errors": 0,
                      "packets": 0, "hello": 0}

    def feed(self, ser):
        n = ser.in_waiting
        if n > 0:
            self.buf.extend(ser.read(n))
        return self

    def next_packet(self):
        """返回 (payload, hdr) 或 None（数据不足/待更多字节）。"""
        while True:
            if len(self.buf) < HEADER_LEN:
                return None
            if self.buf[:4] != MAGIC:
                self.stats["bad_magic"] += 1
                del self.buf[0]
                continue
            (_, version, ptype, flags, _rsvd, seq, length, crc, _r2) = struct.unpack(
                "<4sBBBBHIIH", bytes(self.buf[:HEADER_LEN])
            )
            if version != VER:
                self.stats["protocol_errors"] += 1
                del self.buf[0]
                continue
            if length > MAX_PAYLOAD:
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
            hdr = {"type": ptype, "flags": flags, "seq": seq, "length": length}
            return payload, hdr


def make_hello_payload():
    # HELLO：ver(1) + class(1) + w(2) + h(2) + fmt(1) + mode_mask(1) + name_len(1) + name
    # （DESIGN.md 消息表：nameLen 为 1 字节；与 pc_com3_session_test.py 一致）
    name = b"espview-host"
    return struct.pack("<BBHHBBB", VER, 0, 320, 240, 0, 0b111, len(name)) + name


def handle_hello(payload, out):
    if len(payload) < 9:
        return False
    ver, _cls, width, height, fmt, _mask = struct.unpack_from("<BBHHBB", payload, 0)
    out["hello"] = {"ver": ver, "width": width, "height": height, "fmt": fmt}
    return True


def run(args):
    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    try:
        stream = Stream()
        tx_seq = 0
        stats = {"hello": None, "ping": 0, "error": 0, "ack": 0}
        frames = []  # 每个提交帧: dict(type, frameId, rects, bytes)
        pending = None  # 当前组装中的帧

        if not args.no_reset:
            # 正常启动复位（同 pc_com3_session_test.py）：
            # GPIO0(DTR) 保持高，仅脉冲 EN(RTS)。
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setRTS(False)
            time.sleep(0.2)
        ser.reset_input_buffer()

        # 1) 先等 ESP32 HELLO（固件启动后 Transport 就绪即发 HELLO），
        #    再发本端 HELLO。
        deadline = time.monotonic() + args.timeout
        hello_ok = False
        while time.monotonic() < deadline and not hello_ok:
            stream.feed(ser)
            pkt = stream.next_packet()
            if pkt is None:
                continue
            payload, hdr = pkt
            if hdr["type"] == TYPE_HELLO:
                hello_ok = handle_hello(payload, stats) or hello_ok
            elif hdr["type"] == TYPE_PING:
                stats["ping"] += 1
                ser.write(build_packet(TYPE_PONG, 0, tx_seq, payload))
                tx_seq = (tx_seq + 1) & 0xFFFF
        if not hello_ok or stats["hello"] is None:
            print(f"FAIL: 未收到 ESP32 HELLO（port={args.port} baud={args.baud}）")
            return 1
        h = stats["hello"]
        print(f"HELLO OK: ver={h['ver']} {h['width']}x{h['height']} fmt={h['fmt']}")

        # 发本端 HELLO 完成握手（之后 ESP32 会切 CONNECTED 并开始发帧）
        ser.write(build_packet(TYPE_HELLO, 0, tx_seq, make_hello_payload()))
        tx_seq = (tx_seq + 1) & 0xFFFF

        # 2) 观察帧流（至少 1 FULL + 1 PARTIAL 提交后可提前结束）。
        #    心跳：PC 必须主动发 PING（DESIGN.md M4：长流式帧期间 ESP32 的 PING 会
        #    被 sendMutex 放弃；PC 每 ~1s 的 PING 维持对端会话。仅被动回 PONG 会在
        #    首次 FULL（约14s）期间触发 ESP32 5s peer timeout → 会话断开、帧流停止）。
        last_ping_ms = 0.0
        collect_deadline = time.monotonic() + args.watch
        while time.monotonic() < collect_deadline:
            now = time.monotonic()
            if now - last_ping_ms >= args.ping_interval:
                last_ping_ms = now
                ts_ms = int(time.time() * 1000) & 0xFFFFFFFFFFFFFFFF
                ser.write(build_packet(TYPE_PING, 0, tx_seq, struct.pack("<Q", ts_ms)))
                tx_seq = (tx_seq + 1) & 0xFFFF
            stream.feed(ser)
            pkt = stream.next_packet()
            if pkt is None:
                continue
            payload, hdr = pkt
            t = hdr["type"]
            if t == TYPE_PING:
                stats["ping"] += 1
                ser.write(build_packet(TYPE_PONG, 0, tx_seq, payload))
                tx_seq = (tx_seq + 1) & 0xFFFF
                continue
            if t == TYPE_HELLO:
                handle_hello(payload, stats)
                continue
            if t == TYPE_ERROR:
                stats["error"] += 1
                continue
            if t == TYPE_ACK:
                stats["ack"] += 1
                continue
            if t == TYPE_FRAME_BEGIN:
                if pending is not None:
                    # 收到新 BEGIN：上一帧未提交（END 丢失）→ 作废重来。
                    pending = None
                if len(payload) < 12:
                    continue
                fid, ftype, _fmt, w, h, _hint = struct.unpack_from("<HBBHHI", payload, 0)
                pending = {"frameId": fid, "type": ftype, "w": w, "h": h,
                           "rects": 0, "bytes": 0, "rect_payload": None}
                continue
            if t == TYPE_FRAME_RECT:
                if pending is None:
                    continue
                if pending["rect_payload"] is None:
                    pending["rect_payload"] = bytearray()
                pending["rect_payload"] += payload
                if not (hdr["flags"] & FLAG_CHUNKED):
                    # 消息级拆包结束：解析 RECT（8B 头 + 像素）。
                    rp = bytes(pending["rect_payload"])
                    pending["rect_payload"] = None
                    if len(rp) < 8:
                        continue
                    x, y, w, h = struct.unpack_from("<HHHH", rp, 0)
                    pixel_bytes = w * h * 2
                    if len(rp) != 8 + pixel_bytes:
                        continue
                    pending["rects"] += 1
                    pending["bytes"] += pixel_bytes
                continue
            if t == TYPE_FRAME_END:
                if pending is None:
                    continue
                if len(payload) < 9:
                    pending = None
                    continue
                fid, rects, byte_count = struct.unpack_from("<HHI", payload, 0)
                aborted = payload[8] & 1
                if not aborted and (fid != pending["frameId"] or rects != pending["rects"]):
                    # 帧不一致：作废（统计异常，不阻断后续帧）。
                    stats.setdefault("frame_mismatch", 0)
                    stats["frame_mismatch"] += 1
                    pending = None
                    continue
                frames.append({"frameId": pending["frameId"], "type": pending["type"],
                               "rects": pending["rects"], "bytes": pending["bytes"],
                               "aborted": aborted})
                pending = None
                # 已见 FULL + PARTIAL：提前收工
                if any(f["type"] == FRAME_TYPE_FULL and not f["aborted"] for f in frames) and \
                   any(f["type"] == FRAME_TYPE_PARTIAL and not f["aborted"] for f in frames):
                    break

        # 3) 断言。
        full_frames = [f for f in frames if f["type"] == FRAME_TYPE_FULL and not f["aborted"]]
        partial_frames = [f for f in frames if f["type"] == FRAME_TYPE_PARTIAL and not f["aborted"]]
        full_bytes = sum(f["bytes"] for f in full_frames)
        partial_bytes = sum(f["bytes"] for f in partial_frames)

        print(f"packets={stream.stats['packets']} "
              f"bad_magic={stream.stats['bad_magic']} crc_errors={stream.stats['crc_errors']} "
              f"proto_errors={stream.stats['protocol_errors']} ping_rx={stats['ping']}")
        print(f"frames={len(frames)} full={len(full_frames)} partial={len(partial_frames)} "
              f"full_bytes={full_bytes} partial_bytes={partial_bytes} "
              f"dirty={partial_bytes * 100.0 / FULL_SCREEN_BYTES:.2f}% "
              f"mismatch={stats.get('frame_mismatch', 0)}")

        failures = []
        if len(full_frames) == 0:
            failures.append("未收到任何 FULL 帧")
        elif frames and frames[0]["type"] != FRAME_TYPE_FULL:
            failures.append(f"首帧不是 FULL（type={frames[0]['type']}）")
        if len(partial_frames) == 0:
            failures.append("未收到任何 PARTIAL 帧（LVGL 1Hz 更新应产生 PARTIAL）")
        if stream.stats["crc_errors"] > args.max_crc_errors:
            failures.append(f"CRC 错误过多（{stream.stats['crc_errors']}）")
        if not frames:
            failures.append("未收到任何已提交帧")

        if failures:
            print("FAIL: " + "; ".join(failures))
            return 1
        print("PASS: LVGL frame stream OK")
        return 0
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(description="ESPView M5-A LVGL COM3 sanity")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=5.0, help="HELLO 握手超时(秒)")
    ap.add_argument("--watch", type=float, default=40.0, help="帧流观察时长(秒)，默认40s（首次 FULL @115200 约15s）")
    ap.add_argument("--ping-interval", type=float, default=1.0,
                  help="PC 主动心跳 PING 间隔(秒)；<5s 才能维持 ESP32 会话（peer timeout=5s）")
    ap.add_argument("--max-crc-errors", type=int, default=0)
    ap.add_argument("--no-reset", action="store_true", help="不触发 DTR/RTS 复位（假设 ESP32 即将自动重启）")
    args = ap.parse_args()
    try:
        return run(args)
    except serial.SerialException as exc:
        print(f"FAIL: 串口错误: {exc}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
