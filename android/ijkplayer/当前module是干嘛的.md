# 模块说明

本工程（`settings.gradle`）下有三个 module，实际状态和作用各不相同。

## ijkplayer-cmake — 核心播放器库（活跃，参与编译）

- **类型**：`com.android.library`，通过 CMake + NDK 编译原生代码
- **作用**：将 [ijkmedia/](../../ijkmedia)（跨平台核心 C 代码：播放器逻辑 `ijkplayer`、渲染层 `ijksdl`、JNI 封装 `ijkj4a`）编译为 `.so`，链接下载好的 `ijkffmpeg`（FFmpeg 4.0）/ `soundtouch` / `libyuv` 静态库，产出 `ijkplayer-cmake-release.aar`
- **入口**：`src/main/cpp/CMakeLists.txt`
- **构建**：`./gradlew :ijkplayer-cmake:assemble`（`../../build-aar.sh` 的封装）
- **依赖前提**：需先执行 `../../install-ffmpeg.sh` 下载三方静态库到 `src/main/cpp/third-libs/`

## ijkplayer-example — Demo App（活跃，参与编译）

- **类型**：`com.android.application`，可直接安装运行
- **作用**：官方示例 App，提供选择本地文件 / 输入 URL 播放视频的界面，用于验证 `ijkplayer-cmake` 的播放能力
- **依赖**：`implementation project(':ijkplayer-cmake')`
- **运行方式**：Android Studio 打开本工程，选中 `ijkplayer-example` 运行配置，装到真机/模拟器

## ijkplayer-exo — ExoPlayer 兼容层（历史遗留，未启用）

- **作用**：早期设计用来让上层业务代码通过统一接口在 ijkplayer 和 Google 官方 [ExoPlayer](https://github.com/google/ExoPlayer) 之间切换
- **现状**：
  - **未被 `settings.gradle` include**，不参与当前工程构建
  - `build.gradle` 用的是老版本 Gradle DSL（`compile` 而非 `implementation`）
  - 依赖 `project(':ijkplayer-java')`，但该 module 在当前工程中已不存在（早期架构是 `ijkplayer-java` + 按 ABI 拆分的 native module，现已被单一的 `ijkplayer-cmake` 取代）
- **建议**：可视为废弃代码，无需理会；如需 ExoPlayer 兼容能力需要重新评估、独立实现
