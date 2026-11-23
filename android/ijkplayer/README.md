# ijkplayer Android 工程

基于 bilibili ijkplayer 的 Android 工程，用 CMake + NDK 编译 native 播放器，产出 AAR，附带示例 App。
各 module 的作用见 [当前module是干嘛的.md](当前module是干嘛的.md)。

| module              | 说明                                                           |
|---------------------|--------------------------------------------------------------|
| `ijkplayer-cmake`   | 播放器库，编译 `ijkmedia/` 下的 C 代码，产出 `ijkplayer-cmake-release.aar` |
| `ijkplayer-example` | 示例 App（包名 `tv.danmaku.ijk.media.example`），可以选本地文件或输入 URL 播放  |

## 新增功能

### 1. WebSocket 拉流直播（ws-flv / wss-flv）

播放器可以直接播放 `ws://` 和 `wss://` 地址的 FLV 直播流。

- **实现**：native 层新增 ijkws（`ijkmedia/ijkplayer/ijkavformat/ijkws.c`），基于 libwebsockets 实现一个自定义 AVIO。`ff_ffplay` 遇到 ws/wss 地址时走 ijkws，其他协议的处理不变。
- **业务侧用法**：和播放 http-flv 一样，直接 `setDataSource("ws://...")`。可选参数通过 format 选项设置：

  ```java
  player.setOption(IjkMediaPlayer.OPT_CATEGORY_FORMAT, "ws_timeout", 10000);   // 读超时，毫秒，默认 10000
  player.setOption(IjkMediaPlayer.OPT_CATEGORY_FORMAT, "ws_headers", "Authorization: Bearer xxx"); // 握手附加 Header，多条用换行分隔
  ```

  其他选项：`ws_origin`、`ws_protocol`（子协议）、`ws_tls_verify`（默认 0，不校验证书）、`ws_ca_file`（校验证书时使用的 CA 文件）。
- **错误码**（`onError` 的 extra）：读超时 `-110`，断线或域名解析失败 `-5`，握手被拒返回对应的 HTTP 错误码（如 401 为 `-825242872`）。服务端正常发 close 帧时回调播放完成，不报错。
- **so 库**：libwebsockets 打包成独立的 `libijkwebsockets.so`，没有用 `libwebsockets.so` 这个名字，避免和第三方 SDK 冲突。自定义 so 加载器时，需要在 `ijkplayer` 之前加载 `ijkwebsockets`。
- **构建前提**：全新 clone 后，先在仓库根目录执行 `tools/build-libwebsockets-android.sh`，编出 lws 产物后再构建 AAR 或 App。

更多资料（路径相对仓库根目录）：

- 设计文档：`doc/superpowers/specs/2026-11-19-ws-flv-libwebsockets-design.md`
- 验证清单与集成须知：`doc/ws-flv-verification.md`

### 2. 本地推流测试环境：[tools/ws-live.sh](tools/ws-live.sh)

在 Mac 上搭一个 WebSocket 直播源，手机连同一 Wi-Fi 后直接拉流。

```
ffmpeg ──RTMP──▶ SRS(1935) ──HTTP-FLV(8080)──▶ relay ──ws:18780 / wss:18781──▶ 手机
```

**依赖**：SRS 编译产物（默认 `/Users/dev/Documents/AV/srs/trunk`，可用 `SRS_HOME` 覆盖）、ffmpeg（默认 `/usr/local/ffmpeg/4.4`，可用 `FFMPEG` 覆盖）、python3。

在本目录（`android/ijkplayer`）下执行：

```bash
tools/ws-live.sh start              # 启动推流：测试图 + 正弦音
tools/ws-live.sh start --camera     # 或者推 Mac 摄像头 + 麦克风
tools/ws-live.sh start --file a.mp4 # 或者循环推本地文件（H.264/AAC 直接 copy，不重编码）
                                    # 已在运行时，start 会先自动停掉再按新参数启动

tools/ws-live.sh url                # 打印手机可用的地址，如 ws://<Mac局域网IP>:18780/live/test.flv
tools/ws-live.sh check              # 用局域网 IP 做 ws/wss 握手检查，结果应为 101
tools/ws-live.sh play [ws|wss]      # 通过 adb 让示例 App 播放（不用 adb 时，在 App 里手动输入 url 打印的地址）

tools/ws-live.sh status
tools/ws-live.sh stop
```

看播放日志：`adb logcat -v time | grep -E "ijkws|FFP_MSG|IjkVideoView"`

**常见问题**

- 测试图 / 摄像头模式下，`ffmpeg.log` 一直报 `Invalid DTS ... replacing by guess`：本机 ffmpeg 4.4 没有 libx264，只能用 h264_videotoolbox 编码，它会忽略 `-bf 0` 继续输出 B 帧。muxer 已经修正了时间戳，不影响播放。
- `check` 返回 502：推流没起来，查看 `tests/ijkws/build/live/` 下的 `ffmpeg.log` 和 `srs.log`（路径相对仓库根目录）。
- `check` 通过但手机连不上：确认手机和 Mac 在同一网段，路由器没有开启 AP 隔离或客户端隔离，并检查 macOS 防火墙。
- Mac 的 IP 取错了（比如同时连了有线和 Wi-Fi）：用 `WS_LIVE_IP=x.x.x.x tools/ws-live.sh url` 指定。
- 示例 App「Sample → WebSocket 拉流」里的 adb reverse 条目指向 `127.0.0.1`，只有 USB 连接并执行了 `adb reverse tcp:18780 tcp:18780`（wss 还需 `tcp:18781`）时才能播；走局域网时点「输入自定义地址」，填 `url` 命令打印的地址（会记住上次输入）。

### 3. 直播延迟：端到端测量、HUD 显示与追帧

- **端到端延迟**：推流端给每个视频帧插一个带 NTP 时间的 SEI，播放器在帧送显时计算「当前时刻 - 推流端时刻」，HUD 实时显示，并拆分出网络段和播放端各占多少。推流加 `--sei`：

  ```bash
  SRS_CONF=$PWD/../../tools/latency/srs-low-latency.conf tools/ws-live.sh start --file a.mp4 --sei
  ```

- **低延迟播放**：示例 App「设置 → 直播延迟」里有低延迟模式、以视频为主时钟、追帧（加速播放）、丢 GOP 跳关键帧、HUD 告警阈值、NTP 服务器。
- **Java 接口**：`IjkMediaPlayer.getE2eLatency()` / `getE2eRecvLatency()` / `setE2eClockOffset()`；新增 player 选项 `live-chase*`、`live-drop-ms`、`av-sync-type`。
- 原理、SEI 格式、实测数据和调优建议见仓库根目录 `doc/live-latency.md`。

**其他测试工具**（在仓库根目录执行）

- `tools/ws-flv-live.sh`：推流环境的实际实现，`ws-live.sh` 只是在它之上加了一层封装。
- `tools/ws-flv-soak.sh`：长时间挂流、频繁切流的稳定性测试。
- `tests/ijkws/run.sh`：ijkws 在 Mac 主机上的单元和集成测试；`tests/ijkws/ws_test_server.py` 可以模拟 401、静默、断线等异常情况。

## 构建

```bash
./gradlew :ijkplayer-cmake:assembleRelease      # AAR
./gradlew :ijkplayer-example:assembleDebug      # 示例 App
```

详细的构建说明见仓库根目录的 `doc/android_build.md`。
