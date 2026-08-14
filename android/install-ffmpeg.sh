#! /usr/bin/env bash
#
# ../FFToolChain/main.sh install -l 'soundtouch yuv ijkffmpeg' -p android

# ./FFToolChain/main.sh — FFToolChain 子模块的总入口脚本，是这个下载/编译工具链的调度器

# install — 子命令，对应 main.sh 里的三大动作之一（init 拉源码、compile 编译源码、install 下载预编译产物）。走这个分支时会跳转到 FFToolChain/do-install/main.sh 执行

# -l 'soundtouch yuv ijkffmpeg' — 指定要安装的库列表（空格分隔，所以要加引号），依次是：

# soundtouch — 变速不变调库
# yuv — libyuv，图像格式转换/缩放库
# ijkffmpeg — bilibili fork 的定制版 FFmpeg（目前锁定 4.0，见 FFToolChain/configs/libs/ijkffmpeg.sh）
# 每个名字对应 FFToolChain/configs/libs/<名字>.sh 一个配置文件，脚本会依次 source 它们，读出下载 tag、URL 规则等信息

# -p android — 指定平台为 android，决定了从各库配置文件里取哪个 PRE_COMPILE_TAG_ANDROID 变量，以及产物解压到哪个目录（FFToolChain/build/product/android/）

# 执行时实际发生的事（FFToolChain/do-install/main.sh → install_libs()）：


# for lib in soundtouch yuv ijkffmpeg:
#     source configs/libs/${lib}.sh          # 读取该库的版本 tag、URL 规则
#     解析出 LIB_NAME / VER（从 PRE_COMPILE_TAG_ANDROID 里拆出来）
#     拼出下载 URL:
#         https://github.com/debugly/MRFFToolChainBuildShell/releases/download/<TAG>/<LIB_NAME>-android-universal-<VER>.zip
#     curl 下载 zip → 解压到 FFToolChain/build/product/android/universal/
# 最后再执行一次 correct-pc.sh 修正 .pc 文件（pkg-config 用的元信息文件）里的路径前缀，让后续 cmake/gradle 编译时能正确找到这些库的头文件和 .a。

# 一句话总结：这条命令 = 用工具链批量下载 soundtouch、libyuv、ijkffmpeg(FFmpeg 4.0) 三个库的 Android 四 ABI 预编译静态库，解压放到指定目录，供后面 build-aar.sh 里 CMake 编译时链接使用，全程不涉及本地编译。

../FFToolChain/main.sh install -l 'soundtouch yuv ijkffmpeg' -p android
