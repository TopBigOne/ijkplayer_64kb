# VS Code IntelliSense 配置（解决 #include 找不到的报红）

用 VS Code 打开本项目时，`ijkmedia/` 下涉及 Android 的源码（比如 `ijkj4a/j4a/j4a_base.h` 里的 `#include <android/log.h>`）会被 C/C++ 插件标红，提示找不到头文件。**这只是编辑器 IntelliSense 的问题，不影响真实编译**——`android/install-ffmpeg.sh` + `android/build-aar.sh` 走的是 Gradle + CMake + NDK 的真实工具链，跟 VS Code 插件是否认识这些头文件无关。

## 报红的原因

- `android/log.h` 这类头文件不是标准 C 库自带的，是 **Android NDK 的 sysroot** 里才有的
- **Android Studio** 通过 Gradle 的 `externalNativeBuild { cmake {...} }` 自动跟 CMake/NDK 集成，天然知道去哪找这些头文件
- **VS Code** 装的通用 C/C++ 插件默认不知道 NDK 装在哪、该用哪个 ABI/API level 的 sysroot，所以看到这类 include 就报红

## 修复步骤

### 1. 确认已装好所需插件

本项目用到两个 VS Code 插件：`CMake Tools`（`ms-vscode.cmake-tools`）和微软官方 `C/C++`（`ms-vscode.cpptools`）。检查/安装：

```bash
"/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code" --list-extensions --show-versions | grep -i "cmake\|cpptools"
# 没装的话：
"/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code" --install-extension ms-vscode.cmake-tools
"/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code" --install-extension ms-vscode.cpptools
```

（如果 `code` 命令已经在 PATH 里，直接用 `code --install-extension ...` 即可，不需要写完整应用包路径。）

### 2. 手动跑一次 CMake configure，生成 `compile_commands.json`

`compile_commands.json` 记录了每个源文件真正编译时用的完整参数（含 NDK sysroot 路径、宏定义等），C/C++ 插件读到这份文件就能精确定位头文件，不再依赖插件自己瞎猜。

```bash
mkdir -p android/ijkplayer/ijkplayer-cmake/.cxx/vscode-intellisense
cd android/ijkplayer/ijkplayer-cmake/.cxx/vscode-intellisense

cmake -G "Unix Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=<你的NDK根目录>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-21 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  ../../src/main/cpp
```

本机实测用的是 NDK `28.2.13676358`（`/Users/dev/Documents/Android_Env/sdk/ndk/28.2.13676358`），`ANDROID_PLATFORM=android-21` 对应 [ijkplayer-cmake/build.gradle](../android/ijkplayer/ijkplayer-cmake/build.gradle) 里的 `minSdk 21`。只需要单个 ABI（`arm64-v8a`）就够 IntelliSense 用，不需要四个 ABI 都配。

生成结果放在 `android/ijkplayer/ijkplayer-cmake/.cxx/vscode-intellisense/`，这个路径已经被 [android/ijkplayer/.gitignore](../android/ijkplayer/.gitignore) 里的 `.cxx` 规则忽略，不会被误提交。

### 3. 写 `.vscode/settings.json` 把两者串起来

```json
{
    "cmake.sourceDirectory": "${workspaceFolder}/android/ijkplayer/ijkplayer-cmake/src/main/cpp",
    "cmake.buildDirectory": "${workspaceFolder}/android/ijkplayer/ijkplayer-cmake/.cxx/vscode-intellisense",
    "cmake.configureSettings": {
        "CMAKE_TOOLCHAIN_FILE": "<你的NDK根目录>/build/cmake/android.toolchain.cmake",
        "ANDROID_ABI": "arm64-v8a",
        "ANDROID_PLATFORM": "android-21",
        "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"
    },
    "cmake.configureOnOpen": true,
    "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
    "C_Cpp.default.compileCommands": "${workspaceFolder}/android/ijkplayer/ijkplayer-cmake/.cxx/vscode-intellisense/compile_commands.json"
}
```

### 4. 重新打开报红的文件 / reload window

回 VS Code，把报红的文件关掉重新打开，或者 `⌘⇧P` → `Developer: Reload Window`，让编辑器读到新配置、刷新 IntelliSense 缓存。

## 验证方法（不要只靠"感觉应该好了"）

从生成的 `compile_commands.json` 里，把报错文件对应的真实编译命令抠出来，加上 `-fsyntax-only`（只做语法检查，不产出目标文件）单独跑一遍，退出码 0 且无报错，才能确认真的修好了：

```bash
python3 -c "
import json, subprocess, shlex
data = json.load(open('android/ijkplayer/ijkplayer-cmake/.cxx/vscode-intellisense/compile_commands.json'))
for entry in data:
    if entry['file'].endswith('j4a_allclasses.c'):
        cmd = entry['command'].replace(' -c ', ' -fsyntax-only ')
        result = subprocess.run(shlex.split(cmd), cwd=entry['directory'], capture_output=True, text=True)
        print('退出码:', result.returncode)
        print(result.stderr or '(无报错)')
        break
"
```

## 注意事项

- `.vscode/settings.json` 和 `CMAKE_TOOLCHAIN_FILE` 里写的是**本机 NDK 的绝对路径**，换一台电脑或者换个 NDK 版本这个路径就失效了。如果团队要共享这份配置，建议要么把路径改成用环境变量（`$ANDROID_NDK_HOME` 之类）间接引用，要么就不要提交 `.vscode/` 目录，让每个人自己按这份文档跑一遍第 2、3 步。
- 这套配置只影响编辑器体验（跳转定义、自动补全、报红与否），跟真实的 Android 编译流程（[doc/android_build.md](android_build.md)）完全独立，互不影响。
