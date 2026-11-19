#!/usr/bin/env bash
#
# 交叉编译 libwebsockets 为 Android 独立动态库 libwebsockets.so。
# OpenSSL 1.1.1w（工程现有预编译静态库）静态链接进 libwebsockets.so，
# 并用 --exclude-libs,ALL 隐藏其符号，避免与 libijkplayer.so 内的 OpenSSL 互相串用。
#
# 用法：
#   tools/build-libwebsockets-android.sh [arch ...]     默认 arm64 armv7a
#
# 环境变量（都有默认值）：
#   LWS_SRC   libwebsockets 源码目录（需已切到 v4.3-stable@bb1261254）
#   NDK       Android NDK 目录
#   NINJA     ninja 可执行文件
#
# 产物：android/ijkplayer/ijkplayer-cmake/src/main/cpp/third-libs/libwebsockets/libwebsockets-<arch>/{include,lib/libwebsockets.so}

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LWS_SRC="${LWS_SRC:-/Users/dev/Documents/AV/libwebsockets}"
NDK="${NDK:-/Users/dev/Documents/Android_Env/sdk/ndk/27.0.12077973}"
NINJA="${NINJA:-/Users/dev/Documents/Android_Env/sdk/cmake/3.31.6/bin/ninja}"
API=21
LWS_COMMIT=bb1261254

THIRD="$ROOT/android/ijkplayer/ijkplayer-cmake/src/main/cpp/third-libs"
BUILD_ROOT="$ROOT/android/build-libwebsockets"

ARCHS=("$@")
[ ${#ARCHS[@]} -eq 0 ] && ARCHS=(arm64 armv7a)

head=$(git -C "$LWS_SRC" rev-parse --short=9 HEAD)
if [ "$head" != "$LWS_COMMIT" ]; then
    echo "libwebsockets 源码在 $head，期望 $LWS_COMMIT" >&2
    exit 1
fi

for arch in "${ARCHS[@]}"; do
    case "$arch" in
        arm64)  abi=arm64-v8a ;;
        armv7a) abi=armeabi-v7a ;;
        x86)    abi=x86 ;;
        x86_64) abi=x86_64 ;;
        *) echo "未知架构 $arch" >&2; exit 1 ;;
    esac

    ssl="$THIRD/openssl/openssl-$arch"
    out="$THIRD/libwebsockets/libwebsockets-$arch"
    bld="$BUILD_ROOT/$arch"

    echo "===== libwebsockets $arch ($abi) ====="
    rm -rf "$bld" "$out"
    mkdir -p "$BUILD_ROOT"
    cmake -S "$LWS_SRC" -B "$bld" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$out" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_PLATFORM_NO_VERSIONED_SONAME=ON \
        -DCMAKE_SHARED_LINKER_FLAGS="-Wl,--exclude-libs,ALL -Wl,-z,max-page-size=16384" \
        -DLWS_WITH_SSL=ON \
        -DLWS_OPENSSL_INCLUDE_DIRS="$ssl/include" \
        -DLWS_OPENSSL_LIBRARIES="$ssl/lib/libssl.a;$ssl/lib/libcrypto.a" \
        -DLWS_WITHOUT_SERVER=ON \
        -DLWS_WITHOUT_TESTAPPS=ON \
        -DLWS_WITH_MINIMAL_EXAMPLES=OFF \
        -DLWS_WITH_SHARED=ON \
        -DLWS_WITH_STATIC=OFF \
        -DLWS_WITH_HTTP2=OFF \
        -DLWS_WITHOUT_EXTENSIONS=ON \
        -DLWS_WITH_LIBUV=OFF -DLWS_WITH_LIBEV=OFF -DLWS_WITH_LIBEVENT=OFF -DLWS_WITH_GLIB=OFF \
        -DLWS_WITH_SYS_ASYNC_DNS=OFF \
        -DLWS_IPV6=ON \
        -DDISABLE_WERROR=ON \
        -Wno-dev > "$bld.configure.log" 2>&1 || { tail -40 "$bld.configure.log"; exit 1; }
    cmake --build "$bld"
    cmake --install "$bld" > /dev/null
    # 只保留头文件和 libwebsockets.so
    rm -rf "$out/lib/cmake" "$out/lib/pkgconfig" "$out/share" "$out/bin"
    "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-unneeded "$out/lib/libwebsockets.so"
    echo "-> $out"
done
