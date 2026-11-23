#!/usr/bin/env python3
"""
在链路中间测延迟：读 FLV 流，取出推流端时间戳 SEI，打印「收到时刻 - SEI 时刻」。
用来分段定位延迟花在哪一跳（推流 -> SRS -> relay -> 手机）。

    flv_latency_probe.py http://127.0.0.1:8080/live/test.flv          # SRS 输出
    ijkws_cat ws://127.0.0.1:18780/live/test.flv | flv_latency_probe.py -   # ws relay 输出

和推流端用同一台 NTP 服务器校时（--ntp，默认 ntp.aliyun.com）。在推流的同一台机器上测时，
两边用的是同一个本机时钟，校不校时结果都一样。
"""

import argparse
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flv_sei_inject import Clock, UUID, read_exact  # noqa: E402


def unescape(b):
    out = bytearray()
    zeros = 0
    for x in b:
        if zeros >= 2 and x == 3:
            zeros = 0
            continue
        zeros = zeros + 1 if x == 0 else 0
        out.append(x)
    return bytes(out)


def find_sei_ms(nalus, ls, hevc):
    pos = 0
    while pos + ls <= len(nalus):
        n = int.from_bytes(nalus[pos:pos + ls], "big")
        pos += ls
        nal = nalus[pos:pos + n]
        pos += n
        if not nal:
            continue
        t = (nal[0] >> 1) & 0x3F if hevc else nal[0] & 0x1F
        if (hevc and t in (39, 40)) or (not hevc and t == 6):
            rbsp = unescape(nal[2 if hevc else 1:])
            i = rbsp.find(UUID)
            if i >= 0 and i + 24 <= len(rbsp):
                return int.from_bytes(rbsp[i + 16:i + 24], "big", signed=True)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="http(s) FLV 地址，或 - 表示 stdin")
    ap.add_argument("--ntp", default="ntp.aliyun.com")
    ap.add_argument("--interval", type=float, default=1.0, help="汇总打印间隔（秒）")
    ap.add_argument("--duration", type=float, default=0, help="测多少秒后退出，0 表示一直测")
    args = ap.parse_args()

    clock = Clock(args.ntp)
    f = sys.stdin.buffer if args.src == "-" else urllib.request.urlopen(args.src)
    head = read_exact(f, 13)
    if head is None or head[:3] != b"FLV":
        sys.exit("not FLV")

    ls, samples, last_print = 4, [], time.time()
    deadline = time.time() + args.duration if args.duration > 0 else None
    while deadline is None or time.time() < deadline:
        th = read_exact(f, 11)
        if th is None:
            break
        size = int.from_bytes(th[1:4], "big")
        body = read_exact(f, size + 4)
        if body is None:
            break
        if th[0] & 0x1F != 9 or size < 5:
            continue
        codec, ptype = body[0] & 0x0F, body[1]
        if codec not in (7, 12):
            continue
        hevc = codec == 12
        if ptype == 0:
            cfg = body[5:size]
            if not hevc and len(cfg) >= 5:
                ls = (cfg[4] & 3) + 1
            elif hevc and len(cfg) >= 22:
                ls = (cfg[21] & 3) + 1
            continue
        ms = find_sei_ms(body[5:size], ls, hevc)
        if ms is not None:
            samples.append(clock.now_ms() - ms)
        now = time.time()
        if now - last_print >= args.interval and samples:
            print(f"latency ms: min {min(samples)}  avg {sum(samples) // len(samples)}  max {max(samples)}  ({len(samples)} frames)",
                  flush=True)
            samples, last_print = [], now


if __name__ == "__main__":
    main()
