#!/usr/bin/env bash
#
# 交叉编译 libwebsockets 为 iOS 静态库 libwebsockets.a（真机 arm64，模拟器 arm64 + x86_64）。
# 链接 FFToolChain 预编译的 OpenSSL 1.1.1w 头文件；最终随 IJKMediaFramework（动态 framework）
# 一起链接，与 FFmpeg 共用同一份 OpenSSL。
#
# 用法：
#   tools/build-libwebsockets-ios.sh
#
# 环境变量（都有默认值）：
#   LWS_SRC   libwebsockets 源码目录（需已切到 v4.3-stable@bb1261254）
#   NINJA     ninja 可执行文件
#
# 产物：FFToolChain/build/product/ios/{universal,universal-simulator}/libwebsockets/{include,lib/libwebsockets.a}

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LWS_SRC="${LWS_SRC:-/Users/dev/Documents/AV/libwebsockets}"
NINJA="${NINJA:-/Users/dev/Documents/Android_Env/sdk/cmake/3.31.6/bin/ninja}"
MIN_IOS=11.0
LWS_COMMIT=bb1261254

PRODUCT="$ROOT/FFToolChain/build/product/ios"
BUILD_ROOT="$ROOT/ios/build-libwebsockets"

head=$(git -C "$LWS_SRC" rev-parse --short=9 HEAD)
if [ "$head" != "$LWS_COMMIT" ]; then
    echo "libwebsockets 源码在 $head，期望 $LWS_COMMIT" >&2
    exit 1
fi

# 名称  sdk  架构
build_one() {
    local name=$1 sdk=$2 archs=$3
    local ssl="$PRODUCT/$name/openssl"
    local out="$PRODUCT/$name/libwebsockets"
    local bld="$BUILD_ROOT/$name"
    local sysroot
    sysroot=$(xcrun --sdk "$sdk" --show-sdk-path)

    echo "===== libwebsockets $name ($sdk: $archs) ====="
    rm -rf "$bld" "$out"
    mkdir -p "$BUILD_ROOT"
    cmake -S "$LWS_SRC" -B "$bld" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="$sysroot" \
        -DCMAKE_OSX_ARCHITECTURES="$archs" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$out" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DLWS_WITH_SSL=ON \
        -DLWS_OPENSSL_INCLUDE_DIRS="$ssl/include" \
        -DLWS_OPENSSL_LIBRARIES="$ssl/lib/libssl.a;$ssl/lib/libcrypto.a" \
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
}

build_one universal           iphoneos        "arm64"
build_one universal-simulator iphonesimulator "arm64;x86_64"
