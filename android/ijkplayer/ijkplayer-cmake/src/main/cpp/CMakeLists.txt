# ============================================================
# ijkplayer Android CMake 构建脚本
# 负责将 ijkmedia 下各模块的 C/C++ 源码编译为共享库 libijkplayer.so，
# 并链接 FFmpeg、OpenSSL、SoundTouch、libyuv 等第三方静态库。
# ============================================================

# 指定 CMake 最低版本要求，确保使用的 CMake 特性可用
cmake_minimum_required(VERSION 3.22)

# 定义项目名称，后续通过 ${CMAKE_PROJECT_NAME} 引用（即 "ijkplayer"）
project(ijkplayer)

# ============================================================
# 编译警告抑制
# ijkplayer/FFmpeg 源码中存在将整数隐式转换为指针（或反向）的写法，
# 在新版 Clang 中会触发 -Wint-conversion 警告并可能导致编译中断，
# 此处统一关闭该警告以保证 C 和 C++ 文件都能正常编译。
# ============================================================
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-int-conversion")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-int-conversion")

# ============================================================
# ABI → 架构目录名映射
# Android NDK 使用 ANDROID_ABI 标识目标 ABI，而预编译的第三方库
# 按自定义的目录名存放（如 armv7a、arm64、x86、x86_64）。
# 此处将两者对应起来，后续通过 ${ARCH} 拼接库路径。
# ============================================================
if (ANDROID_ABI STREQUAL "armeabi-v7a")
    set(ARCH "armv7a")
elseif (ANDROID_ABI STREQUAL "arm64-v8a")
    set(ARCH "arm64")
elseif (ANDROID_ABI STREQUAL "x86")
    set(ARCH "x86")
elseif (ANDROID_ABI STREQUAL "x86_64")
    set(ARCH "x86_64")
else()
    message(WARNING "Unsupported ANDROID_ABI: ${ANDROID_ABI}")
endif()

# ============================================================
# 生成版本头文件 ijkversion.h
# 在 CMake 配置阶段执行 version.sh 脚本，该脚本读取版本信息并
# 输出到 ijkversion.h，供 C 源码通过 #include 引用版本号。
# OUTPUT_VARIABLE / ERROR_VARIABLE 分别捕获脚本的标准输出和标准错误，
# 便于调试；STRIP_TRAILING_WHITESPACE 去除末尾多余换行。
# ============================================================
execute_process(
        COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/ijkmedia/ijkplayer/version.sh" "${CMAKE_CURRENT_SOURCE_DIR}/ijkmedia/ijkplayer" ijkversion.h
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
)

# ============================================================
# 头文件搜索路径
# ijkmedia 根目录及其四个子模块目录均加入 include 路径，
# 使各模块之间可以直接 #include 对方的头文件而无需写完整路径。
#   ijkj4a    —— JNI/Java 互调用辅助层
#   ijksdl    —— SDL 抽象层（音视频输出、线程等平台抽象）
#   ijkplayer —— 播放器核心逻辑
# ============================================================
set(ijkmedia ${CMAKE_CURRENT_SOURCE_DIR}/ijkmedia)
include_directories(${ijkmedia})
include_directories(${ijkmedia}/ijkj4a)
include_directories(${ijkmedia}/ijksdl)
include_directories(${ijkmedia}/ijkplayer)

# ============================================================
# 源文件收集
# 使用 GLOB_RECURSE 递归收集各模块目录下的所有 .c / .cpp 文件。
#   SOURCE_IJKJ4A    —— JNI 与 Java 层互调用的 C 胶水代码
#   SOURCE_SDL       —— SDL 抽象层实现（Android 音视频、OpenGL ES 渲染等）
#   SOURCE_IJKPLAYER —— 播放器核心（解封装、解码、同步、控制等）
#   SOURCE_SOUNDTOUCH—— SoundTouch 变速变调 C++ 封装（仅 wrap 文件）
# ============================================================
file(GLOB_RECURSE SOURCE_IJKJ4A ${ijkmedia}/ijkj4a/*.c  ${ijkmedia}/ijkj4a/*.cpp)
file(GLOB_RECURSE SOURCE_SDL ${ijkmedia}/ijksdl/*.c  ${ijkmedia}/ijksdl/*.cpp)
file(GLOB_RECURSE SOURCE_IJKPLAYER ${ijkmedia}/ijkplayer/*.c ${ijkmedia}/ijkplayer/*.cpp)
file(GLOB_RECURSE SOURCE_SOUNDTOUCH ${ijkmedia}/ijksoundtouch/ijksoundtouch_wrap.cpp)

# ============================================================
# 构建主共享库 libijkplayer.so
# 将上述所有源文件以及顶层的 prof.c（性能剖析辅助代码）
# 一同编译为 SHARED 动态库，供 Java 层通过 System.loadLibrary 加载。
# ============================================================
add_library(${CMAKE_PROJECT_NAME}
    SHARED
    prof.c          # 性能剖析辅助代码（CPU/内存采样等）
    ${SOURCE_IJKJ4A}
    ${SOURCE_SDL}
    ${SOURCE_IJKPLAYER}
    ${SOURCE_SOUNDTOUCH}
)

# ============================================================
# 链接器选项：设置最大页大小为 16 KB
# Android 15（API 35）及部分 arm64 设备使用 16 KB 内存页，
# 若 .so 不符合对齐要求则无法加载。此选项告知链接器按 16 KB 对齐
# ELF 段，使生成的库同时兼容 4 KB 和 16 KB 页大小的设备。
# ============================================================
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-Wl,-z,max-page-size=16384")

# ============================================================
# 第三方预编译静态库路径（按 ABI 区分目录）
# 所有第三方库均以预编译静态库（.a）形式提供，
# 最终会被静态链接进 libijkplayer.so，无需额外分发独立 .so。
# ============================================================
set(FFMPEG_LIB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/third-libs/ijkffmpeg/ijkffmpeg-${ARCH}/lib)
set(OPENSSL_LIB_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/third-libs/openssl/openssl-${ARCH}/lib)
set(SOUNDTOUCH_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third-libs/soundtouch/soundtouch-${ARCH}/lib)
set(YUV_LIB_DIR        ${CMAKE_CURRENT_SOURCE_DIR}/third-libs/yuv/yuv-${ARCH}/lib)

# 第三方库头文件路径（FFmpeg、SoundTouch、libyuv）
include_directories(
    third-libs/ijkffmpeg/ijkffmpeg-${ARCH}/include
    third-libs/soundtouch/soundtouch-${ARCH}/include
    third-libs/yuv/yuv-${ARCH}/include
)

# ============================================================
# SoundTouch 静态库（变速变调音频处理）
# STATIC IMPORTED 表示该库不由当前 CMake 构建，而是直接使用
# 预编译好的 .a 文件；IMPORTED_LOCATION 指定其磁盘路径。
# ============================================================
add_library(soundtouch-lib STATIC IMPORTED)
set_target_properties(soundtouch-lib PROPERTIES
    IMPORTED_LOCATION ${SOUNDTOUCH_LIB_DIR}/libSoundTouch.a
)

# ============================================================
# libyuv 静态库（YUV ↔ RGB 格式转换与缩放）
# 用于视频帧的色彩空间转换，性能高于 FFmpeg 内置的 swscale。
# ============================================================
add_library(yuv-lib STATIC IMPORTED)
set_target_properties(yuv-lib PROPERTIES
    IMPORTED_LOCATION ${YUV_LIB_DIR}/libyuv.a
)

# ============================================================
# OpenSSL INTERFACE 库（HTTPS/TLS 支持）
# INTERFACE 类型库本身不编译任何代码，仅作为"逻辑组合容器"，
# 将 libcrypto.a 和 libssl.a 两个静态库捆绑为一个依赖目标，
# 方便后续用一个名字统一引用 OpenSSL 的全部组件。
# ============================================================
add_library(openssl-lib INTERFACE)
target_link_libraries(openssl-lib INTERFACE
    ${OPENSSL_LIB_DIR}/libcrypto.a   # OpenSSL 密码学基础库
    ${OPENSSL_LIB_DIR}/libssl.a      # OpenSSL TLS/SSL 协议库
)

# ============================================================
# ijkffmpeg INTERFACE 库（定制版 FFmpeg）
# 同样使用 INTERFACE 将六个 FFmpeg 子库捆绑为一个依赖目标。
# 链接顺序很重要：静态库链接时符号按顺序解析，
# libavformat 依赖 libavcodec/libavutil 中的符号，
# 因此 avformat 必须排在最前面，avutil 排在最后。
# ============================================================
add_library(ijkffmpeg-lib INTERFACE)
target_link_libraries(ijkffmpeg-lib INTERFACE
    ${FFMPEG_LIB_DIR}/libavformat.a   # 封装/解封装（MP4、FLV、HLS 等）
    ${FFMPEG_LIB_DIR}/libavcodec.a    # 音视频编解码器
    ${FFMPEG_LIB_DIR}/libavfilter.a   # 音视频滤镜
    ${FFMPEG_LIB_DIR}/libswresample.a # 音频重采样
    ${FFMPEG_LIB_DIR}/libswscale.a    # 图像缩放与色彩转换
    ${FFMPEG_LIB_DIR}/libavutil.a     # 公共工具函数库（必须放最后）
)

# 查找系统 zlib（Android NDK 内置）
# FFmpeg 的封装层（avformat）在解析 ZIP/gzip 容器时依赖 zlib。
# CMake 默认会将 libz 排在 FFmpeg 之后，导致链接顺序错误，
# 因此手动将 libz 及 OpenSSL 追加到 ijkffmpeg-lib 的依赖中，
# 由此确保 libz → FFmpeg 的正确解析顺序；
# 主目标（libijkplayer.so）本身无需再单独链接 libz。
find_library(z-lib z REQUIRED)
target_link_libraries(ijkffmpeg-lib INTERFACE ${z-lib} openssl-lib)

# ============================================================
# 主库链接依赖汇总
# 将所有第三方库及 Android 系统库链接进 libijkplayer.so：
#   ijkffmpeg-lib  —— FFmpeg（含 zlib、OpenSSL）
#   yuv-lib        —— libyuv 色彩转换
#   soundtouch-lib —— 变速变调
#   log            —— Android 日志（__android_log_print 等）
#   android        —— Android Native API（AAssetManager、ANativeWindow 等）
#   EGL            —— OpenGL ES 上下文管理
#   GLESv2         —— OpenGL ES 2.0 渲染
#   jnigraphics    —— JNI 位图访问（AndroidBitmap_lockPixels 等）
#   OpenSLES       —— OpenSL ES 音频输出
#   m              —— C 标准数学库（sin/cos/sqrt 等）
# ============================================================
target_link_libraries(${CMAKE_PROJECT_NAME}
    ijkffmpeg-lib
    yuv-lib
    soundtouch-lib
    log
    android
    EGL
    GLESv2
    jnigraphics
    OpenSLES
    m
)
