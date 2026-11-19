# 共享的主机构建配置（被 run.sh / build.sh / tools/ws-flv-live.sh source）。
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$HERE/build"
FFMPEG="${FFMPEG:-/usr/local/ffmpeg/4.4}"
OPENSSL="${OPENSSL:-/opt/homebrew/opt/openssl@3}"
LWS="$BUILD/lws"
SRC="$ROOT/ijkmedia/ijkplayer/ijkavformat"

CFLAGS=(-std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -g -O0 -Wall -Wextra -Wno-unused-parameter
        -I"$SRC" -I"$HERE" -I"$LWS/include" -I"$FFMPEG/include" -I"$OPENSSL/include")
LIBS=("$LWS/lib/libwebsockets.a" -L"$FFMPEG/lib" -lavformat -lavutil
      "$OPENSSL/lib/libssl.a" "$OPENSSL/lib/libcrypto.a" -lpthread)
IJKWS_SRCS=("$SRC/ijkws.c" "$SRC/ijkws_buf.c" "$HERE/resolve_host.c")

ensure_lws() {
    mkdir -p "$BUILD"
    if [ ! -f "$LWS/lib/libwebsockets.a" ]; then
        "$ROOT/tools/build-libwebsockets-host.sh" || return 1
    fi
}

ensure_server_cert() {
    mkdir -p "$BUILD"
    if [ ! -f "$BUILD/server.crt" ]; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
            -keyout "$BUILD/server.key" -out "$BUILD/server.crt" \
            -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1 || return 1
    fi
}

ensure_other_ca() {
    if [ ! -f "$BUILD/other-ca.crt" ]; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
            -keyout "$BUILD/other-ca.key" -out "$BUILD/other-ca.crt" \
            -subj "/CN=ijkws-other-ca" >/dev/null 2>&1 || return 1
    fi
}

# build_c <name> [extra clang args]：编译 $HERE/<name>.c 到 $BUILD/<name>
build_c() {
    local name="$1"; shift
    clang "${CFLAGS[@]}" "$@" -o "$BUILD/$name" "$HERE/$name.c" "${IJKWS_SRCS[@]}" "${LIBS[@]}"
}
