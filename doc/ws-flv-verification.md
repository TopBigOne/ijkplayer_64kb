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

## 稳定性（Task 9）

工具：`tools/ws-flv-soak.sh`（基于 `tests/ijkws/build/ijkws_cat`，缺则自动 `tests/ijkws/build.sh cat`）。可指向任意 ws/wss 地址，`-o key=value` 原样传给 ijkws（可重复）。

```
tools/ws-flv-soak.sh soak   [-o k=v]... <url> <duration_s>            # 挂流，每 10s 采样 RSS 和累计字节，CSV 在 tests/ijkws/build/soak/<时间戳>/
tools/ws-flv-soak.sh switch [-o k=v]... <url> <count> <interval_s>    # 反复 起进程->收到>=1字节->SIGINT，统计失败/relay 连接数/ESTABLISHED
```

判定：soak 在 ijkws_cat 提前退出、退出码非 0、RSS 首尾增长 > 10MB 或没收到数据时 FAIL；switch 在任一轮失败、relay_connect 与 relay_close 数量不等（读 `tests/ijkws/build/live/relay.jsonl`，可用 `RELAY_LOG` 覆盖，文件不存在则跳过该项）或 URL 端口上还有 ESTABLISHED 连接时 FAIL。wss 自签名证书加 `-o ws_tls_verify=0`。

### Mac 缩短版（本地 live 环境）

先 `tools/ws-flv-live.sh start`，跑完 `tools/ws-flv-live.sh stop`。

| # | 项目 | 命令 | 预期 | 状态 | 日期 |
|---|------|------|------|------|------|
| M1 | 挂流 10 分钟 | `tools/ws-flv-soak.sh soak ws://127.0.0.1:18780/live/test.flv 600` | 不提前退出，RSS 首尾增长 < 10MB | 通过：603s，23007496 字节，均速 37.3 KB/s，RSS 起/止/最大 7648/4816/7648 KB，退出码 0 | 2026-11-19 |
| M2 | ws 切流 300 次 | `tools/ws-flv-soak.sh switch ws://127.0.0.1:18780/live/test.flv 300 1` | 0 失败；relay connect == close；无残留 ESTABLISHED | 通过：335s，0 失败，connect=300 close=300，ESTABLISHED=0 | 2026-11-19 |
| M3 | wss 切流 50 次 | `tools/ws-flv-soak.sh switch -o ws_tls_verify=0 wss://127.0.0.1:18781/live/test.flv 50 1` | 同 M2 | 通过：55s，0 失败，connect=50 close=50，ESTABLISHED=0 | 2026-11-19 |

Mac 缩短版只覆盖 ijkws 本身（无解码、无播放器），不能替代 S1/S2/S6 的真机验证。

### 待测（真机 / 真实服务器）

以下均未执行（未提供生产地址，Android 设备未连接）。

| # | 项目 | 步骤 | 预期 | 状态 | 日期 |
|---|------|------|------|------|------|
| S1 | Android 24h 挂流 | 示例 App 播放 ws 流（本地 live 环境用 `adb reverse`，或 `<PROD_WS_URL>`），屏幕常亮（`adb shell svc power stayon true`，测完 `false`）。每小时记录：`adb shell dumpsys meminfo tv.danmaku.ijk.media.example \| grep -E "TOTAL\|Native Heap\|Java Heap"` 和 `adb shell dumpsys gfxinfo tv.danmaku.ijk.media.example \| head -5`。可用循环：`while true; do date; adb shell dumpsys meminfo tv.danmaku.ijk.media.example \| grep -E "TOTAL PSS\|Native Heap"; sleep 3600; done >> meminfo.log` | 24h 不崩溃、不卡顿；TOTAL PSS 和 Native Heap 末值相对第 1 小时增长 < 10MB，无持续线性增长；logcat 无 ijkws 报错 | 待测 | 2026-11-19 |
| S2 | iOS 24h 挂流 | 真机 Demo 的 Input URL 填 ws 地址，Xcode 从 Debug Navigator 的 Memory 仪表观察，或每小时在 Instruments 里用 Allocations/Leaks 取一次快照；也可 `leaks <pid>`（模拟器进程）。屏幕保持常亮（Demo 播放时默认不息屏，否则设置自动锁定为永不） | 24h 不崩溃；Memory 曲线平稳（增长 < 10MB），Leaks 无 ijkws / lws 相关泄漏 | 待测 | 2026-11-19 |
| S3 | 弱网（Android） | 部分设备开发者选项没有网络限速。可行做法：Mac 开热点（共享网络）并把设备连上，Mac 上装 Xcode 的 Additional Tools 里的 Network Link Conditioner，设为 3G / Very Bad Network / 100% Loss 等档位，设备 URL 用 Mac 热点 IP（`ws://<Mac 热点 IP>:18780/live/test.flv`，live 环境监听 0.0.0.0 即可）。依次测：带宽 256kbps、延迟 500ms、丢包 10% | 画面卡顿但不崩溃；缓冲时 UI 可响应；恢复网络后继续播放；无 ANR | 待测 | 2026-11-19 |
| S4 | 弱网（iOS 真机） | 设备「设置 > 开发者 > Network Link Conditioner」，依次选 Very Bad Network / 3G / 100% Loss，各播 5 分钟 | 同 S3 | 待测 | 2026-11-19 |
| S5 | 断网 30s 恢复 | 播放中断开网络 30s（Android：开飞行模式；iOS：控制中心飞行模式，或 100% Loss），再恢复。Android 抓 `adb logcat -v time \| grep -E "ijkws\|FFP_MSG_ERROR"`（飞行模式 adb 无线连接会断，用 USB）。恢复后 App 内重新 prepare/播放 | 断网后在 ws_timeout（默认 10000ms）内出现错误回调（Android -110、iOS -60，或连接已被系统断开时 -5/-104）；恢复网络后重新播放成功，无需重启 App | 待测 | 2026-11-19 |
| S6 | 设备端 1/s 切流 30 分钟 | 在示例 App（或临时测试代码）里每秒执行一次 stop/release 并重新创建播放器 prepare 同一 ws URL，共 1800 次；期间 `adb shell dumpsys meminfo <pkg>` 每 5 分钟记录一次；iOS 用 Xcode Memory 仪表。同时在 Mac 上看 relay：`grep -c relay_connect tests/ijkws/build/live/relay.jsonl` 与 `grep -c relay_close ...` | 无崩溃/ANR；PSS 增长 < 10MB；relay 的 connect 数 == close 数；`netstat -an \| grep 18780 \| grep -c ESTABLISHED` 回到 0 | 待测 | 2026-11-19 |
| S7 | 其他协议回归 | 示例 App（Android 和 iOS）依次播放：http-flv、hls（.m3u8）、rtsp、本地文件 | 全部仍正常起播和退出，无 ijkws 相关日志 | 待测 | 2026-11-19 |
| S8 | 真实服务器挂流 | Mac 上：`tools/ws-flv-soak.sh soak -o 'ws_headers=Authorization: Bearer <TOKEN>' <PROD_WS_URL> 3600`（无需鉴权则不带 `-o`；多个 header 用换行分隔，`\r\n` 或 `\n` 均可；wss 用正规证书则不用 `ws_tls_verify`） | PASS：进程不提前退出，RSS 增长 < 10MB，平均码率与服务器码流一致 | 待测 | 2026-11-19 |
| S9 | 真实服务器切流 | `tools/ws-flv-soak.sh switch -o 'ws_headers=Authorization: Bearer <TOKEN>' <PROD_WS_URL> 300 1`（relay 日志检查自动跳过；服务端若有连接数监控请对照观察是否归零） | 0 失败，结束后本机 ESTABLISHED 为 0 | 待测 | 2026-11-19 |
