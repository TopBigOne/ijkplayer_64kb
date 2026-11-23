#!/usr/bin/env bash
# 本地 WebSocket-FLV 推流环境（局域网版）：Mac 推流，同一 Wi-Fi 下的手机拉流。
#
# 链路：ffmpeg --RTMP--> SRS(1935) --HTTP-FLV(8080)--> relay --ws(18780)/wss(18781)--> 手机
# 推流的实际实现在仓库根目录 tools/ws-flv-live.sh，本脚本只补充局域网地址、连通性检查和手机起播。
#
# 用法：
#   tools/ws-live.sh start [--camera | --file PATH] [--sei]
#                                                     启动推流（默认测试图+正弦音）；
#                                                     --sei 插入推流端时间戳，HUD 显示端到端延迟
#   tools/ws-live.sh stop | status                    停止 / 查看状态
#   tools/ws-live.sh url                              打印手机可用的拉流地址
#   tools/ws-live.sh check                            在 Mac 上用局域网 IP 做 ws/wss 握手检查
#   tools/ws-live.sh play [ws|wss]                    通过 adb 让示例 App 播放（多设备时设 ANDROID_SERIAL）
#
# 环境变量：
#   WS_LIVE_IP   手动指定 Mac 的局域网 IP（默认取默认路由网卡的 IPv4）
#   SRS_HOME     SRS 源码目录（默认 /Users/dev/Documents/AV/srs/trunk，见根目录脚本）
#   FFMPEG       ffmpeg 安装前缀（默认 /usr/local/ffmpeg/4.4）
#   NTP_SERVER   --sei 时推流端校时用的 NTP 服务器（默认 ntp.aliyun.com，要和 App 设置里一致；设为空表示不校时）
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
LIVE_SH="$REPO/tools/ws-flv-live.sh"
WS_PORT=18780
WSS_PORT=18781
STREAM_PATH=/live/test.flv
PKG=tv.danmaku.ijk.media.example
ACTIVITY=$PKG/.activities.VideoActivity

[ -f "$LIVE_SH" ] || { echo "找不到推流脚本: $LIVE_SH" >&2; exit 1; }

lan_ip() {
    if [ -n "${WS_LIVE_IP:-}" ]; then echo "$WS_LIVE_IP"; return 0; fi
    local ifc ip
    ifc="$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')"
    [ -n "$ifc" ] && ip="$(ipconfig getifaddr "$ifc" 2>/dev/null)"
    [ -z "${ip:-}" ] && ip="$(ipconfig getifaddr en0 2>/dev/null)"
    [ -n "${ip:-}" ] || { echo "取不到局域网 IP，请设置 WS_LIVE_IP" >&2; return 1; }
    echo "$ip"
}

url_of() {  # ws|wss
    local ip; ip="$(lan_ip)" || return 1
    case "$1" in
        ws)  echo "ws://$ip:$WS_PORT$STREAM_PATH" ;;
        wss) echo "wss://$ip:$WSS_PORT$STREAM_PATH" ;;
        *) echo "未知协议: $1（ws 或 wss）" >&2; return 1 ;;
    esac
}

cmd_url() {
    local ws wss; ws="$(url_of ws)" || return 1; wss="$(url_of wss)"
    cat <<MSG
手机与 Mac 连同一 Wi-Fi 后，在示例 App 里输入以下地址：
  WS  : $ws
  WSS : $wss   （自签名证书，ijkws 默认不校验，可直接播）
MSG
}

handshake() {  # scheme host port -> 打印 HTTP 状态码
    # 101 之后服务器开始推数据，curl 会一直读，用 -m 超时退出；-w 仍会输出状态码
    curl -sk -m 2 -o /dev/null -w '%{http_code}' \
        -H "Connection: Upgrade" -H "Upgrade: websocket" \
        -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==" \
        "$1://$2:$3$STREAM_PATH" 2>/dev/null
}

cmd_check() {
    local ip code fail=0; ip="$(lan_ip)" || return 1
    code="$(handshake http "$ip" $WS_PORT)"
    echo "ws  $ip:$WS_PORT  握手: ${code:-无响应}"; [ "$code" = 101 ] || fail=1
    code="$(handshake https "$ip" $WSS_PORT)"
    echo "wss $ip:$WSS_PORT 握手: ${code:-无响应}"; [ "$code" = 101 ] || fail=1
    if [ $fail -ne 0 ]; then
        echo "检查失败：确认已 start；502 一般是推流（ffmpeg/SRS）没起来；无响应检查端口占用和 macOS 防火墙" >&2
        return 1
    fi
    echo "OK：局域网地址可用。手机仍连不上时，检查是否同一网段、路由器是否开了 AP/客户端隔离"
}

cmd_play() {
    local scheme="${1:-ws}" url
    url="$(url_of "$scheme")" || return 1
    command -v adb >/dev/null || { echo "没有 adb；请在示例 App 里手动输入: $url" >&2; return 1; }
    if [ "$(adb get-state 2>/dev/null)" != device ]; then
        echo "adb 没有可用设备（多设备请设 ANDROID_SERIAL）；请在示例 App 里手动输入: $url" >&2
        return 1
    fi
    adb shell am force-stop "$PKG"
    adb shell am start -n "$ACTIVITY" --es videoPath "$url" >/dev/null || return 1
    echo "已在手机上播放: $url"
    echo "看日志: adb logcat -v time | grep -E \"ijkws|FFP_MSG|IjkVideoView\""
}

case "${1:-}" in
    start)
        shift
        # 已在运行时先停掉再起，方便直接切换推流源（测试图 / 摄像头 / 文件）
        if "$LIVE_SH" status >/dev/null 2>&1; then
            echo "推流环境已在运行，先停止再按新参数启动"
            "$LIVE_SH" stop
        fi
        "$LIVE_SH" start "$@" || exit 1
        echo
        cmd_url
        ;;
    stop)   "$LIVE_SH" stop ;;
    status) "$LIVE_SH" status ;;
    url)    cmd_url ;;
    check)  cmd_check ;;
    play)   shift; cmd_play "$@" ;;
    *)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac
