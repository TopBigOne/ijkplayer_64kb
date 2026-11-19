#!/bin/bash
# 本地 WS-FLV 直播环境：SRS(RTMP 收流 + HTTP-FLV) + ffmpeg 推流 + WebSocket 转发。
#   tools/ws-flv-live.sh start [--camera] [--file PATH]
#   tools/ws-flv-live.sh stop
#   tools/ws-flv-live.sh status
# SRS 目录可用 SRS_HOME 覆盖。日志和 pid 文件在 tests/ijkws/build/live/。
set -uo pipefail

. "$(cd "$(dirname "$0")" && pwd)/../tests/ijkws/common.sh"

SRS_HOME="${SRS_HOME:-/Users/dev/Documents/AV/srs/trunk}"
SRS_BIN="${SRS_BIN:-$SRS_HOME/cmake/cmake-build-debug/srs}"
SRS_CONF="${SRS_CONF:-conf/http.flv.live.conf}"
FF="$FFMPEG/bin/ffmpeg"
LIVE="$BUILD/live"
RTMP_PORT=1935
HTTP_PORT=8080
WS_PORT=18780
WSS_PORT=18781
NAMES=(srs ffmpeg relay)

pidfile() { echo "$LIVE/$1.pid"; }

# 进程是否真是我们起的：pid 存活且命令行匹配（防 pid 复用后误杀无关进程）
pid_is_ours() {  # name pid
    local args
    args="$(ps -o args= -p "$2" 2>/dev/null)" || return 1
    case "$1" in
        srs)    [[ -n "$SRS_BIN" && "$args" == "$SRS_BIN"* ]] ;;
        ffmpeg) [[ "$args" == "$FF "* && "$args" == *"rtmp://127.0.0.1:$RTMP_PORT/live/test"* ]] ;;
        relay)  [[ "$args" == *"$HERE/ws_test_server.py"* ]] ;;
    esac
}

is_running() {
    local f pid; f="$(pidfile "$1")"
    [ -f "$f" ] || return 1
    pid="$(cat "$f")"
    if pid_is_ours "$1" "$pid"; then return 0; fi
    echo "note: stale pid file for $1 (pid $pid is not ours), removing" >&2
    rm -f "$f"
    return 1
}

port_listening() { lsof -nP -iTCP:"$1" -sTCP:LISTEN >/dev/null 2>&1; }

cmd_status() {
    local n running=0
    for n in "${NAMES[@]}"; do
        if is_running "$n"; then
            echo "$n: running (pid $(cat "$(pidfile "$n")"))"
            running=$((running + 1))
        else
            echo "$n: not running"
        fi
    done
    return $((running == 0))
}

cmd_stop() {
    local n pid i
    for n in relay ffmpeg srs; do
        local f; f="$(pidfile "$n")"
        [ -f "$f" ] || continue
        pid="$(cat "$f")"
        if ! pid_is_ours "$n" "$pid"; then
            echo "note: stale pid file for $n (pid $pid is not ours), removed without killing" >&2
        elif kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            for i in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
            kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null
            echo "stopped $n (pid $pid)"
        fi
        rm -f "$f"
    done
    rm -f "$LIVE/relay.ready"
}

wait_port() {  # port seconds
    local i
    for i in $(seq 1 $(($2 * 10))); do
        port_listening "$1" && return 0
        sleep 0.1
    done
    return 1
}

cmd_start() {
    local mode=pattern file="" busy=0 p
    while [ $# -gt 0 ]; do
        case "$1" in
            --camera) mode=camera ;;
            --file) mode=file; file="${2:-}"; shift
                    [ -f "$file" ] || { echo "--file: no such file: $file" >&2; return 1; } ;;
            *) echo "unknown option: $1" >&2; return 1 ;;
        esac
        shift
    done

    [ -x "$SRS_BIN" ] || { echo "SRS binary not found: $SRS_BIN (set SRS_HOME)" >&2; return 1; }
    [ -x "$FF" ] || { echo "ffmpeg not found: $FF" >&2; return 1; }

    mkdir -p "$LIVE"
    for n in "${NAMES[@]}"; do
        if is_running "$n"; then echo "already running: $n (run stop first)" >&2; return 1; fi
    done
    for p in $RTMP_PORT $HTTP_PORT $WS_PORT $WSS_PORT; do
        if port_listening "$p"; then
            echo "port $p already in use:" >&2
            lsof -nP -iTCP:"$p" -sTCP:LISTEN >&2
            busy=1
        fi
    done
    [ $busy -eq 0 ] || return 1
    ensure_server_cert || { echo "cert generation failed" >&2; return 1; }

    # 1. SRS
    (cd "$SRS_HOME" && exec "$SRS_BIN" -c "$SRS_CONF") >"$LIVE/srs.log" 2>&1 &
    echo $! >"$(pidfile srs)"
    if ! wait_port $RTMP_PORT 10 || ! wait_port $HTTP_PORT 10; then
        echo "SRS did not listen on $RTMP_PORT/$HTTP_PORT within 10s; see $LIVE/srs.log" >&2
        cmd_stop
        return 1
    fi

    # 2. ffmpeg 推流
    local url="rtmp://127.0.0.1:$RTMP_PORT/live/test"
    local enc=(-c:v h264_videotoolbox -b:v 1M -g 50 -c:a aac -f flv "$url")
    local ffargs
    case "$mode" in
        pattern) ffargs=(-re -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine "${enc[@]}") ;;
        camera)  ffargs=(-f avfoundation -framerate 30 -i "0:0" "${enc[@]}") ;;
        file)    ffargs=(-stream_loop -1 -re -i "$file" "${enc[@]}") ;;
    esac
    "$FF" -nostdin -loglevel warning "${ffargs[@]}" >"$LIVE/ffmpeg.log" 2>&1 &
    echo $! >"$(pidfile ffmpeg)"

    # 3. WebSocket 转发
    rm -f "$LIVE/relay.ready"
    python3 "$HERE/ws_test_server.py" --relay-http-base "http://127.0.0.1:$HTTP_PORT" \
        --relay-port $WS_PORT --relay-tls-port $WSS_PORT \
        --cert "$BUILD/server.crt" --key "$BUILD/server.key" \
        --log "$LIVE/relay.jsonl" --ready-file "$LIVE/relay.ready" >"$LIVE/relay.log" 2>&1 &
    echo $! >"$(pidfile relay)"
    if ! wait_port $WS_PORT 10 || ! wait_port $WSS_PORT 10; then
        echo "relay did not start; see $LIVE/relay.log" >&2
        cmd_stop
        return 1
    fi
    sleep 1
    if ! is_running ffmpeg; then
        echo "ffmpeg exited early; see $LIVE/ffmpeg.log" >&2
        tail -5 "$LIVE/ffmpeg.log" >&2
        cmd_stop
        return 1
    fi

    local ip; ip="$(ipconfig getifaddr en0 2>/dev/null || true)"; ip="${ip:-<LAN-IP>}"
    cat <<MSG
Live environment is up (source: $mode). Logs/pids: $LIVE
  HTTP-FLV : http://127.0.0.1:$HTTP_PORT/live/test.flv
  WS       : ws://$ip:$WS_PORT/live/test.flv
  WSS      : wss://$ip:$WSS_PORT/live/test.flv
  Mac play : $HERE/build/ijkws_cat ws://127.0.0.1:$WS_PORT/live/test.flv | $FFMPEG/bin/ffplay -
             (build the tool first: $HERE/build.sh cat)
  Android USB: adb reverse tcp:$WS_PORT tcp:$WS_PORT  then  ws://127.0.0.1:$WS_PORT/live/test.flv
  Stop     : $ROOT/tools/ws-flv-live.sh stop
MSG
}

case "${1:-}" in
    start)  shift; cmd_start "$@" ;;
    stop)   cmd_stop ;;
    status) cmd_status ;;
    *) echo "usage: $0 start [--camera] [--file PATH] | stop | status" >&2; exit 2 ;;
esac
