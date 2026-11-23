#! /usr/bin/env bash
#
# 准备 Android 编译 ijkplayer 所需的三方静态库，产物统一放到
# FFToolChain/build/product/android/universal/（ijkplayer-cmake 通过 third-libs 软链接读取）。
#
# 用法：
#   ./install-ffmpeg.sh              默认：先下载预编译包，再用源码编译的 ijkffmpeg 覆盖 arm64/armv7a
#   ./install-ffmpeg.sh --prebuilt   全部下载上游预编译包（快，但 ijkffmpeg 不含我们的定制，如 rtmps）
#
# 为什么 ijkffmpeg 默认走源码：
#   FFToolChain 子模块是自有 fork（ijk-custom 分支），改了 FFmpeg 裁剪配置
#   （configs/ijk-ffmpeg-config/module-lite.sh，例如启用 rtmps）。
#   `main.sh install` 下载的是 debugly 上游 Releases 里的预编译包，不包含这些改动。
#
# 源码编译的前置条件：
#   - ANDROID_NDK_HOME（或 ANDROID_NDK_ROOT / ANDROID_NDK）指向 NDK 目录
#   - brew install nasm pkg-config
#   - 能访问 GitHub（首次 init 会 clone https://github.com/bilibili/FFmpeg.git）
#
# 详细说明见 doc/android_build.md「从源码编译 ijkffmpeg」。

set -e

THIS_DIR=$(cd "$(dirname "$0")"; pwd)
TOOLCHAIN="$THIS_DIR/../FFToolChain/main.sh"
# 与 ijkplayer-cmake / ijkplayer-example 的 abiFilters 保持一致
ARCHS="armv7a arm64"

usage() {
    sed -n '6,8p' "$0" | sed 's/^# \{0,1\}//'
}

PREBUILT=0
for arg in "$@"; do
    case "$arg" in
        --prebuilt) PREBUILT=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $arg"; usage; exit 1 ;;
    esac
done

if [[ ! -x "$TOOLCHAIN" ]]; then
    echo "找不到 $TOOLCHAIN，先执行：git submodule update --init --recursive"
    exit 1
fi

if [[ $PREBUILT -eq 1 ]]; then
    echo "==> [--prebuilt] 下载上游预编译包：soundtouch yuv ijkffmpeg（含依赖 openssl）"
    echo "    注意：ijkffmpeg 不含自有 fork 的定制（如 rtmps）"
    "$TOOLCHAIN" install -l 'soundtouch yuv ijkffmpeg' -p android
    exit 0
fi

# ---- 源码编译前检查，缺什么直接报错，不自动安装 ----
missing=()
for bin in nasm pkg-config; do
    command -v "$bin" >/dev/null 2>&1 || missing+=("$bin")
done
if [[ ${#missing[@]} -gt 0 ]]; then
    echo "缺少编译工具：${missing[*]}，请先执行：brew install ${missing[*]}"
    exit 1
fi
if [[ -z "$ANDROID_NDK_HOME$ANDROID_NDK_ROOT$ANDROID_NDK" ]]; then
    echo "未设置 ANDROID_NDK_HOME（或 ANDROID_NDK_ROOT / ANDROID_NDK），请指向 NDK 目录"
    exit 1
fi

# 1. 先下载上游预编译包：soundtouch、yuv 直接使用；openssl 是 ijkffmpeg 的依赖
#    （pkg-config 自动发现），但上游 Releases 没有单独的 Android openssl 包，只打包在
#    ijkffmpeg 的 zip 里，所以这里照常装 ijkffmpeg，随后第 3 步用源码编译结果覆盖 arm64/armv7a。
#    zip 会缓存在 FFToolChain/build/pre/，之后不再重复下载。
echo "==> [1/3] 下载预编译包：soundtouch yuv ijkffmpeg（取其中的 openssl）"
"$TOOLCHAIN" install -l 'soundtouch yuv ijkffmpeg' -p android

# 2. 拉取 bilibili FFmpeg 源码（已存在时复用）
echo "==> [2/3] 准备 ijkffmpeg 源码：$ARCHS"
"$TOOLCHAIN" init -p android -l ijkffmpeg -a "$ARCHS"

# 3. 交叉编译。用 rebuild（clean + build），保证 module-lite.sh 的改动一定重新 configure
echo "==> [3/3] 源码编译 ijkffmpeg：$ARCHS"
"$TOOLCHAIN" compile -p android -l ijkffmpeg -a "$ARCHS" -c rebuild

echo
echo "完成。产物：FFToolChain/build/product/android/universal/ijkffmpeg/ijkffmpeg-{arm64,armv7a}/"
echo "校验 rtmps：grep CONFIG_RTMPS_PROTOCOL $THIS_DIR/../FFToolChain/build/product/android/universal/ijkffmpeg/ijkffmpeg-arm64/config.h"
