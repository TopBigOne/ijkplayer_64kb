# Android 编译指南

本文档记录在 macOS（Apple Silicon）上编译 Android 版 ijkplayer 的完整流程。

## 前置条件

- Xcode Command Line Tools（提供 git / curl）
- Android Studio（含 Android SDK、cmake）
- Android NDK（任意较新版本即可；`ijkplayer-cmake/build.gradle` 未锁定具体版本，由 AGP 自动选择）
- JDK 17

## 目录结构简述

```
ijkplayer_64kb/
├── FFToolChain/          # git submodule，三方库下载/编译工具链
├── ijkmedia/             # 核心播放器 C 代码（跨平台）
└── android/
    ├── install-ffmpeg.sh # 下载预编译三方库
    ├── build-aar.sh      # 编译 aar
    └── ijkplayer/        # Android Studio 工程（gradle）
        └── ijkplayer-cmake/  # 产出 aar 的模块
```

## 编译步骤

### 1. 拉取代码与子模块

```bash
git clone git@github.com:TopBigOne/ijkplayer_64kb.git
cd ijkplayer_64kb
git submodule update --init --recursive
```

`git submodule update --init --recursive` 会拉取 [FFToolChain](../FFToolChain)（三方库工具链仓库）。若之前只 clone 没带子模块，`FFToolChain/` 目录会是空的，必须先跑这一步。

### 2. 下载预编译三方库

```bash
cd android
chmod +x install-ffmpeg.sh build-aar.sh   # 首次执行需要加权限
./install-ffmpeg.sh
```

该脚本实际调用：

```bash
../FFToolChain/main.sh install -l 'soundtouch yuv ijkffmpeg' -p android
```

**注意**：这一步是从 [MRFFToolChainBuildShell Releases](https://github.com/debugly/MRFFToolChainBuildShell/releases) **下载已编译好的静态库**（`soundtouch`、`libyuv`、`ijkffmpeg`），覆盖 arm64-v8a / armeabi-v7a / x86 / x86_64 四个 ABI，并不在本地从源码编译 ffmpeg，因此：

- 不需要 nasm / pkg-config 等 ffmpeg 编译工具
- 依赖网络能访问 GitHub
- 编译产物版本受 [FFToolChain/configs/libs/ijkffmpeg.sh](../FFToolChain/configs/libs/ijkffmpeg.sh) 中 `PRE_COMPILE_TAG_ANDROID` 锁定的 tag 控制，当前为 FFmpeg 4.0（bilibili fork，`ff4.0--ijk0.8.8--20210426--001`）

下载解压后的库位于 `FFToolChain/build/product/android/`，并同步进 `android/ijkplayer/ijkplayer-cmake/src/main/cpp/third-libs/`。

### 3. 编译 aar

```bash
./build-aar.sh
```

实际执行：

```bash
cd ijkplayer
./gradlew :ijkplayer-cmake:assemble
```

Gradle 通过 CMake + NDK，将 [ijkmedia/](../ijkmedia) 下的核心播放器 C 代码与第 2 步下载的静态库一起编译为 `.so`，再打包为 aar。首次构建约 1 分钟（含 4 个 ABI 全量编译）。

### 4. 产物

```
android/ijkplayer/ijkplayer-cmake/build/outputs/aar/
├── ijkplayer-cmake-debug.aar
└── ijkplayer-cmake-release.aar
```

## 运行 Demo

用 Android Studio 打开 `android/ijkplayer` 工程，运行 `ijkplayer-example` module（依赖上面编译出的 `ijkplayer-cmake` 模块）。

## 常见问题

- **`FFToolChain` 目录为空 / `git submodule status` 前有 `-` 号**：说明子模块未初始化，执行 `git submodule update --init --recursive`（少了 `--init` 只会更新已初始化过的子模块，不会做任何事）。
- **想自己改 ffmpeg 裁剪配置（编解码器、协议开关等）**：需要走 `main.sh init` + `main.sh compile` 从源码交叉编译，涉及真实的 NDK 工具链、nasm 等，流程比预编译下载复杂得多，本文档未覆盖。
