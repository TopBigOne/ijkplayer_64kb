# WS-FLV 验证清单

状态：待测 / 通过 / 失败。日期为最近一次更新。

## Android

通用准备（设备 R5CWA1RSV3T，Android 14）：

```
tools/ws-flv-live.sh start            # ws 18780 / wss 18781
adb -s R5CWA1RSV3T install -r android/ijkplayer/ijkplayer-example/build/outputs/apk/debug/ijkplayer-example-debug.apk
adb reverse tcp:18780 tcp:18780; adb reverse tcp:18781 tcp:18781
# 清理：adb shell am force-stop tv.danmaku.ijk.media.example; adb reverse --remove-all; tools/ws-flv-live.sh stop
```

启动 URL：`adb shell am start -n tv.danmaku.ijk.media.example/.activities.VideoActivity --es videoPath <URL>`

| # | 项目 | 步骤 | 预期 | 状态 | 日期 |
|---|------|------|------|------|------|
| A1 | 构建与打包 | `./gradlew :ijkplayer-cmake:assembleRelease :ijkplayer-example:assembleDebug`；`unzip -l` AAR；`llvm-readelf -d libijkplayer.so` | AAR 两个 ABI 均含 libijkplayer.so 与 libwebsockets.so；NEEDED 含 libwebsockets.so | 通过 | 2026-11-19 |
| A2 | ws 起播 | 启动 `ws://127.0.0.1:18780/live/test.flv` | 有画面和声音 | 待测 | 2026-11-19 |
| A3 | wss 起播 | 启动 `wss://127.0.0.1:18781/live/test.flv` | 有画面和声音 | 待测 | 2026-11-19 |
| A4 | 退出耗时 | 播放中按返回键；`adb logcat -v time \| grep ijkws` | 退出到 ijkws close 完成 <= 200ms | 待测 | 2026-11-19 |
| A5 | 401 错误码 | 测试服务器 `ws_test_server.py --port 18790 ...`，`adb reverse tcp:18790 tcp:18790`，启动 `ws://127.0.0.1:18790/status/401` | FFP_MSG_ERROR 等于 AVERROR_HTTP_UNAUTHORIZED 对应值 | 待测 | 2026-11-19 |
| A6 | 读超时 | 启动 `ws://127.0.0.1:18790/silent`（默认 ws_timeout=10000） | open 到 FFP_MSG_ERROR -110 约 10s（原 30s），日志只出现 1 次 "no data for 10000 ms" | 待测 | 2026-11-19 |
| A7 | Android 5.x 启动 | 在 API 21/22 设备或模拟器启动示例 App 并播放 ws 流 | 无 UnsatisfiedLinkError（验证 websockets 先于 ijkplayer 加载） | 待测（无此类设备则注明未验证） | 2026-11-19 |
| A8 | DNS 阶段打断 | 使 DNS 不可用（或用解析很慢的域名的 ws URL），起播后立即返回 | 不卡顿，无 ANR | 待测 | 2026-11-19 |
| A9 | http-flv 回归 | 播放示例 App 原有 http-flv 条目 | 仍正常播放 | 待测 | 2026-11-19 |
| A10 | armv7a 设备 | 在 armeabi-v7a 设备重复 A2/A3 | 正常播放 | 待测 | 2026-11-19 |

## iOS

模拟器：iPhone 16 Pro（iOS 18.5）。测试服务器：`python3 tests/ijkws/ws_test_server.py --port 18865 --tls-port 18866 --cert ... --key ... --flv test.flv --log ...`（test.flv 为 5s testsrc+sine，证书为 CN=localhost 自签名）。
模拟器上直接访问 Mac 的 127.0.0.1。手动测试走 Demo 的 Input URL（已允许 ws/wss）；自动化测试时临时加了启动参数钩子，未提交。
iOS 上 ETIMEDOUT 是 60（Darwin），所以读超时的错误码是 -60，Android 上是 -110。

| # | 项目 | 步骤 | 预期 | 状态 | 日期 |
|---|------|------|------|------|------|
| I1 | 构建与链接 | `cd ios && ./build-framework.sh`；对 `ios/IJKMediaPlayer/Release-{iphoneos,iphonesimulator}/IJKMediaFramework.framework/IJKMediaFramework` 跑 `nm -gU` 和 `otool -L` | 构建成功；两个产物都定义了 lws_create_context 和 ijkws_open；没有 libwebsockets 动态库依赖（静态链入） | 通过 | 2026-11-19 |
| I2 | 模拟器 ws 起播 | `ws://127.0.0.1:18865/flv` | FFP_MSG_PREPARED，AUDIO/VIDEO_RENDERING_START（音视频都开始渲染） | 通过 | 2026-11-19 |
| I3 | 模拟器 wss 起播 | `wss://localhost:18866/flv` | 同 I2（截图确认有画面） | 通过 | 2026-11-19 |
| I4 | 正常结束 | I2/I3 播完，服务端发 close 1000 | FFP_MSG_COMPLETED，Demo 收到 FinishReasonPlaybackEnded（之前是 FFP_MSG_ERROR） | 通过 | 2026-11-19 |
| I5 | 断连错误码 | `ws://127.0.0.1:18865/drop`（发 64KB 后直接断 TCP，不发 close 帧） | 起播后 FFP_MSG_ERROR -5（EIO） | 通过 | 2026-11-19 |
| I6 | 读超时 | `ws://127.0.0.1:18865/silent`（默认 ws_timeout=10000） | 大约 10s 后 FFP_MSG_ERROR -60（ETIMEDOUT），"no data for 10000 ms" 只出现 1 次 | 通过 | 2026-11-19 |
| I7 | 静默时退出 | 打开 `/silent`，4s 后关闭播放器 | 退出耗时约 0.2s，服务端收到带掩码的 close 1000 | 通过 | 2026-11-19 |
| I8 | 真机 ws 起播 | Input URL 输入 `ws://<Mac 局域网 IP>:<端口>/flv` | 有画面和声音（需要配置签名） | 待测 | 2026-11-19 |
| I9 | 真机 wss 起播、退出耗时、错误码 | 同 I3–I7 | 同上 | 待测 | 2026-11-19 |
| I10 | 真机 DNS 阶段打断 | 打开 Network Link Conditioner 的 100% Loss，用域名 ws URL 起播后马上退出 | 不卡顿 | 待测 | 2026-11-19 |
| I11 | http-flv 回归 | 播放原有 http-flv 地址 | 仍正常播放 | 待测（模拟器上用本地 `python3 -m http.server` 测试时，请求一直没有到达服务器，原因未查，跟 ws 改动无关） | 2026-11-19 |
