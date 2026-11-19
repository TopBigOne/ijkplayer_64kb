# WS-FLV 验证清单

状态：待测 / 通过 / 失败。日期为最近一次更新。

## Android

通用准备（设备 R5CWA1RSV3T，Android 14）：

```
tools/ws-flv-live.sh start            # ws 18780 / wss 18781（A2/A3 用）
# 故障注入测试服务器（A5/A6/A11 共用同一个实例，--log 必填，/flv 需要 --flv）
python3 tests/ijkws/ws_test_server.py --port 18790 --flv tests/ijkws/build/test.flv --log /tmp/ws18790.jsonl &
adb -s R5CWA1RSV3T install -r android/ijkplayer/ijkplayer-example/build/outputs/apk/debug/ijkplayer-example-debug.apk
adb reverse tcp:18780 tcp:18780; adb reverse tcp:18781 tcp:18781; adb reverse tcp:18790 tcp:18790
# 清理：adb shell am force-stop tv.danmaku.ijk.media.example; adb reverse --remove-all; tools/ws-flv-live.sh stop; kill %1（或 pkill -f ws_test_server.py）
```

（上面的 ws_test_server 命令已在主机上验证：启动后对 `/flv` 握手返回 101，对 `/status/401` 返回 `HTTP/1.1 401`。）

启动 URL：`adb shell am start -n tv.danmaku.ijk.media.example/.activities.VideoActivity --es videoPath <URL>`

日志约定：native 日志 tag 为 `IJKMEDIA_YA`（带尾随空格的前缀，grep 时用 `IJKMEDIA`），消息事件由 jni 打印为 `FFP_MSG_ERROR: <码>` / `FFP_MSG_COMPLETED:`；示例 App 的 IjkVideoView 另打 `Error: <what>,<extra>`（tag IjkVideoView）。统一过滤：
`adb logcat -v time | grep -E "ijkws|FFP_MSG|IjkVideoView"`

| # | 项目 | 步骤 | 预期 | 状态 | 日期 |
|---|------|------|------|------|------|
| A1 | 构建与打包 | `./gradlew :ijkplayer-cmake:assembleRelease :ijkplayer-example:assembleDebug`；`unzip -l` AAR；`llvm-readelf -d libijkplayer.so` | AAR 两个 ABI 均含 libijkplayer.so 与 libwebsockets.so；NEEDED 含 libwebsockets.so | 通过 | 2026-11-19 |
| A2 | ws 起播 | 启动 `ws://127.0.0.1:18780/live/test.flv` | 有画面和声音 | 待测 | 2026-11-19 |
| A3 | wss 起播 | 启动 `wss://127.0.0.1:18781/live/test.flv` | 有画面和声音 | 待测 | 2026-11-19 |
| A4 | 退出耗时 | 播放中：`adb logcat -c`；`adb shell log -t mark BEFORE_BACK; adb shell input keyevent 4`；然后 `adb logcat -v time -d \| grep -E "BEFORE_BACK\|ijkmp_shutdown_l\|ijkws"`。比较 mark 行与 `ijkmp_shutdown_l()=void` 行的时间戳（ijkws 没有专门的 close 完成日志，shutdown 返回即包含 ijkws_close；若有 `ijkws: no close reply within 200 ms` 说明对端未回应 close） | mark 到 `ijkmp_shutdown_l()=void` <= 200ms（含返回键到 release 的少量 UI 开销，略超请复测并看 `ijkmp_shutdown_l()` 开始行单独量 shutdown 耗时） | 待测 | 2026-11-19 |
| A5 | 401 错误码 | 启动 `ws://127.0.0.1:18790/status/401` | 日志出现 `ijkws: connect ws://127.0.0.1:18790/status/401 failed: HTTP 401`，随后 `FFP_MSG_ERROR: -825242872`。AVERROR_HTTP_UNAUTHORIZED = -FFERRTAG(0xF8,'4','0','1') = -(0xF8 \| '4'<<8 \| '0'<<16 \| '1'<<24) = -825242872 | 待测 | 2026-11-19 |
| A6 | 读超时 | 启动 `ws://127.0.0.1:18790/silent`（默认 ws_timeout=10000，同 A5 的服务器）；`adb logcat -v time \| grep -E "ijkws\|FFP_MSG_ERROR\|FFP_MSG_OPEN_INPUT"` | open 到 `FFP_MSG_ERROR: -110` 约 10s（原 30s）；`ijkws: no data for 10000 ms, read timeout` 只出现 1 次 | 待测 | 2026-11-19 |
| A7 | Android 5.x 启动 | 在 API 21/22 设备或模拟器启动示例 App 并播放 ws 流 | 无 UnsatisfiedLinkError（验证 websockets 先于 ijkplayer 加载） | 待测（无此类设备则注明未验证） | 2026-11-19 |
| A8 | DNS 阶段打断 | 没有网络管理权限时没有可靠的全自动方案。可行做法：设备 Wi-Fi 改静态 IP，DNS 填一个黑洞地址（如 10.255.255.1，同网段不存在的主机，查询会超时而不是秒拒绝）；用从未解析过的域名起播，如 `ws://nx-$(date +%s).example.com:18790/flv`，起播后 1s 内按返回键（`adb shell input keyevent 4`）。不要用 Private DNS 的 `hostname` 模式（失败是即时的，达不到卡住的效果）。测完恢复 Wi-Fi 设置 | 返回后界面立即响应，logcat 无 ANR（`adb shell dumpsys activity \| grep -i anr` 无新增），进程不卡死 | 待测 | 2026-11-19 |
| A9 | http-flv 回归 | 播放示例 App 原有 http-flv 条目 | 仍正常播放 | 待测 | 2026-11-19 |
| A10 | armv7a 设备 | 在 armeabi-v7a 设备重复 A2/A3 | 正常播放 | 待测 | 2026-11-19 |
| A11 | 正常结束与断连 | (a) 启动 `ws://127.0.0.1:18790/flv`（服务器发完文件后发 close 1000）；(b) 启动 `ws://127.0.0.1:18790/drop`（发 64KB 后直接断 TCP） | (a) 日志出现 `FFP_MSG_COMPLETED:`，没有 `FFP_MSG_ERROR`；(b) `FFP_MSG_ERROR: -5` | 待测 | 2026-11-19 |

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
