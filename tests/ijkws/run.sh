#!/bin/bash
# ijkws 主机测试入口：tests/ijkws/run.sh [test_xxx ...]
# 不带参数时编译并运行全部 test_*.c；带参数时只运行指定的测试程序。
set -uo pipefail

. "$(dirname "$0")/common.sh"
WS_PORT=18765
WSS_PORT=18766

ensure_lws || exit 1

if [ ! -f "$BUILD/test.flv" ]; then
    "$FFMPEG/bin/ffmpeg" -nostdin -loglevel error -y \
        -f lavfi -i testsrc=size=320x240:rate=25 -f lavfi -i sine -t 5 \
        -c:v flv -c:a aac -f flv "$BUILD/test.flv" || exit 1
fi

# 自签名证书（CN/SAN = localhost；另一张只含 IP:::1，无 SNI 时出示）和一张无关的 CA
ensure_server_cert || exit 1
ensure_server_ip_cert || exit 1
ensure_other_ca || exit 1

if [ $# -gt 0 ]; then
    TESTS=("$@")
else
    TESTS=()
    for f in "$HERE"/test_*.c; do TESTS+=("$(basename "$f" .c)"); done
fi

PASS=0
FAIL=0
for t in "${TESTS[@]}"; do
    # 测试源码里形如 " * IJKWS_TEST_CFLAGS: -DFOO=1" 的一行给出额外编译参数（作用于整个测试程序，含 ijkws.c）
    extra=$(sed -n 's/^ \* IJKWS_TEST_CFLAGS: //p' "$HERE/$t.c")
    # shellcheck disable=SC2086
    if ! build_c "$t" $extra; then
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
    --ip-cert "$BUILD/server-ip.crt" --ip-key "$BUILD/server-ip.key" \
    --flv "$BUILD/test.flv" --log "$LOG" --ready-file "$READY" 2>"$BUILD/server.stderr" &
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
