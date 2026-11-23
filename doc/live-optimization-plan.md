# 直播卡顿、追帧、延迟优化计划（参考 SimpleFFPlay6.0）

本文对照 SimpleFFPlay6.0（基于 FFmpeg 6.0 ffplay 改造，macOS / VideoToolbox）在直播上的优化，列出 ijkplayer（Android）可以借鉴的项目、改动位置、工作量和风险，供后续排期实施。

- 参考项目：`/Users/dev/Documents/AV/W_3/SimpleFFPlay6.0`，文档在 `docs/`（优化记录.md、live_catchup_algorithm.md、live_streaming_tuning.md、live_reliability.md 等）
- 现状基线：`doc/live-latency.md`（SEI 端到端延迟测量、追帧、调优建议）
- 本文代码行号基于 master `01bda850`，实施前请重新核对

> SimpleFFPlay 的效果数据全部来自本机回环和合成流，作者自己写了尚未在真实摄像头上验证。移植后需在我们的场景下重新测。

## 1. 差距总览

| # | SimpleFFPlay 的机制（提交） | 类别 | ijkplayer 现状 |
|---|---|---|---|
| 1 | 断线重连：4s 读不到数据或 IO 错误即重连，退避 0/1/2/3/4/5s，连续 6 次无进展才放弃；先建新连接再替换旧连接（7880533） | 可靠性 | 部分有：只有 URL 加 `ijklivehook:` 前缀才重连（`ijklivehook.c`），无限重试、无退避；ws 走自定义 AVIO，不经过该钩子，断线只上报 `FFP_MSG_ERROR`（-5） |
| 2 | 首次连接失败也重试（566277e） | 可靠性 | 没有：打开失败直接 `goto fail` |
| 3 | 看门狗：数据在到但 3s 无新画面，逐级执行 清队列 → 重建解码器 → 重连；帧队列满（如暂停、后台）时不动作（ff4d4ee、1b493b6） | 可靠性 | 没有 |
| 4 | 积压时等关键帧：关键帧到达且比当前画面超前 600ms 以上，清空队列从该关键帧开始（94a320e） | 延迟 | 部分有：`live-drop-ms` 先清空，再由 `live_drop_filter` 丢到下一个关键帧，期间画面冻结，最长一个 GOP |
| 5 | 自适应目标延迟：以"到达时刻 − pts"的 2s 最小值为基准，目标 = 抖动均值 + 2.33σ，限制 30~300ms，涨快降慢；抖动离群值限幅 ±3.5σ；过点帧丢弃（94a320e、ff4d4ee） | 延迟 | 部分有：`live_latency_control` 按包队列时长判断，阈值固定 500/200ms，追帧方式为加速播放；无抖动估计 |
| 6 | 音频衔接：断音时 5ms 渐弱，恢复或跳变时 5ms 交叉淡化（ff4d4ee） | 音质 | 没有：`sdl_audio_callback` 遇到 serial 变化直接填 0，有咔哒声 |
| 7 | 音频稳定晚到时接受滞后（≤0.5s），连续播放中不插静音（1b493b6） | 音质 | 没有：`synchronize_audio` 只做 ±10% 采样补偿 |
| 8 | 运行指标：帧率、延迟分位、冻结、重连、看门狗次数（1b493b6） | 可观测性 | 部分有：`ffp_statistic_l`、SEI 端到端延迟、`LIVE_DROP_COUNT`；没有冻结、重连、看门狗计数和延迟分位 |
| 9 | 丢包不花屏：出现坏帧后停在最后一个好帧，等干净关键帧（≤10s）（a534b6d） | 画质 | 没有 |
| 10 | 故障注入测试：11 个场景，网络模拟器 + 咔哒检测（2586999） | 测试 | 部分有：tools/ 下有 ws 稳定性、频繁切换脚本，无丢包 / 抖动注入和自动判定 |

我们领先的一项：ijkplayer 有基于 SEI 的端到端延迟测量（含 NTP 校时），SimpleFFPlay 只能测相对延迟。

## 2. 优化清单（按建议顺序）

### P0：直播低延迟模式下部分 HLS 无声（0.5 人天）

- **现象**：开启"直播低延迟"后，部分 HLS 源报 `Could not find codec parameters ... (Audio: aac, 0 channels)`，没有声音。FFmpeg 4.0 与 7.1 表现一致。
- **原因（推断，待确认）**：`IjkVideoView.applyLiveLatencyOptions` 对所有源都把 `probesize` 压到 64K、`analyzeduration` 压到 0.5s。HLS 一次要探测多个码率档，探测数据不够。SimpleFFPlay 的低延迟模式没有压 `probesize`。
- **改法**（二选一）：
  - 只对 flv / ws / rtmp 压探测参数，HLS 保持默认；
  - 或在 `avformat_find_stream_info` 之后检查音频流 `sample_rate` / 声道数为 0 时，用默认参数重新探测。
- **位置**：`android/ijkplayer/ijkplayer-example/.../widget/media/IjkVideoView.java` 的 `applyLiveLatencyOptions`。
- **验证**：mux 的 HLS 测试流（`https://test-streams.mux.dev/x36xhzz/x36xhzz.m3u8`）开启低延迟后应有声音。

### P1：积压改为"关键帧到达时再跳"（0.5~1 人天）

- **目标**：消除 `live-drop-ms` 触发时最长一个 GOP 的画面冻结。
- **改法**：把"先清空队列、再丢到下一个关键帧"改成"关键帧到达且积压超过阈值时，清掉旧包，把这个关键帧入队"。
- **位置**：`ff_ffplay.c` 的 `live_latency_control`、`live_drop_filter`。
- **风险**：低，与 MediaCodec 无关。

### P2：读线程级断线重连 + 首次连接重试（3~5 人天）

- **目标**：ws-flv 等直播断线后自动恢复，不再直接报错退出。
- **位置**：`ff_ffplay.c` `read_thread` 的打开段（ws 走 `ijkws_open`）和读错误分支；`decode_interrupt_cb` 增加超时截止时间。参考 SimpleFFPlay `src/demux/read_thread.c` 的 `handle_disconnect` / `reopen_input`。
- **两种实现**：
  - 关掉音视频组件后整体重开：简单，但 MediaCodec 要重建（涉及 JNI 和 Surface），较重；
  - 流参数不变时只替换 `ic` / `pb`，再塞 `flush_pkt` 走 serial 刷新（类似现有对 DISCONTINUITY 的处理）：能复用解码器，但要处理 `is->video_st` 等仍指向旧 `ic` 的指针。
- **注意**：
  - `ijkws_open` 会消费 `format_opts`，重连前要先复制一份；
  - 重连策略参考：退避 0/1/2/3/4/5s，连续 6 次无进展才放弃，最终仍按现有错误码上报（握手失败 HTTP 码、EIO）；
  - Java 层要能区分"重连中"和"出错"：新增消息，或复用 `MEDIA_INFO_BUFFERING_START/END`，否则 App 看到的是 ERROR。
- **验证**：沿用真机回归里"播放中杀掉 ws 转发进程"的用例，期望先进入缓冲，转发恢复后继续出画面；转发一直不恢复时，按退避次数用完后上报 `-5`。

### P3：音频淡入淡出（1~2 人天）

- **目标**：消除断音、seek、追帧丢帧时的咔哒声。
- **改法**：在 serial 变化、`audio_decode_frame` 返回负值、追帧丢帧这几处加 5ms 淡出 / 淡入。SimpleFFPlay 的 `src/live/audio_smooth.c` 基本可以照搬。
- **位置**：`ff_ffplay.c` 的 `sdl_audio_callback`（Android 和 iOS 共用）。
- **注意**：要和 SoundTouch 变速的输出衔接，先确认采样格式（S16 还是 float）；OpenSL ES 和 AudioTrack 两条输出路径都要测。

### P4：看门狗（2~3 人天，第 3 级依赖 P2）

- **目标**：有数据却长时间不出画面（解码器卡死）时自动恢复。
- **改法**：在 `read_thread` 主循环检测"数据在到但 N 秒无新画面"，逐级执行：
  1. 清队列（可先单独做）；
  2. 重建解码器（MediaCodec 走 pipenode 重建）；
  3. 重连（依赖 P2）。
- **必须保留的规则**：帧队列满时不动作（暂停、App 切后台、Surface 销毁都会导致队列满）；`packet-buffering` 缓冲期间不计时。

### P5：补充运行指标（1 人天）

- 在 `ffp_statistic_l` 和 Java `_getPropertyLong` 增加：冻结次数与时长、重连次数、断线时长、看门狗触发次数、延迟分位（P50/P95）。
- example 的 HUD 直接展示，便于验证 P2~P4。

### P6：自适应目标延迟（4~6 人天）

- **目标**：根据网络抖动自动设定追帧目标，替代固定的 500/200ms。
- **改法**：在 `read_thread` 记录包的到达时刻，按 SimpleFFPlay 的算法估计抖动、计算目标延迟。第一步只用估计值替换 `live_latency_control` 的固定阈值，保留现有的加速追帧方式；再评估是否改成纯丢帧追。
- **风险**：MediaCodec 内部会缓存几帧，可能把抖动估计抬高，需要实测（未确认）。

## 3. 不建议移植或需先验证

| 项 | 原因 |
|---|---|
| 丢包不花屏（第 9 项） | 我们的源是 FLV / ws / HLS，走 TCP，基本不丢包；MediaCodec 也不报单帧错误。FFmpeg 7.1 是否把包的损坏标记传到帧上未确认，收益低 |
| 视频为主时钟 + 音频接受滞后（第 7 项） | SimpleFFPlay 是安防场景"画面实时优先"的取舍；有人声的直播要先评估约 200ms 的音画差能否接受 |
| VideoToolbox 硬解相关 | 平台不同，不适用 |
| 故障注入测试框架（第 10 项） | 依赖 macOS 桌面会话和 SDL 窗口，Android 上要重写驱动；可以先把它的 `netem_relay.py` 接到现有 ws 转发链路上，做丢包、抖动注入 |

## 4. 关键代码位置

| 项目 | 位置 |
|---|---|
| SimpleFFPlay | `src/demux/read_thread.c`（重连、看门狗）、`src/live/live_latency.c`（自适应延迟）、`src/live/audio_smooth.c`（淡入淡出）、`src/video/video_decoder_thread.c`（`frame_integrity_ok`） |
| ijkplayer | `ijkmedia/ijkplayer/ff_ffplay.c`（`read_thread`、`live_latency_control`、`live_drop_filter`、`sdl_audio_callback`、`synchronize_audio`、`ffp_statistic_l`）、`ff_e2e_latency.h`、`ijkavformat/ijklivehook.c`、`ijkavformat/ijkws.c`、`android/.../IjkVideoView.java` |
