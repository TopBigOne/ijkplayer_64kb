#!/bin/bash
# 转发上游失败路径不应泄漏 fd：上游只接受不回应 / 回垃圾，各发起 N 次，
# 断言 relay 进程的 fd 数没有随 N 增长。用法：tests/ijkws/check_relay_leak.sh
set -uo pipefail
. "$(dirname "$0")/common.sh"
N=${N:-25}
RP=18790
UP=18791
mkdir -p "$BUILD"
ensure_server_cert || exit 1
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; }
trap cleanup EXIT

# 假上游：/hang 只接受不回应，/garbage 回非 HTTP 内容
python3 - $UP <<'PY' &
import asyncio, sys
async def h(r, w):
    try:
        req = await r.readuntil(b"\r\n\r\n")
        if b"/garbage" in req:
            w.write(b"garbage\r\n\r\n"); await w.drain(); w.close()
        else:
            await asyncio.sleep(60)
    except Exception:
        pass
async def main():
    s = await asyncio.start_server(h, "127.0.0.1", int(sys.argv[1]))
    await s.serve_forever()
asyncio.run(main())
PY
PIDS+=($!)

python3 "$HERE/ws_test_server.py" --relay-http-base "http://127.0.0.1:$UP" --relay-port $RP \
    --relay-tls-port $((RP + 1)) --cert "$BUILD/server.crt" --key "$BUILD/server.key" \
    --relay-header-timeout 0.3 --log "$BUILD/relay_leak.jsonl" --ready-file "$BUILD/relay_leak.ready" \
    2>"$BUILD/relay_leak.stderr" &
RELAY=$!
PIDS+=($RELAY)
for _ in $(seq 1 50); do lsof -nP -iTCP:$RP -sTCP:LISTEN >/dev/null 2>&1 && break; sleep 0.1; done

hit() {  # path
    python3 - $RP "$1" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
s.sendall(("GET %s HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" % sys.argv[2]).encode())
s.settimeout(5); s.recv(100); s.close()
PY
}
fds() { lsof -nP -p $RELAY 2>/dev/null | wc -l | tr -d ' '; }

hit /live/hang; hit /garbage   # 预热
sleep 0.5
before=$(fds)
for _ in $(seq 1 $N); do hit /live/hang; hit /garbage; done
sleep 1
after=$(fds)
echo "relay fds before=$before after=$after (N=$N x2 paths)"
if [ $((after - before)) -lt 5 ]; then echo "PASS no fd leak"; else echo "FAIL fd leak"; exit 1; fi
