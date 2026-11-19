#!/bin/bash
# ijkws 主机测试入口：tests/ijkws/run.sh [test_xxx ...]
# 不带参数时编译并运行全部 test_*.c；带参数时只运行指定的测试程序。
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$HERE/build"
FFMPEG="${FFMPEG:-/usr/local/ffmpeg/4.4}"
OPENSSL="${OPENSSL:-/opt/homebrew/opt/openssl@3}"
LWS="$BUILD/lws"
SRC="$ROOT/ijkmedia/ijkplayer/ijkavformat"
WS_PORT=18765
WSS_PORT=18766

mkdir -p "$BUILD"

if [ ! -f "$LWS/lib/libwebsockets.a" ]; then
    "$ROOT/tools/build-libwebsockets-host.sh" || exit 1
fi

# 1. 测试用 FLV
if [ ! -f "$BUILD/test.flv" ]; then
    "$FFMPEG/bin/ffmpeg" -nostdin -loglevel error -y \
        -f lavfi -i testsrc=size=320x240:rate=25 -f lavfi -i sine -t 5 \
        -c:v flv -c:a aac -f flv "$BUILD/test.flv" || exit 1
fi

# 2. 自签名证书（CN/SAN = localhost）和一张无关的 CA
if [ ! -f "$BUILD/server.crt" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$BUILD/server.key" -out "$BUILD/server.crt" \
        -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1 || exit 1
fi
if [ ! -f "$BUILD/other-ca.crt" ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$BUILD/other-ca.key" -out "$BUILD/other-ca.crt" \
        -subj "/CN=ijkws-other-ca" >/dev/null 2>&1 || exit 1
fi

# 4. 编译测试程序
CFLAGS=(-std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -g -O0 -Wall -Wextra -Wno-unused-parameter
        -I"$SRC" -I"$HERE" -I"$LWS/include" -I"$FFMPEG/include" -I"$OPENSSL/include")
LIBS=("$LWS/lib/libwebsockets.a" -L"$FFMPEG/lib" -lavformat -lavutil
      "$OPENSSL/lib/libssl.a" "$OPENSSL/lib/libcrypto.a" -lpthread)
IJKWS_SRCS=("$SRC/ijkws.c" "$SRC/ijkws_buf.c" "$HERE/resolve_host.c")

if [ $# -gt 0 ]; then
    TESTS=("$@")
else
    TESTS=()
    for f in "$HERE"/test_*.c; do TESTS+=("$(basename "$f" .c)"); done
fi

PASS=0
FAIL=0
for t in "${TESTS[@]}"; do
    if ! clang "${CFLAGS[@]}" -o "$BUILD/$t" "$HERE/$t.c" "${IJKWS_SRCS[@]}" "${LIBS[@]}"; then
        echo "[BUILD FAIL] $t"
        FAIL=$((FAIL + 1))
    fi
done
[ $FAIL -gt 0 ] && { echo "PASS $PASS / FAIL $FAIL"; exit 1; }

# 3. 后台启动服务器
LOG="$BUILD/server.log"
READY="$BUILD/server.ready"
: > "$LOG"
rm -f "$READY"
python3 "$HERE/ws_test_server.py" --port $WS_PORT --tls-port $WSS_PORT \
    --cert "$BUILD/server.crt" --key "$BUILD/server.key" \
    --flv "$BUILD/test.flv" --log "$LOG" --ready-file "$READY" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null' EXIT
for _ in $(seq 1 50); do
    [ -f "$READY" ] && break
    if ! kill -0 $SERVER_PID 2>/dev/null; then echo "server failed to start"; exit 1; fi
    sleep 0.1
done
[ -f "$READY" ] || { echo "server not ready"; exit 1; }

# 5. 逐个运行（每个测试程序内部打印自己的 PASS/FAIL 计数行）
export IJKWS_TEST_BUILD="$BUILD"
export IJKWS_TEST_LOG="$LOG"
for t in "${TESTS[@]}"; do
    out=$(cd "$BUILD" && "./$t" 2>&1)
    rc=$?
    echo "$out" | grep -v '^#counts '
    p=$(echo "$out" | sed -n 's/^#counts pass=\([0-9]*\) fail=\([0-9]*\)$/\1/p')
    f=$(echo "$out" | sed -n 's/^#counts pass=\([0-9]*\) fail=\([0-9]*\)$/\2/p')
    if [ -z "$p" ]; then
        echo "[CRASH] $t (exit $rc)"
        FAIL=$((FAIL + 1))
        continue
    fi
    PASS=$((PASS + p))
    FAIL=$((FAIL + f))
done

# 6. 汇总
echo "PASS $PASS / FAIL $FAIL"
[ $FAIL -eq 0 ]
