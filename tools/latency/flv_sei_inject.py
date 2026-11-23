#!/usr/bin/env python3
"""
给 FLV 直播流的每个视频帧插入「推流端时间戳」SEI，用于测量端到端延迟。

数据流：
    ffmpeg(采集+编码, 输出 FLV 到管道) ──▶ 本脚本(插 SEI) ──▶ ffmpeg(-c copy 推 RTMP)

SEI 格式（播放端解析见 ijkmedia/ijkplayer/ff_e2e_latency.h）：
    H.264 NAL type 6 / H.265 NAL type 39，payloadType 5 (user_data_unregistered)
    payload = UUID "AIJK_E2E_LATENCY"(16 字节) + int64 大端 Unix 毫秒（已做 NTP 校正）

时间戳在「编码器输出这一帧、进入推流」时打，所以测出来的延迟不含采集和编码耗时，
要看完整的 glass-to-glass 延迟，用拍照法（见 doc/live-latency.md）。

--cts：回放文件（-re）时用。-re 按 dts 发帧，有 B 帧时 P 帧会比它的显示时刻早 cts 发出，
       时间戳记为「发出时刻 + cts」，相当于这一帧如果是实时拍摄的拍摄时刻；不加的话 P 帧会多算 cts。

用法：
    flv_sei_inject.py --ffmpeg /usr/local/ffmpeg/4.4/bin/ffmpeg --out rtmp://127.0.0.1/live/test \
        [--ntp ntp.aliyun.com] -- <编码端 ffmpeg 的输入和编码参数，不含输出>
    flv_sei_inject.py --filter [--ntp ...] < in.flv > out.flv      # 只做过滤，便于调试
"""

import argparse
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

UUID = b"AIJK_E2E_LATENCY"
assert len(UUID) == 16

NTP_EPOCH_OFFSET = 2208988800


def log(msg):
    print(f"[sei_inject] {msg}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------- NTP

def sntp_query(server, timeout=2.0):
    """返回 (offset_ms, rtt_ms)，offset = NTP 时间 - 本机时间"""
    pkt = bytearray(48)
    pkt[0] = 0x1B
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        t1 = time.time()
        sec = int(t1) + NTP_EPOCH_OFFSET
        frac = int((t1 % 1) * (1 << 32))
        struct.pack_into("!II", pkt, 40, sec, frac)
        s.sendto(pkt, (server, 123))
        data, _ = s.recvfrom(48)
        t4 = time.time()
    if len(data) < 48 or (data[0] & 7) != 4 or data[1] == 0:
        raise ValueError("bad NTP response")

    def ts(off):
        sec, frac = struct.unpack_from("!II", data, off)
        return sec - NTP_EPOCH_OFFSET + frac / (1 << 32)

    t2, t3 = ts(32), ts(40)
    offset = ((t2 - t1) + (t3 - t4)) / 2
    rtt = (t4 - t1) - (t3 - t2)
    return offset * 1000.0, rtt * 1000.0


class Clock:
    """NTP 校正后的毫秒时钟；每 60 秒重新校时一次，取 3 次里 RTT 最小的结果。

    以单调时钟为基准（NTP 时间 = monotonic + mono_offset），系统时钟被 macOS 校时服务
    突然拨动（实测出现过一次跳 1.8 秒）时不受影响。
    """

    def __init__(self, server):
        self.server = server
        self.offset_ms = 0.0
        self.mono_offset_ms = time.time() * 1000 - time.monotonic() * 1000
        if server:
            self._sync()
            t = threading.Thread(target=self._loop, daemon=True)
            t.start()

    def _sync(self):
        best = None
        for _ in range(3):
            try:
                wall_minus_mono = time.time() * 1000 - time.monotonic() * 1000
                r = sntp_query(self.server)
                if best is None or r[1] < best[1]:
                    best = (r[0], r[1], wall_minus_mono)
            except Exception as e:  # noqa: BLE001
                log(f"NTP {self.server} failed: {e}")
        if best:
            self.offset_ms = best[0]
            self.mono_offset_ms = best[2] + best[0]
            log(f"NTP {self.server}: offset {best[0]:+.1f} ms, rtt {best[1]:.1f} ms")

    def _loop(self):
        while True:
            time.sleep(60)
            self._sync()

    def now_ms(self):
        return int(round(time.monotonic() * 1000 + self.mono_offset_ms))


# ---------------------------------------------------------------- SEI

def escape_rbsp(rbsp):
    """加防竞争字节：00 00 0x(x<=3) -> 00 00 03 0x"""
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def build_sei_nal(ms, hevc):
    payload = UUID + struct.pack(">q", ms)
    rbsp = bytes([5, len(payload)]) + payload + b"\x80"
    header = b"\x4e\x01" if hevc else b"\x06"  # HEVC prefix SEI (39<<1, tid=1) / H.264 SEI
    return header + escape_rbsp(rbsp)


# ---------------------------------------------------------------- FLV

def read_exact(f, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = f.read(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


class Injector:
    def __init__(self, clock, add_cts=False):
        self.clock = clock
        self.add_cts = add_cts
        self.length_size = 4
        self.frames = 0

    def process_video(self, data):
        """data 为 FLV video tag 的 body，返回插好 SEI 的新 body"""
        if len(data) < 5:
            return data
        codec = data[0] & 0x0F
        if codec not in (7, 12):  # 7 = AVC, 12 = HEVC（国内通行的扩展 ID）
            return data
        hevc = codec == 12
        pkt_type = data[1]
        if pkt_type == 0:  # sequence header：取 NAL 长度字节数
            cfg = data[5:]
            if not hevc and len(cfg) >= 5:
                self.length_size = (cfg[4] & 3) + 1
            elif hevc and len(cfg) >= 22:
                self.length_size = (cfg[21] & 3) + 1
            return data
        if pkt_type != 1:
            return data

        nalus = data[5:]
        ls = self.length_size
        ms = self.clock.now_ms()
        if self.add_cts:
            cts = int.from_bytes(data[2:5], "big")
            if cts & 0x800000:
                cts -= 1 << 24
            ms += cts
        sei = build_sei_nal(ms, hevc)
        sei = len(sei).to_bytes(ls, "big") + sei

        # 第一个 NAL 是 AUD 时插在 AUD 后面（AUD 必须在最前）
        insert_at = 0
        if len(nalus) > ls:
            n0 = int.from_bytes(nalus[:ls], "big")
            t0 = (nalus[ls] >> 1) & 0x3F if hevc else nalus[ls] & 0x1F
            if (hevc and t0 == 35) or (not hevc and t0 == 9):
                insert_at = ls + n0
        self.frames += 1
        if self.frames == 1:
            log(f"injecting {'HEVC' if hevc else 'H.264'} SEI, NAL length size {ls}")
        return data[:5] + nalus[:insert_at] + sei + nalus[insert_at:]

    def run(self, fin, fout):
        header = read_exact(fin, 9 + 4)
        if header is None or header[:3] != b"FLV":
            raise ValueError("input is not FLV")
        fout.write(header)
        fout.flush()
        while True:
            th = read_exact(fin, 11)
            if th is None:
                return
            tag_type = th[0] & 0x1F
            size = int.from_bytes(th[1:4], "big")
            body = read_exact(fin, size + 4)
            if body is None:
                return
            data = body[:size]
            if tag_type == 9:
                data = self.process_video(data)
            out_header = bytes([th[0]]) + len(data).to_bytes(3, "big") + th[4:]
            fout.write(out_header + data + (11 + len(data)).to_bytes(4, "big"))
            fout.flush()


# ---------------------------------------------------------------- main

def main():
    argv = sys.argv[1:]
    enc_args = []
    if "--" in argv:
        i = argv.index("--")
        argv, enc_args = argv[:i], argv[i + 1:]

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--out", help="推流地址，如 rtmp://127.0.0.1:1935/live/test")
    ap.add_argument("--ntp", default="ntp.aliyun.com", help="NTP 服务器，空字符串表示不校时")
    ap.add_argument("--filter", action="store_true", help="只做 stdin -> stdout 过滤")
    ap.add_argument("--cts", action="store_true", help="时间戳加上 composition time（回放文件时用）")
    args = ap.parse_args(argv)

    clock = Clock(args.ntp)
    injector = Injector(clock, args.cts)

    if args.filter:
        injector.run(sys.stdin.buffer, sys.stdout.buffer)
        return 0

    if not args.out or not enc_args:
        ap.error("需要 --out 和 -- 后面的编码参数")

    enc_cmd = [args.ffmpeg, "-nostdin", "-loglevel", "warning", *enc_args,
               "-flush_packets", "1", "-f", "flv", "pipe:1"]
    push_cmd = [args.ffmpeg, "-nostdin", "-loglevel", "warning",
                "-fflags", "nobuffer", "-analyzeduration", "1000000", "-f", "flv", "-i", "pipe:0",
                "-c", "copy", "-flush_packets", "1", "-f", "flv", args.out]
    log("encoder: " + " ".join(enc_cmd))
    log("pusher : " + " ".join(push_cmd))

    enc = subprocess.Popen(enc_cmd, stdout=subprocess.PIPE, bufsize=0)
    push = subprocess.Popen(push_cmd, stdin=subprocess.PIPE, bufsize=0)

    def stop(*_):
        for p in (enc, push):
            if p.poll() is None:
                p.terminate()
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    try:
        injector.run(enc.stdout, push.stdin)
    except (BrokenPipeError, ValueError) as e:
        log(f"stopped: {e}")
    finally:
        stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
