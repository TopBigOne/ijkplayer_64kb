#!/usr/bin/env bash
#
# 交叉编译 libwebsockets 为 Android 独立动态库 libijkwebsockets.so（文件名与 SONAME 都是它）。
# 不叫 libwebsockets.so：第三方 SDK（P2P / 云存储等）常自带一份 libwebsockets.so，
# 同名会导致 AGP 打包时 duplicate file 失败，或运行时加载到别人的那份。
# OpenSSL 1.1.1w（工程现有预编译静态库）静态链接进 libijkwebsockets.so，
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
# 产物：android/ijkplayer/ijkplayer-cmake/src/main/cpp/third-libs/libwebsockets/libwebsockets-<arch>/{include,lib/libijkwebsockets.so}

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

# lws 的 lib/CMakeLists.txt 把共享库的 OUTPUT_NAME 写死为 websockets，没有可覆盖的缓存变量。
# 不改 lws 源码：通过 CMAKE_PROJECT_libwebsockets_INCLUDE 在 project(libwebsockets) 时注入，
# 用 cmake_language(DEFER) 推迟到顶层目录处理完（websockets_shared 已定义）再改 OUTPUT_NAME。
# 配合 CMAKE_PLATFORM_NO_VERSIONED_SONAME，文件名与 SONAME 都是 libijkwebsockets.so。
mkdir -p "$BUILD_ROOT"
RENAME_CMAKE="$BUILD_ROOT/ijk-lws-output-name.cmake"
cat > "$RENAME_CMAKE" <<'CMAKE'
cmake_language(DEFER CALL set_target_properties websockets_shared PROPERTIES OUTPUT_NAME ijkwebsockets)
CMAKE

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
        -DCMAKE_PROJECT_libwebsockets_INCLUDE="$RENAME_CMAKE" \
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
    # 只保留头文件和 libijkwebsockets.so
    rm -rf "$out/lib/cmake" "$out/lib/pkgconfig" "$out/share" "$out/bin"
    # 记录所用 OpenSSL 版本，ijkws.c 编译期校验与 libijkplayer 的 OpenSSL 一致
    "$ROOT/tools/write-lws-openssl-version.sh" "$ssl/include" "$out/include"
    lib="$out/lib/libijkwebsockets.so"
    "$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-strip" --strip-unneeded "$lib"
    # 确认改名生效：文件存在、SONAME 正确、没有残留的 libwebsockets.so
    soname=$("$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-readelf" -d "$lib" | sed -n 's/.*Library soname: \[\(.*\)\]/\1/p')
    if [ "$soname" != "libijkwebsockets.so" ] || ls "$out"/lib/libwebsockets.so* >/dev/null 2>&1; then
        echo "libijkwebsockets.so 改名失败（SONAME=$soname）" >&2
        exit 1
    fi
    echo "-> $out"
done
