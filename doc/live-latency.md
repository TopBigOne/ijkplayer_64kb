# 直播延迟：测量与优化

本文说明直播"推流端一帧画面 → 拉流端显示出来"的延迟怎么测、ijkplayer 这边做了哪些优化，以及在本地 ws-flv 环境里实测的结果。

## 1. 两种测量方法

| 方法 | 测的是什么 | 优点 | 缺点 |
|---|---|---|---|
| 拍照法 | 真正的 glass-to-glass：镜头采集 → 屏幕发光 | 不改代码，结果最可信，验收常用 | 只能人工读数，不能实时看 |
| SEI 时间戳 | 推流端打时间戳 → 播放器送显 | HUD 实时显示，还能分出网络段和播放端各占多少 | 不含采集和编码耗时；两端时钟要对齐 |

### 拍照法

1. Mac 屏幕上打开一个毫秒秒表网页，用摄像头对着它推流（`tools/ws-live.sh start --camera`），或者直接录屏推流。
2. 手机放在 Mac 屏幕旁边拉流播放。
3. 用第三台设备拍一张照片，同时拍到原始秒表和手机画面里的秒表，两个读数相减就是延迟。多拍几张取平均。

## 2. SEI 端到端延迟（重点）

### 2.1 原理

FLV / RTMP 里的时间戳是从 0 开始的相对时间，只表示帧和帧之间隔了多久，没有"这一帧是几点几分拍的"这个信息，所以播放器单靠自己算不出端到端延迟。

SEI（Supplemental Enhancement Information，补充增强信息）是 H.264 / H.265 码流里专门放附加数据的 NAL 单元。解码器不认识的 SEI 会直接忽略，不影响画面；服务器（SRS 等）和转封装（ffmpeg `-c copy`）也会原样透传。所以它很适合带一个"拍摄时刻"：

```
推流端                         服务器 / 网络                  播放端
每帧插入 SEI(T_push) ──RTMP──▶ SRS ──HTTP-FLV──▶ relay ──ws──▶ 读包时记下 (pts, T_push)
                                                              该帧送显时：延迟 = T_now - T_push
```

安防摄像头（IPC）常见的"SEI 时间戳 / 私有 SEI 水印"也是这个机制，在固件编码时写入。

### 2.2 SEI 格式

```
H.264: NAL type 6 (SEI)          H.265: NAL type 39 (prefix SEI)
  payloadType = 5                  user_data_unregistered
  payloadSize = 24
  payload     = UUID "AIJK_E2E_LATENCY"（16 字节 ASCII）
              + int64 大端，Unix 毫秒，已做 NTP 校正
  rbsp_trailing_bits = 0x80
```

写入时要加防竞争字节（`00 00 0x` → `00 00 03 0x`，x ≤ 3），解析时去掉。时间戳的 8 个字节里经常有 `00`，不加防竞争字节就会被误判成起始码。

插入位置：每个视频帧的第一个 NAL 前面；如果第一个 NAL 是 AUD（访问单元分隔符），就插在 AUD 后面，因为 AUD 必须在最前。

### 2.3 时钟对齐

延迟 = 手机时刻 - 推流端时刻，两台机器时钟差多少，结果就差多少。实测 Mac 的系统时钟比 NTP 慢 2.4 秒，测试过程中还被系统校时服务拨动过一次 1.8 秒。所以：

- 推流端和手机向**同一台 NTP 服务器**（默认 `ntp.aliyun.com`）做 SNTP 校时，取 3 次里 RTT 最小的一次，误差一般在 10ms 以内。
- 推流端以单调时钟为基准换算 NTP 时间，每 60 秒重新校时，系统时钟被拨动也不受影响。
- 手机端：App 起播时校时一次（结果缓存 10 分钟），通过 `setE2eClockOffset()` 传给播放器。HUD 的"时钟校正"一行显示偏差和 RTT。目前手机端以系统时钟为基准，播放途中手机系统时钟被拨动的话，要到下次校时才恢复（实际很少发生）。

### 2.4 播放端实现

代码在 `ijkmedia/ijkplayer/ff_e2e_latency.h`（只被 `ff_ffplay.c` 包含，iOS 工程不用加文件）：

1. **读包线程**：视频包入队前，按 avcC / hvcC 的 NAL 长度字节数（Annex B 也支持）遍历 NAL，找到上面的 SEI 后，把 `(pts, T_push)` 存进 64 项环形缓冲。同时记下**到达延迟** = 读到包的时刻 - T_push。
2. **显示线程**：一帧送显（`SDL_VoutDisplayYUVOverlay`）后，按这帧的 pts 找环形缓冲里 pts 不大于它的最近一条：
   - 每帧都有 SEI 时就是这一帧自己；
   - 只有关键帧带 SEI 时用 `T_push + (pts - pts_sei)` 推算（最多推算 5 秒）。

   **端到端延迟** = 当前时刻 + 时钟校正 - 推算出的 T_push。
3. 3 秒内没收到这种 SEI，接口返回 -1，HUD 显示"流里没有时间戳 SEI"。
4. 每 2 秒打一条日志，可以用 `adb logcat | grep "e2e latency"` 批量采集：

   ```
   e2e latency: 237 ms (recv 81 ms, vcache 0 ms, acache 0 ms, pictq 0, sampq 2, clock offset 67 ms)
   ```

Java 接口（`IjkMediaPlayer`）：

| 方法 | 说明 |
|---|---|
| `getE2eLatency()` | 端到端延迟（ms），没有 SEI 时 -1 |
| `getE2eRecvLatency()` | 到达延迟（ms）：推流 → 服务器 → 网络，端到端减去它就是播放端耗时 |
| `setE2eClockOffset(ms)` | 本机时钟校正 = NTP 时间 - `System.currentTimeMillis()` |

### 2.5 推流端

`tools/latency/flv_sei_inject.py`：

```
ffmpeg(采集+编码 → FLV 管道) ─▶ flv_sei_inject.py(每个视频 tag 插 SEI) ─▶ ffmpeg(-c copy → RTMP)
```

已接入本地环境，加 `--sei` 即可：

```bash
# 在 android/ijkplayer 目录下
SRS_CONF=$PWD/../../tools/latency/srs-low-latency.conf tools/ws-live.sh start --file a.mp4 --sei
```

注意：

- 时间戳在**编码器输出之后**打，不含采集和编码耗时。要看完整的 glass-to-glass 延迟，用拍照法补上这一段。
- 回放文件时脚本自动加 `--cts`：`-re` 按 dts 发帧，有 B 帧时 P 帧会比它的显示时刻早 cts 发出，所以时间戳记为"发出时刻 + cts"。这种情况下，HUD 上 P 帧的"网络段"可能是负数，属于正常现象。
- `tools/latency/flv_latency_probe.py` 可以在链路中间任何一跳测延迟（读 HTTP-FLV，或 `ijkws_cat ... | probe -`），用来定位延迟花在哪一段。

## 3. 播放端优化（示例 App「设置 → 直播延迟」）

| 设置 | 对应 player 选项 | 作用 |
|---|---|---|
| 低延迟模式 | `packet-buffering=0`、`infbuf=1`、format `fflags=nobuffer`、`probesize=64K`、`analyzeduration=0.5s`、codec `thread_type=slice` | 读到就播，不等水位线；软解不用帧级多线程（N 线程会让输出晚 N-1 帧） |
| 以视频为主时钟 | `av-sync-type=1`（新增选项） | 画面不再等音频输出缓冲，音频跟着视频调整 |
| 追帧 | `live-chase=1`、`live-chase-start-ms=500`、`live-chase-stop-ms=200`、`live-chase-rate=120`（新增）、`soundtouch=1` | 缓冲超过 500ms 时 1.2 倍速播放，降到 200ms 以下恢复。音频主时钟时音频变速（SoundTouch，不变调；OpenSL ES 输出本身不支持变速），纯视频或视频主时钟时缩短帧间隔，帧队列里有帧排队时也加速 |
| 缓冲超过该值时直接跳到关键帧 | `live-drop-ms`（新增） | 清空包队列并 flush 解码器，丢视频包直到下一个关键帧。画面会跳一下；GOP 长的流（比如 5 秒以上）要等很久，不建议开 |
| HUD 延迟告警阈值 | - | HUD 上端到端延迟和缓冲延迟超过它就标红，默认 300ms |
| NTP 服务器 | - | 端到端延迟校时用，留空不校时 |

## 4. 实测（本地 ws-flv，Wi-Fi，测试文件 test_1.mp4）

测试文件：256x144、**6fps**、H.264 Main **带 B 帧**（重排 1 帧）、GOP 5.5–7 秒、AAC。

| 配置 | 端到端延迟 | 主要瓶颈 |
|---|---|---|
| 默认设置 | ~11.3 s | SRS GOP 缓存的积压一直留在播放缓冲里（缓冲 8–9 s） |
| + 低延迟模式、追帧 | 1.4–2.4 s | 软解帧级多线程（6fps 下多线程延迟 1 秒多） |
| + 软解改 slice 线程 | 1.15–1.35 s | OpenSL ES 加 SoundTouch 的输出缓冲（音频主时钟时视频要等它） |
| + AudioTrack | ~0.9 s | SRS 合并写（mw_latency 350ms）；推流端 mp4 音视频分块交织 |
| + SRS 低延迟配置、视频主时钟、帧队列追帧、按 pts 打时间戳 | **215–270 ms**（偶尔 340–490） | 网络段 ~80–200ms；6fps 下一帧就是 166ms |
| 同一内容用 FFmpeg 6.0 重编码：25fps、无 B 帧、GOP 1 s | **40–180 ms**，中位数 ~90 ms | 起播后十几秒在追 GOP 缓存的积压 |
| FFmpeg 6.0 实时编码测试图（640x360，25fps，无 B 帧） | **60–250 ms**，中位数 ~160 ms | 分辨率更高，软解更慢 |

重编码命令（FFmpeg 6.0）：

```bash
/Users/dev/Documents/AV/FFmpeg-6.0/install_macos/bin/ffmpeg -i test_1.mp4 -t 120 -vf fps=25 \
    -c:v h264_videotoolbox -profile:v main -bf 0 -g 25 -b:v 600k -c:a copy t1_lowlat.flv
```

各段在 Mac 上用 `flv_latency_probe.py` 测到的结果：推流转封装 ~150ms（B 帧 dts 偏移）、SRS 默认配置平均 350ms / 最大 800ms，改低延迟配置后降到 176ms、ws relay 基本为 0。

## 5. 要稳定控制在 300ms 以内，还要做的事

1. **推流端不用 B 帧**（baseline 或 `-bf 0`），帧率 25fps 以上。B 帧至少多一帧重排；帧率低的话，一帧本身就很长。ffmpeg 4.4 的 videotoolbox 会忽略 `-bf 0`，baseline 会报 "DTS is invalid"；**FFmpeg 6.0 的 videotoolbox 已正常支持**。本地推流用 6.0：

   ```bash
   FFMPEG=/Users/dev/Documents/AV/FFmpeg-6.0/install_macos \
   SRS_CONF=$PWD/../../tools/latency/srs-low-latency.conf tools/ws-live.sh start --sei
   ```
2. **GOP 1–2 秒**：首屏更快，丢 GOP 追帧的代价也更小。
3. **服务器开低延迟**：SRS 用 `min_latency on`、`mw_latency 0`、`mr off`（见 `tools/latency/srs-low-latency.conf`）。
4. **硬解（MediaCodec）**：没有帧级多线程问题。部分机型硬解内部会压几帧，用 HUD 的"播放端"一栏确认。
5. 以画面为主的场景用**视频主时钟**，并开追帧。
