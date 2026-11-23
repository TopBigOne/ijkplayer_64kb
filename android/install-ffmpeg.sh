#! /usr/bin/env bash
#
# 准备 Android 编译 ijkplayer 所需的三方静态库，产物统一放到
# FFToolChain/build/product/android/universal/（ijkplayer-cmake 通过 third-libs 软链接读取）。
#
# 用法：
#   ./install-ffmpeg.sh              下载 soundtouch/yuv/openssl 预编译包，再源码编译 FFmpeg 7（ijkffmpeg7）
#   （FFmpeg 7 没有可用的上游预编译包，原 --prebuilt 选项已移除）
#
# 为什么 FFmpeg 必须走源码：
#   FFToolChain 子模块是自有 fork（ijk-custom 分支），改了 FFmpeg 裁剪配置
#   （configs/ijk-ffmpeg-config/module-lite-ff7.sh，例如启用 rtmps、mp4 muxer）；
#   上游 Releases 也没有 ijkplayer 可用的 FFmpeg 7 预编译包。
#
# 源码编译的前置条件：
#   - ANDROID_NDK_HOME（或 ANDROID_NDK_ROOT / ANDROID_NDK）指向 NDK 目录
#   - brew install nasm pkg-config
#   - 能访问 GitHub（首次 init 会 clone https://github.com/FFmpeg/FFmpeg.git）
#
# 详细说明见 doc/ffmpeg4_to_7_upgrade.md。

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
    echo "FFmpeg 7（ijkffmpeg7）没有上游预编译包，请去掉 --prebuilt 从源码编译"
    exit 1
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
# 只需要其中的 openssl；FFmpeg 4.0（ijkffmpeg）本身已不再使用，删掉避免混淆
rm -rf "$THIS_DIR/../FFToolChain/build/product/android/universal/ijkffmpeg"

# 2. 拉取官方 FFmpeg 源码并打 ijk 补丁（已存在时复用）
echo "==> [2/3] 准备 FFmpeg 7 源码（官方 n7.1.3 + ijk 补丁）：$ARCHS"
"$TOOLCHAIN" init -p android -l ijkffmpeg7 -a "$ARCHS"

# 3. 交叉编译。用 rebuild（clean + build），保证 module-lite-ff7.sh 的改动一定重新 configure
echo "==> [3/3] 源码编译 ijkffmpeg7：$ARCHS"
"$TOOLCHAIN" compile -p android -l ijkffmpeg7 -a "$ARCHS" -c rebuild

echo
echo "完成。产物：FFToolChain/build/product/android/universal/ijkffmpeg7/ijkffmpeg7-{arm64,armv7a}/"
echo "校验 rtmps：grep CONFIG_RTMPS_PROTOCOL $THIS_DIR/../FFToolChain/build/product/android/universal/ijkffmpeg7/ijkffmpeg7-arm64/include/libffmpeg/config_components.h"
