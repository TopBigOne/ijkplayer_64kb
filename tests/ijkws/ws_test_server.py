#!/usr/bin/env python3
"""ijkws 主机测试用的最小 WebSocket 服务器（只用标准库）。

按 path 选择行为，详见 behave / handle_conn。每个连接的请求行、握手 Header、
收到的 pong / close 帧（含 close 码、是否带掩码）和连接结束时间以 JSONL 形式写入 --log。
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
sni_names = {}  # id(SSLObject) -> 客户端发来的 SNI


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
        """解析客户端（带掩码的）帧：回 pong；记录收到的 pong、close 码以及帧是否带掩码。"""
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
                elif opcode == OP_PONG:
                    log_event({"event": "pong_received", "conn": self.cid,
                               "masked": bool(masked), "len": n})
                elif opcode == OP_CLOSE:
                    code = struct.unpack("!H", data[:2])[0] if n >= 2 else None
                    # RFC 6455 §5.3：客户端发出的帧必须带掩码
                    log_event({"event": "close_received", "conn": self.cid,
                               "code": code, "masked": bool(masked),
                               "reason": bytes(data[2:]).decode("utf-8", "replace")})
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
               "sni": sni_names.pop(id(writer.get_extra_info("ssl_object")), None),
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


# ---------------------------------------------------------------- relay 模式
# 把 --relay-http-base 上的 HTTP 直播流（如 SRS 的 HTTP-FLV）转成 WebSocket 二进制帧。
RELAY_FRAME = 64 * 1024

async def relay_open_upstream(path_q):
    """GET 上游；返回 (status, reader, writer, chunked)。"""
    u = urlsplit(args.relay_http_base)
    host = u.hostname
    port = u.port or 80
    r, w = await asyncio.wait_for(asyncio.open_connection(host, port), 5)
    try:
        w.write(("GET %s HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\nConnection: close\r\n\r\n"
                 % (path_q, u.netloc)).encode())
        await w.drain()
        raw = await asyncio.wait_for(r.readuntil(b"\r\n\r\n"), args.relay_header_timeout)
        lines = raw.decode("latin-1").split("\r\n")
        status = int(lines[0].split(" ")[1])
        hdrs = {}
        for ln in lines[1:]:
            k, _, v = ln.partition(":")
            if k:
                hdrs[k.strip().lower()] = v.strip()
        chunked = "chunked" in hdrs.get("transfer-encoding", "").lower()
    except BaseException:
        w.close()
        raise

    return status, r, w, chunked

async def relay_body(reader, chunked):
    """异步生成器：产出上游 body 数据块（≤ RELAY_FRAME）。"""
    if chunked:
        while True:
            line = await reader.readline()
            if not line:
                return
            size = int(line.split(b";")[0].strip() or b"0", 16)
            if size == 0:
                return
            left = size
            while left > 0:
                d = await reader.read(min(left, RELAY_FRAME))
                if not d:
                    return
                left -= len(d)
                yield d
            await reader.readexactly(2)  # chunk 后的 CRLF
    else:
        while True:
            d = await reader.read(RELAY_FRAME)
            if not d:
                return
            yield d

async def handle_relay(reader, writer):
    global conn_seq
    conn_seq += 1
    cid = conn_seq
    up_w = None
    reason = "end"
    try:
        raw = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10)
        lines = raw.decode("latin-1").split("\r\n")
        request_line = lines[0]
        hmap = {}
        for ln in lines[1:]:
            k, _, v = ln.partition(":")
            if k:
                hmap[k.strip().lower()] = v.strip()
        parts = request_line.split(" ")
        target = parts[1] if len(parts) > 1 else "/"
        peer = writer.get_extra_info("peername")
        log_event({"event": "relay_connect", "conn": cid, "target": target,
                   "peer": peer[0] if peer else None})
        try:
            status, ur, up_w, chunked = await relay_open_upstream(target)
        except Exception as e:
            reason = "upstream_error: %r" % (e,)
            writer.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
            await writer.drain()
            return
        if status != 200:
            reason = "upstream_status_%d" % status
            writer.write(("HTTP/1.1 %d Relay\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                          % status).encode())
            await writer.drain()
            return
        key = hmap.get("sec-websocket-key", "")
        accept = base64.b64encode(hashlib.sha1(key.encode() + WS_GUID).digest()).decode()
        writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
        await writer.drain()

        conn = Conn(cid, reader, writer, target)
        rtask = asyncio.ensure_future(conn.read_loop())

        async def pump():
            async for d in relay_body(ur, chunked):
                await conn.send(frame(OP_BIN, d))
            return "upstream_eof"
        ptask = asyncio.ensure_future(pump())
        done, _ = await asyncio.wait({rtask, ptask}, return_when=asyncio.FIRST_COMPLETED)
        if ptask in done:
            try:
                reason = ptask.result()
                await conn.send_close(1000)
            except Exception as e:
                reason = "client_gone"
        else:
            reason = "client_closed"
        ptask.cancel()
        rtask.cancel()
    except Exception as e:
        reason = "error: %r" % (e,)
    finally:
        for w in (up_w, writer):
            try:
                if w:
                    w.close()
            except Exception:
                pass
        log_event({"event": "relay_close", "conn": cid, "reason": reason})

async def handle_relay_conn(reader, writer):
    await handle_relay(reader, writer)

async def main():
    servers = []
    if args.port:
        servers.append(await asyncio.start_server(handle_conn, ["127.0.0.1", "::1"], args.port))
    if args.port and args.tls_port:
        sctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
        sctx.load_cert_chain(args.cert, args.key)
        sctx.sni_callback = lambda sslobj, name, c: sni_names.__setitem__(id(sslobj), name)
        servers.append(await asyncio.start_server(handle_conn, ["127.0.0.1", "::1"], args.tls_port, ssl=sctx))
    if args.relay_http_base:
        servers.append(await asyncio.start_server(handle_relay_conn, "0.0.0.0", args.relay_port))
        if args.cert and args.key:
            rctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
            rctx.load_cert_chain(args.cert, args.key)
            servers.append(await asyncio.start_server(handle_relay_conn, "0.0.0.0", args.relay_tls_port, ssl=rctx))
    if args.ready_file:
        with open(args.ready_file, "w") as f:
            f.write(str(os.getpid()))
    await asyncio.gather(*(s.serve_forever() for s in servers))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, help="测试监听端口（ws）；省略则不开测试监听")
    ap.add_argument("--tls-port", type=int)
    ap.add_argument("--cert")
    ap.add_argument("--key")
    ap.add_argument("--flv")
    ap.add_argument("--log", required=True)
    ap.add_argument("--ready-file")
    ap.add_argument("--relay-http-base", help="开启转发模式：上游 HTTP 直播流的 base URL，如 http://127.0.0.1:8080")
    ap.add_argument("--relay-port", type=int, default=18780)
    ap.add_argument("--relay-tls-port", type=int, default=18781)
    ap.add_argument("--relay-header-timeout", type=float, default=5,
                    help="等上游响应头的秒数（SRS 对未推流的 .flv 会挂起不回头）；超时回 502")
    args = ap.parse_args()
    if args.flv:
        with open(args.flv, "rb") as f:
            flv_data = f.read()
    log_fp = open(args.log, "a")
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
