#!/bin/bash
# 本地 WS-FLV 直播环境：SRS(RTMP 收流 + HTTP-FLV) + ffmpeg 推流 + WebSocket 转发。
#   tools/ws-flv-live.sh start [--camera] [--file PATH] [--sei]
#     --sei：给每个视频帧插入推流端时间戳 SEI（tools/latency/flv_sei_inject.py），
#            播放端 HUD 显示端到端延迟。NTP 服务器用 NTP_SERVER 覆盖，默认 ntp.aliyun.com。
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
SEI_INJECT="$ROOT/tools/latency/flv_sei_inject.py"
NTP_SERVER="${NTP_SERVER-ntp.aliyun.com}"

pidfile() { echo "$LIVE/$1.pid"; }

# 进程是否真是我们起的：pid 存活且命令行匹配（防 pid 复用后误杀无关进程）
pid_is_ours() {  # name pid
    local args
    args="$(ps -o args= -p "$2" 2>/dev/null)" || return 1
    case "$1" in
        srs)    [[ -n "$SRS_BIN" && "$args" == "$SRS_BIN"* ]] ;;
        ffmpeg) [[ ( "$args" == "$FF "* || "$args" == *"$SEI_INJECT"* ) && "$args" == *"rtmp://127.0.0.1:$RTMP_PORT/live/test"* ]] ;;
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
    local mode=pattern file="" busy=0 p sei=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --camera) mode=camera ;;
            --file) mode=file; file="${2:-}"; shift
                    [ -f "$file" ] || { echo "--file: no such file: $file" >&2; return 1; } ;;
            --sei) sei=1 ;;
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
    # 注：ffmpeg 4.4 的 h264_videotoolbox 会忽略 -bf 0 仍输出 B 帧（ffmpeg.log 里持续出现
    # "Invalid DTS ... replacing by guess"，muxer 已修正，不影响播放）；FFmpeg 6.0 起 -bf 0 生效。
    # 测低延迟时用 6.0：FFMPEG=/Users/dev/Documents/AV/FFmpeg-6.0/install_macos
    # ffargs 只含输入和编码参数，输出（-f flv url）在最后统一加
    local enc=(-c:v h264_videotoolbox -b:v 1M -bf 0 -g 25 -c:a aac)
    local ffargs
    case "$mode" in
        pattern) ffargs=(-re -f lavfi -i testsrc=size=640x360:rate=25 -f lavfi -i sine "${enc[@]}") ;;
        camera)  ffargs=(-f avfoundation -framerate 30 -i "0:0" "${enc[@]}") ;;
        file)
            # 源文件已是 H.264/AAC 时直接 copy：不重编码，推出去的就是原始码流
            local vc ac cv=(-c:v h264_videotoolbox -b:v 1M -bf 0 -g 25) ca=(-c:a aac)
            vc="$("$FFMPEG/bin/ffprobe" -v error -select_streams v:0 -show_entries stream=codec_name -of csv=p=0 "$file" 2>/dev/null)"
            ac="$("$FFMPEG/bin/ffprobe" -v error -select_streams a:0 -show_entries stream=codec_name -of csv=p=0 "$file" 2>/dev/null)"
            [ "$vc" = h264 ] && cv=(-c:v copy)
            [ "$ac" = aac ] && ca=(-c:a copy)
            echo "file source: video=${vc:-none} (${cv[*]}), audio=${ac:-none} (${ca[*]})"
            ffargs=(-stream_loop -1 -re -i "$file" "${cv[@]}" "${ca[@]}") ;;
    esac
    if [ $sei -eq 1 ]; then
        # 编码输出 FLV 到管道 -> 插 SEI -> ffmpeg -c copy 推 RTMP；stop 时杀 python，它会带走两个 ffmpeg
        echo "SEI timestamp injection: on (NTP: ${NTP_SERVER:-off})"
        # 回放文件时按 pts 记时间戳（--cts），见 flv_sei_inject.py 说明
        local cts=(); [ "$mode" = file ] && cts=(--cts)
        python3 "$SEI_INJECT" --ffmpeg "$FF" --ntp "$NTP_SERVER" ${cts[@]+"${cts[@]}"} --out "$url" -- "${ffargs[@]}" >"$LIVE/ffmpeg.log" 2>&1 &
    else
        "$FF" -nostdin -loglevel warning "${ffargs[@]}" -f flv "$url" >"$LIVE/ffmpeg.log" 2>&1 &
    fi
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
    *) echo "usage: $0 start [--camera] [--file PATH] [--sei] | stop | status" >&2; exit 2 ;;
esac
