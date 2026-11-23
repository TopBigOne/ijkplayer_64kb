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

脚本默认分三步：

1. `main.sh install -l 'soundtouch yuv ijkffmpeg'`：从 [MRFFToolChainBuildShell Releases](https://github.com/debugly/MRFFToolChainBuildShell/releases) 下载上游预编译包。soundtouch、libyuv 直接使用；openssl 只打包在 ijkffmpeg 的 zip 里，借此装上（上游没有单独的 Android openssl 包）
2. `main.sh init -l ijkffmpeg`：拉取 bilibili FFmpeg 源码（`ff4.0--ijk0.8.8--20210426--001`，FFmpeg 4.0）
3. `main.sh compile -l ijkffmpeg -c rebuild`：按自有 fork 的 `module-lite.sh` 配置源码编译 arm64 / armv7a，覆盖第 1 步下载的 ijkffmpeg（启用 rtmps 等定制）

因此需要：能访问 GitHub、设置 `ANDROID_NDK_HOME`、`brew install nasm pkg-config`。缺少时脚本直接报错退出，不会自动安装。首次约 2~3 分钟（预编译 zip 缓存在 `FFToolChain/build/pre/`，之后不再重复下载）。

只想快速拿到能编译的环境、不需要定制时，可以用 `./install-ffmpeg.sh --prebuilt`，全部用上游预编译包（**不含 rtmps 等定制**）。

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
- **想自己改 ffmpeg 裁剪配置（编解码器、协议开关等）**：见下一节「从源码编译 ijkffmpeg」。

## 从源码编译 ijkffmpeg（启用 rtmps 等自定义配置）

`FFToolChain` 子模块指向自有 fork（`git@github.com:TopBigOne/MRFFToolChainBuildShell.git`，`ijk-custom` 分支），在上游基础上有以下改动：

- `configs/ijk-ffmpeg-config/module-lite.sh`：启用 `rtmps` 协议（RTMP over TLS）
- `tools/export-android-host-env.sh`：依赖命令已在 PATH 时不再调用 `brew install`（新版 brew 的 pkg-config 由 pkgconf 提供，原检测会误判）

第 2 步的 `./install-ffmpeg.sh` 默认已经包含下面的源码编译流程。本节用于单独重编 ijkffmpeg（例如只改了 `module-lite.sh`），或排查编译问题。

**注意**：`./install-ffmpeg.sh --prebuilt` 用的是上游作者的预编译包，**不含上述改动**（例如不支持 `rtmps://`）。

### 额外前置条件

- `ANDROID_NDK_HOME` 指向 NDK 目录
- `brew install nasm pkg-config`
- 已执行过第 2 步（需要其中的 openssl、soundtouch、yuv 预编译库；ijkffmpeg 通过 pkg-config 自动发现 openssl）

### 手动命令

```bash
cd FFToolChain

# 1. 拉取 bilibili FFmpeg 源码（tag ff4.0--ijk0.8.8--20210426--001）到 build/src/android/
./main.sh init -p android -l ijkffmpeg -a "armv7a arm64"

# 2. 交叉编译（两个 ABI 约 1~2 分钟）
./main.sh compile -p android -l ijkffmpeg -a "armv7a arm64"
# 修改过 module-lite.sh 后需重新 configure，用 rebuild：
# ./main.sh compile -p android -l ijkffmpeg -a "armv7a arm64" -c rebuild
```

- 只编 `armv7a arm64`，与 `ijkplayer-cmake` / `ijkplayer-example` 的 `abiFilters` 一致；`-a` 不支持 `all`
- 产物直接覆盖 `FFToolChain/build/product/android/universal/ijkffmpeg/ijkffmpeg-{arm64,armv7a}/`，CMake 通过 `third-libs` 软链接读取，无需额外拷贝
- 之后不要再跑 `install-ffmpeg.sh --prebuilt`，否则 ijkffmpeg 会被上游预编译包覆盖回去（不带参数的 `install-ffmpeg.sh` 没问题）

### 验证

```bash
# 配置是否生效（应为 1）
grep CONFIG_RTMPS_PROTOCOL FFToolChain/build/product/android/universal/ijkffmpeg/ijkffmpeg-arm64/config.h

# 重新编 so 后检查符号
cd android/ijkplayer && ./gradlew :ijkplayer-cmake:assemble
strings ijkplayer-cmake/build/intermediates/stripped_native_libs/release/stripReleaseDebugSymbols/out/lib/arm64-v8a/libijkplayer.so \
  | grep -E '^ff_(rtmps|rtmp|tls|https)_protocol$'
```

### 修改工具链并提交

```bash
cd FFToolChain
git checkout ijk-custom
# ……修改……
git commit -am "..." && git push            # 推到自有 fork
cd .. && git add FFToolChain && git commit -m "FFToolChain: 更新子模块指针"

# 同步上游作者更新
cd FFToolChain && git fetch upstream && git merge upstream/master
```

> 新 clone 的仓库执行 `git submodule update --init` 后，子模块处于 detached HEAD，且没有 `upstream` remote；需要修改时先 `git checkout ijk-custom`，再 `git remote add upstream https://github.com/debugly/MRFFToolChainBuildShell.git`。
