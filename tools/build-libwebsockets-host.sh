#!/bin/bash
# 编译 macOS 主机版 libwebsockets 静态库，仅供 tests/ijkws 主机测试使用。
# 产物：tests/ijkws/build/lws/{include,lib/libwebsockets.a}
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LWS_SRC="${LWS_SRC:-/Users/dev/Documents/AV/libwebsockets}"
NINJA="${NINJA:-/Users/dev/Documents/Android_Env/sdk/cmake/3.31.6/bin/ninja}"
OPENSSL="${OPENSSL:-/opt/homebrew/opt/openssl@3}"
LWS_COMMIT=bb1261254

BUILD_ROOT="$ROOT/tests/ijkws/build"
out="$BUILD_ROOT/lws"
bld="$BUILD_ROOT/lws-build"

head=$(git -C "$LWS_SRC" rev-parse --short=9 HEAD)
if [ "$head" != "$LWS_COMMIT" ]; then
    echo "libwebsockets 源码在 $head，期望 $LWS_COMMIT" >&2
    exit 1
fi

echo "===== libwebsockets host (macOS) ====="
rm -rf "$bld" "$out"
mkdir -p "$BUILD_ROOT"
cmake -S "$LWS_SRC" -B "$bld" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$out" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DLWS_WITH_SSL=ON \
    -DLWS_OPENSSL_INCLUDE_DIRS="$OPENSSL/include" \
    -DLWS_OPENSSL_LIBRARIES="$OPENSSL/lib/libssl.a;$OPENSSL/lib/libcrypto.a" \
    -DLWS_WITHOUT_SERVER=ON \
    -DLWS_WITHOUT_TESTAPPS=ON \
    -DLWS_WITH_MINIMAL_EXAMPLES=OFF \
    -DLWS_WITH_SHARED=OFF \
    -DLWS_WITH_STATIC=ON \
    -DLWS_WITH_HTTP2=OFF \
    -DLWS_WITHOUT_EXTENSIONS=ON \
    -DLWS_WITH_LIBUV=OFF -DLWS_WITH_LIBEV=OFF -DLWS_WITH_LIBEVENT=OFF -DLWS_WITH_GLIB=OFF \
    -DLWS_WITH_SYS_ASYNC_DNS=OFF \
    -DLWS_IPV6=ON \
    -DDISABLE_WERROR=ON \
    -Wno-dev > "$bld.configure.log" 2>&1 || { tail -40 "$bld.configure.log"; exit 1; }
cmake --build "$bld"
cmake --install "$bld" > /dev/null
rm -rf "$out/lib/cmake" "$out/lib/pkgconfig" "$out/share" "$out/bin"
echo "-> $out"
