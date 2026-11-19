#!/bin/bash
# WS-FLV 稳定性测试驱动（基于 tests/ijkws/build/ijkws_cat，可指向任意 ws/wss 地址）。
#   tools/ws-flv-soak.sh soak   [-o key=value]... <url> <duration_s>
#   tools/ws-flv-soak.sh switch [-o key=value]... <url> <count> <interval_s>
# soak  ：挂流 duration_s 秒，每 10s 采样 ijkws_cat 的 RSS 和累计字节，写 CSV；
#         ijkws_cat 提前退出，或 RSS 首尾增长 > 10MB 判 FAIL。
# switch：反复 起进程 -> 收到>=1 字节(最多等 2s) -> SIGINT -> 间隔 interval_s，统计失败数；
#         若 $RELAY_LOG（默认本地 live 环境的 relay.jsonl）存在，对比本次新增的
#         relay_connect / relay_close 数量；并检查本脚本起的 ijkws_cat 进程均已退出且无残留 TCP 连接；
#         仅当主机是回环地址时，再检查该端口上 ESTABLISHED 连接数回到 0。
# 选项原样传给 ijkws_cat，例如 -o ws_headers='Authorization: Bearer xxx'、-o ws_tls_verify=0。
# 输出：tests/ijkws/build/soak/<时间戳>/ 下的 CSV / 日志。退出码 0=PASS，1=FAIL，2=用法错误。
set -uo pipefail

. "$(cd "$(dirname "$0")" && pwd)/../tests/ijkws/common.sh"
CAT="$BUILD/ijkws_cat"
RELAY_LOG="${RELAY_LOG:-$BUILD/live/relay.jsonl}"
RSS_LIMIT_KB=$((10 * 1024))
SAMPLE_S=10

usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

[ $# -ge 1 ] || usage
MODE="$1"; shift
OPTS=()
while [ "${1:-}" = "-o" ]; do
    [ $# -ge 2 ] || usage
    OPTS+=(-o "$2"); shift 2
done
URL="${1:-}"; [ -n "$URL" ] || usage
case "$URL" in ws://*|wss://*) ;; *) echo "url must be ws:// or wss://" >&2; exit 2 ;; esac

if [ ! -x "$CAT" ]; then
    echo "building ijkws_cat ..." >&2
    "$HERE/build.sh" cat >&2 || { echo "build failed" >&2; exit 1; }
fi

OUT="$BUILD/soak/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
CAT_PID=""
cleanup() { [ -n "$CAT_PID" ] && kill -0 "$CAT_PID" 2>/dev/null && kill -INT "$CAT_PID" 2>/dev/null; return 0; }
trap 'cleanup; exit 130' INT TERM

url_port() {
    local hp="${URL#*://}"; hp="${hp%%/*}"; hp="${hp##*@}"
    if [[ "$hp" == *:* && "${hp##*:}" =~ ^[0-9]+$ ]]; then echo "${hp##*:}"
    elif [[ "$URL" == wss://* ]]; then echo 443; else echo 80; fi
}

last_total() {  # 从 -v 日志取最新累计字节
    grep -o 'total=[0-9]*' "$1" | tail -1 | cut -d= -f2
}

# ---------------------------------------------------------------- soak
cmd_soak() {
    local dur="${2:-}"
    [[ "$dur" =~ ^[0-9]+$ ]] && [ "$dur" -gt 0 ] || usage
    local csv="$OUT/soak.csv" errlog="$OUT/cat.stderr"
    echo "elapsed_s,rss_kb,total_bytes" >"$csv"
    local t0=$SECONDS early=0 rss total
    "$CAT" -v "${OPTS[@]+"${OPTS[@]}"}" "$URL" >/dev/null 2>"$errlog" &
    CAT_PID=$!
    echo "soak: $URL for ${dur}s, pid $CAT_PID, output $OUT"
    sleep 2
    while :; do
        if ! kill -0 "$CAT_PID" 2>/dev/null; then early=1; break; fi
        rss="$(ps -o rss= -p "$CAT_PID" 2>/dev/null | tr -d ' ')"
        total="$(last_total "$errlog")"
        [ -n "$rss" ] && echo "$((SECONDS - t0)),$rss,${total:-0}" >>"$csv"
        [ $((SECONDS - t0)) -ge "$dur" ] && break
        sleep $SAMPLE_S
    done
    local code=0
    if [ $early -eq 1 ]; then
        wait "$CAT_PID"; code=$?
    else
        kill -INT "$CAT_PID" 2>/dev/null
        wait "$CAT_PID"; code=$?
    fi
    CAT_PID=""
    local elapsed=$((SECONDS - t0))
    total="$(last_total "$errlog")"; total="${total:-0}"
    local first last max n
    n=$(($(wc -l <"$csv") - 1))
    first="$(sed -n 2p "$csv" | cut -d, -f2)"
    last="$(tail -1 "$csv" | cut -d, -f2)"
    max="$(tail -n +2 "$csv" | cut -d, -f2 | sort -n | tail -1)"
    echo "---- soak result ----"
    echo "duration      : ${elapsed}s (requested ${dur}s), samples: $n"
    echo "total bytes   : $total"
    echo "avg rate      : $(awk -v b="$total" -v s="$elapsed" 'BEGIN{printf "%.1f KB/s", (s>0? b/1024/s : 0)}')"
    echo "RSS start/end/max: ${first:-?} / ${last:-?} / ${max:-?} KB"
    echo "exit code     : $code"
    echo "csv           : $csv"
    local fail=0
    if [ $early -eq 1 ]; then echo "FAIL: ijkws_cat exited early"; tail -3 "$errlog" | sed 's/^/  stderr: /'; fail=1; fi
    [ "$code" -eq 0 ] || { echo "FAIL: exit code $code"; fail=1; }
    if [ "$n" -lt 2 ]; then echo "FAIL: fewer than 2 RSS samples"; fail=1
    elif [ $((last - first)) -gt $RSS_LIMIT_KB ]; then
        echo "FAIL: RSS grew $((last - first)) KB (> $RSS_LIMIT_KB KB)"; fail=1
    fi
    [ "$total" -gt 0 ] || { echo "FAIL: no bytes received"; fail=1; }
    [ $fail -eq 0 ] && echo "PASS" || echo "RESULT: FAIL"
    return $fail
}

# ---------------------------------------------------------------- switch
estab_count() { lsof -nP -iTCP:"$1" -sTCP:ESTABLISHED 2>/dev/null | tail -n +2 | wc -l | tr -d ' '; }
url_host() { local hp="${URL#*://}"; hp="${hp%%/*}"; hp="${hp##*@}"; hp="${hp#[}"; hp="${hp%%]*}"; echo "${hp%%:*}"; }
ALL_PIDS=()

cmd_switch() {
    local count="${2:-}" interval="${3:-}"
    [[ "$count" =~ ^[0-9]+$ ]] && [ "$count" -gt 0 ] && [[ "$interval" =~ ^[0-9.]+$ ]] || usage
    local port; port="$(url_port)"
    local relay_base=0 have_relay=0
    if [ -f "$RELAY_LOG" ]; then have_relay=1; relay_base=$(wc -l <"$RELAY_LOG" | tr -d ' '); fi
    local i fail=0 f data pid w code sz t0=$SECONDS
    data="$OUT/switch.data"
    echo "switch: $URL x$count, interval ${interval}s, output $OUT"
    for i in $(seq 1 "$count"); do
        "$CAT" "${OPTS[@]+"${OPTS[@]}"}" "$URL" >"$data" 2>"$OUT/cat.$i.stderr" &
        pid=$!; CAT_PID=$pid; ALL_PIDS+=($pid)
        sz=0
        for w in $(seq 1 40); do   # 最多等 2s
            sz=$(stat -f %z "$data" 2>/dev/null || echo 0)
            [ "$sz" -gt 0 ] && break
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.05
        done
        f=""
        if ! kill -0 "$pid" 2>/dev/null; then f="exited before SIGINT"; fi
        [ "$sz" -gt 0 ] || f="${f:+$f; }no bytes within 2s"
        kill -INT "$pid" 2>/dev/null
        # 最多等 3s 退出，否则强杀并计失败
        for w in $(seq 1 60); do kill -0 "$pid" 2>/dev/null || break; sleep 0.05; done
        if kill -0 "$pid" 2>/dev/null; then kill -9 "$pid" 2>/dev/null; f="${f:+$f; }did not exit within 3s of SIGINT"; fi
        wait "$pid"; code=$?
        CAT_PID=""
        [ "$code" -eq 0 ] || f="${f:+$f; }exit code $code"
        if [ -n "$f" ]; then fail=$((fail + 1)); echo "  #$i FAIL: $f"; else rm -f "$OUT/cat.$i.stderr"; fi
        [ "$i" -lt "$count" ] && sleep "$interval"
    done
    rm -f "$data"
    local elapsed=$((SECONDS - t0))
    sleep 1   # 给对端处理 close
    echo "---- switch result ----"
    echo "iterations    : $count in ${elapsed}s, failures: $fail"
    local bad=0
    if [ $have_relay -eq 1 ]; then
        local conn cls
        conn=$(tail -n +$((relay_base + 1)) "$RELAY_LOG" | grep -c '"relay_connect"')
        cls=$(tail -n +$((relay_base + 1)) "$RELAY_LOG" | grep -c '"relay_close"')
        echo "relay events  : connect=$conn close=$cls ($RELAY_LOG, new lines only)"
        [ "$conn" -eq "$cls" ] || { echo "FAIL: relay connects != closes"; bad=1; }
    else
        echo "relay events  : skipped (no $RELAY_LOG; set RELAY_LOG to enable)"
    fi
    local alive=0 p socks=0
    for p in ${ALL_PIDS[@]+"${ALL_PIDS[@]}"}; do
        if kill -0 "$p" 2>/dev/null; then
            alive=$((alive + 1))
            socks=$((socks + $(lsof -nP -a -p "$p" -iTCP 2>/dev/null | tail -n +2 | wc -l)))
        fi
    done
    echo "own ijkws_cat still alive = $alive (open TCP sockets: $socks)"
    [ "$alive" -eq 0 ] || { echo "FAIL: ijkws_cat processes left running"; bad=1; }
    case "$(url_host)" in
        127.0.0.1|localhost|::1)
            local est; est=$(estab_count "$port")
            echo "ESTABLISHED on loopback :$port = $est"
            [ "$est" -eq 0 ] || { echo "FAIL: leftover ESTABLISHED connections"; bad=1; } ;;
        *) echo "server-port ESTABLISHED check skipped (non-loopback host)" ;;
    esac
    [ "$fail" -eq 0 ] || bad=1
    [ $bad -eq 0 ] && echo "PASS" || echo "RESULT: FAIL"
    return $bad
}

case "$MODE" in
    soak)   cmd_soak "$@" ;;
    switch) cmd_switch "$@" ;;
    *) usage ;;
esac
