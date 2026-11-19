#!/usr/bin/env python3
"""ijkws 主机测试用的最小 WebSocket 服务器（只用标准库）。

按 path 选择行为，详见 BEHAVIORS / handle_conn。每个连接的请求行、握手 Header、
收到的 close 码和连接结束时间以 JSONL 形式写入 --log。
"""
import argparse
import asyncio
import base64
import hashlib
import json
import os
import ssl
import struct
import time
from urllib.parse import urlsplit

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_CONT, OP_TEXT, OP_BIN, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA

FLV_MSG = 4096
BIG_PREFIX = 1024 * 1024
FLOOD_TOTAL = 20 * 1024 * 1024
FLOOD_MSG = 64 * 1024
DROP_BYTES = 64 * 1024

args = None
flv_data = b""
log_fp = None
conn_seq = 0


def log_event(ev):
    ev["t"] = time.time()
    log_fp.write(json.dumps(ev, ensure_ascii=False) + "\n")
    log_fp.flush()


def big_prefix():
    # 与 tests/ijkws/test_stream.c 保持一致：byte[i] = i % 251
    return bytes(i % 251 for i in range(251)) * (BIG_PREFIX // 251) + bytes(i % 251 for i in range(BIG_PREFIX % 251))


def frame(opcode, payload, fin=True):
    b0 = (0x80 if fin else 0) | opcode
    n = len(payload)
    if n < 126:
        hdr = struct.pack("!BB", b0, n)
    elif n < 65536:
        hdr = struct.pack("!BBH", b0, 126, n)
    else:
        hdr = struct.pack("!BBQ", b0, 127, n)
    return hdr + payload


class Conn:
    def __init__(self, cid, reader, writer, path):
        self.cid = cid
        self.reader = reader
        self.writer = writer
        self.path = path
        self.close_sent = False
        self.peer_closed = asyncio.Event()
        self.gone = False

    async def send(self, data):
        if self.gone:
            raise ConnectionError("gone")
        self.writer.write(data)
        await self.writer.drain()

    async def send_close(self, code=1000, reason=b""):
        if self.close_sent:
            return
        self.close_sent = True
        await self.send(frame(OP_CLOSE, struct.pack("!H", code) + reason))

    async def read_loop(self):
        """解析客户端（带掩码的）帧：回 pong，记录 close 码。"""
        try:
            while True:
                h = await self.reader.readexactly(2)
                opcode = h[0] & 0x0F
                masked = h[1] & 0x80
                n = h[1] & 0x7F
                if n == 126:
                    n = struct.unpack("!H", await self.reader.readexactly(2))[0]
                elif n == 127:
                    n = struct.unpack("!Q", await self.reader.readexactly(8))[0]
                mask = await self.reader.readexactly(4) if masked else b"\0\0\0\0"
                data = bytearray(await self.reader.readexactly(n))
                for i in range(n):
                    data[i] ^= mask[i & 3]
                if opcode == OP_PING:
                    await self.send(frame(OP_PONG, bytes(data)))
                elif opcode == OP_CLOSE:
                    code = struct.unpack("!H", data[:2])[0] if n >= 2 else None
                    log_event({"event": "close_received", "conn": self.cid,
                               "code": code, "reason": bytes(data[2:]).decode("utf-8", "replace")})
                    self.peer_closed.set()
                    if self.path == "/close-silent":
                        continue  # 不回复，也不断开
                    try:
                        await self.send_close(code if code else 1000)
                    except Exception:
                        pass
                    return
        except Exception:
            return
        finally:
            self.peer_closed.set()


async def push_messages(conn, data, msg_size, opcode=OP_BIN, ping_every=0, fragments=1):
    count = 0
    for off in range(0, len(data), msg_size):
        msg = data[off:off + msg_size]
        if fragments > 1 and len(msg) >= fragments:
            step = (len(msg) + fragments - 1) // fragments
            parts = [msg[i:i + step] for i in range(0, len(msg), step)]
            for i, p in enumerate(parts):
                op = opcode if i == 0 else OP_CONT
                await conn.send(frame(op, p, fin=(i == len(parts) - 1)))
        else:
            await conn.send(frame(opcode, msg))
        count += 1
        if ping_every and count % ping_every == 0:
            await conn.send(frame(OP_PING, b"ijkws"))


async def finish_normal(conn):
    await conn.send_close(1000)
    try:
        await asyncio.wait_for(conn.peer_closed.wait(), 5)
    except asyncio.TimeoutError:
        pass


async def behave(conn):
    p = conn.path
    if p == "/silent":
        await conn.peer_closed.wait()
    elif p == "/chunked":
        await push_messages(conn, flv_data, 7)
        await finish_normal(conn)
    elif p == "/fragmented":
        await push_messages(conn, flv_data, FLV_MSG, fragments=3)
        await finish_normal(conn)
    elif p == "/text":
        await push_messages(conn, flv_data, FLV_MSG, opcode=OP_TEXT)
        await finish_normal(conn)
    elif p == "/ping":
        await push_messages(conn, flv_data, FLV_MSG, ping_every=3)
        await finish_normal(conn)
    elif p == "/big":
        await conn.send(frame(OP_BIN, big_prefix()))
        await push_messages(conn, flv_data, FLV_MSG)
        await finish_normal(conn)
    elif p == "/flood":
        chunk = bytes(i & 0xFF for i in range(FLOOD_MSG))
        for _ in range(FLOOD_TOTAL // FLOOD_MSG):
            await conn.send(frame(OP_BIN, chunk))
        await finish_normal(conn)
    elif p == "/drop":
        await push_messages(conn, flv_data[:DROP_BYTES], FLV_MSG)
        # 直接关 TCP，不发 close 帧
    elif p == "/close-silent":
        await push_messages(conn, flv_data, FLV_MSG)
        # 收到客户端 close 后不回复也不断开：一直挂着直到客户端断开
        while not conn.reader.at_eof():
            await asyncio.sleep(0.05)
    else:  # /flv 及其他未列出的 path
        await push_messages(conn, flv_data, FLV_MSG)
        await finish_normal(conn)


async def handle_conn(reader, writer):
    global conn_seq
    conn_seq += 1
    cid = conn_seq
    try:
        raw = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
    except Exception:
        writer.close()
        return
    lines = raw.decode("latin-1").split("\r\n")
    request_line = lines[0]
    headers = []
    for ln in lines[1:]:
        if not ln:
            continue
        k, _, v = ln.partition(":")
        headers.append([k.strip(), v.strip()])
    hmap = {k.lower(): v for k, v in headers}
    parts = request_line.split(" ")
    target = parts[1] if len(parts) > 1 else "/"
    u = urlsplit(target)
    path, query = u.path, u.query
    sockname = writer.get_extra_info("sockname")
    log_event({"event": "connect", "conn": cid, "request_line": request_line,
               "path": path, "query": query, "headers": headers,
               "tls": writer.get_extra_info("sslcontext") is not None,
               "local_port": sockname[1] if sockname else None})

    try:
        if path.startswith("/status/"):
            code = int(path[len("/status/"):])
            writer.write(("HTTP/1.1 %d Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" % code).encode())
            await writer.drain()
            return

        key = hmap.get("sec-websocket-key", "")
        accept = base64.b64encode(hashlib.sha1(key.encode() + WS_GUID).digest()).decode()
        resp = ("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Accept: %s\r\n" % accept)
        proto = hmap.get("sec-websocket-protocol")
        if proto:
            resp += "Sec-WebSocket-Protocol: %s\r\n" % proto.split(",")[0].strip()
        resp += "\r\n"
        writer.write(resp.encode())
        await writer.drain()

        conn = Conn(cid, reader, writer, path)
        rtask = asyncio.ensure_future(conn.read_loop())
        try:
            await behave(conn)
        except Exception:
            pass
        rtask.cancel()
    except Exception:
        pass
    finally:
        try:
            writer.close()
        except Exception:
            pass
        log_event({"event": "end", "conn": cid})


async def main():
    servers = [await asyncio.start_server(handle_conn, ["127.0.0.1", "::1"], args.port)]
    if args.tls_port:
        sctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        sctx.load_cert_chain(args.cert, args.key)
        servers.append(await asyncio.start_server(handle_conn, ["127.0.0.1", "::1"], args.tls_port, ssl=sctx))
    if args.ready_file:
        with open(args.ready_file, "w") as f:
            f.write(str(os.getpid()))
    await asyncio.gather(*(s.serve_forever() for s in servers))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--tls-port", type=int)
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--flv", required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--ready-file")
    args = ap.parse_args()
    with open(args.flv, "rb") as f:
        flv_data = f.read()
    log_fp = open(args.log, "a")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
