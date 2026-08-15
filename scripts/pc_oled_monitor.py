#!/usr/bin/env python3
"""ESPView M7-A/M7-B PC 侧 OLED/内存诊断监控脚本。

监听 ESP32 的 ERROR 文本通道（statsLoop 每 3s 上报），过滤诊断行：
  oled a=<addr> c=<ctrl> err=<errCount> ok=<0|1>
  mem  h=<freeHeap> lg=<largestBlock> mn=<minFreeHeap>   （M7-B 新增）

M7-B 变更：
  1. 打开串口后先做 HELLO 握手（与 pc_com3_session_test.py 同格式），
     否则会话停留在 Disconnected，sink 不发送任何诊断行；
  2. 监听期间自动回复 ESP32 的 PING -> PONG（回显时间戳），保持会话
     心跳（ProtocolEndpoint peer_timeout=5s，无响应即断开）；
  3. 新增 mem 行解析与堆趋势统计（freeHeap min/max/delta，检测泄漏/漂移）。

用途：
  1. 实机 I2C scan 结果确认（a=0x3C / c=SSD1306）；
  2. 30 分钟稳定性：oledErrorCount 不增长 / ok=1 / heap 无漂移；
  3. 与帧流并行运行，确认 OLED 不污染协议链路。

用法：
  python pc_oled_monitor.py --port COM4 --baud 115200 [--duration 1800] [--no-reset]
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


def build_packet(ptype, flags, seq, payload):
    hdr = struct.pack("<4sBBBBH", MAGIC, VER, ptype, flags, 0, seq)
    hdr += struct.pack("<I", len(payload))
    crc = zlib.crc32(hdr + payload) & 0xFFFFFFFF
    hdr += struct.pack("<I", crc)
    hdr += struct.pack("<H", 0)  # RSVD2
    return hdr + payload


def parse_packet(buf, stats):
    """从 buf 头部尝试解析一个包。返回 (body, ptype) 或 None（数据不足）。
    失败（坏 MAGIC/长度/CRC）时消费 1 字节并统计。"""
    while True:
        if len(buf) < HEADER_LEN:
            return None
        if buf[:4] != MAGIC:
            stats["bad_magic"] += 1
            del buf[0]
            continue
        (_, version, ptype, _flags, _rsvd, _seq, length, crc, _r2) = struct.unpack(
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
        body = bytes(buf[:total])
        expect = zlib.crc32(body[:14] + body[HEADER_LEN:]) & 0xFFFFFFFF
        got = struct.unpack("<I", body[14:18])[0]
        if expect != got:
            stats["crc_errors"] += 1
            del buf[0]
            continue
        del buf[:total]
        return body, ptype


def tx_packet(ser, stats, ptype, flags, payload):
    seq = stats["seq"]
    stats["seq"] = (stats["seq"] + 1) & 0xFFFF
    ser.write(build_packet(ptype, flags, seq, payload))


def do_hello(ser, stats, timeout_s=8.0):
    """HELLO 握手（与 pc_com3_lvgl_sanity.py 同序）：
    1) 先等 ESP32 启动后主动发出的 HELLO（固件 Transport 就绪即发）；
    2) 再发本端 HELLO（避免复位后 ESP32 未就绪时 PC HELLO 丢失，
       会话停留在 Handshake 导致 5s 后 peer timeout 静默）；
    3) 握手完成后双方 seq 清零（协议约定），本地 seq 归零。"""
    name = b"mon"
    hello_payload = struct.pack("<BBHHBBB", VER, 2, 320, 240, 0, 7, len(name)) + name
    deadline = time.monotonic() + timeout_s
    start_t = time.monotonic()
    buf = bytearray()
    sent_mine = False
    while time.monotonic() < deadline:
        data = ser.read(512)
        if data:
            buf.extend(data)
        while True:
            item = parse_packet(buf, stats)
            if item is None:
                break
            body, ptype = item
            if ptype == TYPE_HELLO:
                if len(body) < HEADER_LEN + 9:
                    stats["protocol_errors"] += 1
                    continue
                payload = body[HEADER_LEN:]
                if payload[0] != VER:
                    stats["protocol_errors"] += 1
                    print("# !! HELLO version mismatch", flush=True)
                    return False
                stats["hello_ok"] = True
                width, height = struct.unpack_from("<HH", payload, 2)
                print(f"# HELLO OK: ver={payload[0]} {width}x{height} fmt={payload[6]}",
                      flush=True)
                if not sent_mine:
                    tx_packet(ser, stats, TYPE_HELLO, 0, hello_payload)
                    sent_mine = True
                    stats["seq"] = 0  # 握手完成：双方 seq 清零
                return True
            if ptype == TYPE_PING:  # 握手期间保持心跳
                tx_packet(ser, stats, TYPE_PONG, 0, body[HEADER_LEN:])
                continue
        # 双阶段：先等 ESP32 主动 HELLO（刚复位）；约 3s 未到则主动发本端
        # HELLO（已运行/断线重连场景：ESP32 在 Disconnected 被动等待对端 HELLO）。
        if not sent_mine and (time.monotonic() - start_t) > 3.0:
            tx_packet(ser, stats, TYPE_HELLO, 0, hello_payload)
            sent_mine = True
            # ESP32 若在 kDisconnected 会回 HELLO；若在 kHandshake 则直接
            # completeHandshake（不回 HELLO）。因此发完本端 HELLO 后继续等
            # 至多 3s：收到 HELLO 即确认；超时按"已收到过启动 HELLO 或
            # 会话由后续 PING/PONG 验证"处理，不把无第二个 HELLO 当失败。
            reply_deadline = time.monotonic() + 3.0
            while time.monotonic() < reply_deadline:
                data = ser.read(512)
                if data:
                    buf.extend(data)
                item = parse_packet(buf, stats)
                if item is None:
                    continue
                body, ptype = item
                if ptype == TYPE_HELLO:
                    payload = body[HEADER_LEN:]
                    if len(payload) >= 9 and payload[0] == VER:
                        stats["hello_ok"] = True
                        width, height = struct.unpack_from("<HH", payload, 2)
                        print(
                            f"# HELLO OK(2nd): ver={payload[0]} {width}x{height} fmt={payload[6]}",
                            flush=True,
                        )
                        return True
                if ptype == TYPE_PING:
                    tx_packet(ser, stats, TYPE_PONG, 0, body[HEADER_LEN:])
                    continue
            return True  # 未收到第二个 HELLO 也接受（kHandshake 直接完成）
    print("# !! HELLO handshake timeout", flush=True)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--duration", type=float, default=0.0, help="秒；0=无限")
    ap.add_argument("--no-reset", action="store_true",
                    help="不发送 DTR/RTS 复位（默认复位让 ESP32 从头启动）")
    ap.add_argument("--no-hello", action="store_true",
                    help="跳过 HELLO 握手（会话未建立时不会收到诊断行）")
    args = ap.parse_args()

    stats = {
        "bad_magic": 0,
        "protocol_errors": 0,
        "crc_errors": 0,
        "error_msgs": 0,
        "oled_lines": 0,
        "mem_lines": 0,
        "rx_ping": 0,
        "rx_pong": 0,
        "hello_ok": False,
        "seq": 0,
    }
    first_oled = None
    last_oled = None
    last_err_count = None
    heap = {"free": [], "min": [], "largest": []}  # 记录趋势（每行一个样本）

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as exc:
        sys.exit(f"无法打开 {args.port}: {exc}")

    try:
        if not args.no_reset:
            ser.setDTR(False)
            ser.setRTS(True)
            time.sleep(0.1)
            ser.setRTS(False)
            time.sleep(0.2)
    except serial.SerialException:
        pass
    # 立即清空启动期噪声字节（boot ROM 输出等），随后 do_hello 会捕获
    # ESP32 应用启动后主动发出的 HELLO（同 pc_com3_lvgl_sanity.py 时序）。
    ser.reset_input_buffer()

    if not args.no_hello:
        if not do_hello(ser, stats):
            ser.close()
            print("# RESULT FAIL (hello handshake)", flush=True)
            sys.exit(1)

    print(f"# monitor {args.port} @ {args.baud} duration={args.duration or 'inf'}s",
          flush=True)
    start = time.monotonic()
    buf = bytearray()
    last_ping = 0.0
    try:
        while True:
            if args.duration and (time.monotonic() - start) >= args.duration:
                break
            now = time.monotonic()
            # 主动心跳（M4）：长流式帧（FULL 153600B @115200 ≈ 13.5s）期间
            # ESP32 自身 PING 被 sendMutex 放弃；PC 每 ~1s PING 维持对端会话，
            # 否则 ESP32 5s peer timeout 会在首帧期间断开（DESIGN.md M4）。
            if now - last_ping >= 1.0:
                last_ping = now
                ts_ms = int(time.time() * 1000) & 0xFFFFFFFFFFFFFFFF
                tx_packet(ser, stats, TYPE_PING, 0, struct.pack("<Q", ts_ms))
            data = ser.read(512)
            if not data:
                continue
            buf.extend(data)
            while True:
                item = parse_packet(buf, stats)
                if item is None:
                    break
                body, ptype = item
                payload = body[HEADER_LEN:]
                if ptype == TYPE_ERROR:
                    # ERROR 负载布局：u16 errorCode + u8 msgLen + text（DESIGN.md）。
                    if len(payload) >= 3:
                        msg_len = payload[2]
                        text = payload[3:3 + msg_len].decode(
                            "utf-8", errors="replace"
                        ).strip()
                    else:
                        text = ""
                    stats["error_msgs"] += 1
                    t = time.monotonic() - start
                    if text.startswith("oled"):
                        stats["oled_lines"] += 1
                        print(f"[{t:8.1f}s] {text}", flush=True)
                        if first_oled is None:
                            first_oled = text
                        last_oled = text
                        for tok in text.split():
                            if tok.startswith("err="):
                                try:
                                    cur = int(tok[4:])
                                except ValueError:
                                    cur = None
                                if cur is not None:
                                    if last_err_count is None:
                                        last_err_count = cur
                                    elif cur != last_err_count:
                                        print(
                                            f"# !! oledErrorCount changed {last_err_count} -> {cur}",
                                            flush=True,
                                        )
                                        last_err_count = cur
                    elif text.startswith("mem"):
                        stats["mem_lines"] += 1
                        vals = {}
                        for tok in text.split():
                            for key in ("h=", "lg=", "mn="):
                                if tok.startswith(key):
                                    try:
                                        vals[key[:-1]] = int(tok[len(key):])
                                    except ValueError:
                                        pass
                        if len(vals) == 3:
                            heap["free"].append(vals["h"])
                            heap["min"].append(vals["mn"])
                            heap["largest"].append(vals["lg"])
                        if stats["mem_lines"] <= 2 or stats["mem_lines"] % 10 == 0:
                            print(f"[{t:8.1f}s] {text}", flush=True)
                elif ptype == TYPE_PING:
                    stats["rx_ping"] += 1
                    tx_packet(ser, stats, TYPE_PONG, 0, payload)  # 保持心跳
                elif ptype == TYPE_PONG:
                    stats["rx_pong"] += 1
                elif ptype == TYPE_HELLO:
                    stats["hello_ok"] = True
                elif ptype == TYPE_ACK:
                    pass  # 忽略
                else:
                    pass  # 帧/其他消息：跳过
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    print("# ---- summary ----", flush=True)
    print(f"# duration_s={time.monotonic() - start:.1f}", flush=True)
    print(f"# stats={stats}", flush=True)
    if first_oled:
        print(f"# first_oled={first_oled}", flush=True)
    if last_oled:
        print(f"# last_oled={last_oled}", flush=True)
    if last_err_count is not None:
        print(f"# final_oledErrorCount={last_err_count}", flush=True)
    if heap["free"]:
        h0, h1 = heap["free"][0], heap["free"][-1]
        hmin = min(heap["free"])
        hmax = max(heap["free"])
        mn_final = heap["min"][-1] if heap["min"] else 0
        delta = h1 - h0
        print(f"# heap free: first={h0} last={h1} delta={delta:+d} "
              f"min={hmin} max={hmax}", flush=True)
        print(f"# heap minWatermark_final={mn_final}", flush=True)
        # 泄漏判定：末尾比开头低 5% 以上视为可疑漂移（ESP32 堆常态小幅波动）。
        leak_suspect = h0 > 0 and delta < -(h0 // 20)
        if leak_suspect:
            print("# !! heap free trend: possible leak/fragmentation", flush=True)
    else:
        leak_suspect = False

    ok = (
        stats["crc_errors"] == 0
        and stats["protocol_errors"] == 0
        and last_oled is not None
        and not leak_suspect
    )
    print(f"# RESULT {'PASS' if ok else 'FAIL'}", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()