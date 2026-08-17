# HEVC 解码流程梳理

本文档梳理一条 HEVC (H.265) 视频流在 ijkplayer 里从解封装到渲染上屏的完整链路，涵盖 Android（MediaCodec）和 iOS（VideoToolbox）两条硬解路径，以及软解回退逻辑。所有结论均已对照源码核实，标注 `文件:行号`。

## 0. 核心结论：硬解/软解由 option 开关决定，不是自动探测编码格式

播放器**不使用** FFmpeg 原生的 `hwaccel`（`AVCodecContext.hw_device_ctx`/`get_format` 回调）机制。硬解是 ijkplayer 在 FFmpeg 之外用一套 `ffpipeline`/`ffpipenode` 抽象自己实现的旁路系统，跟 avcodec 硬解 API 无关。

是否走硬解，第一道闸门是应用层设置的 option 开关，**默认全部关闭**：

- Android：`mediacodec-hevc`（对应 `ffp->mediacodec_hevc`）、`mediacodec-avc`、`mediacodec-mpeg2`、`mediacodec-mpeg4`、`mediacodec-all-videos`，定义在 [ijkmedia/ijkplayer/ff_ffplay_options.h:181-196](../ijkmedia/ijkplayer/ff_ffplay_options.h)，默认值全部是 `OPTION_INT(0, 0, 1)`
- iOS：`videotoolbox` 开关，同一份文件里对应位置，默认同样是 0

**即使设备完全支持硬解 HEVC，App 不显式设置 `mediacodec-hevc=1` / `videotoolbox=1`，播放器也只会走 FFmpeg 内置软解码器。** 不是"检测到 HEVC 就自动尝试硬解"。

第二道闸门是能力探测，但两平台实现方式完全不同（见第 4 节）。

## 1. Demux 阶段：拿到 codec_id = AV_CODEC_ID_HEVC

`read_thread`（[ff_ffplay.c:3052](../ijkmedia/ijkplayer/ff_ffplay.c)）里：

1. `avformat_open_input` 打开容器（`ff_ffplay.c:3113`）
2. `avformat_find_stream_info` 探测流信息，此时 `AVStream->codecpar->codec_id` 已被设为 `AV_CODEC_ID_HEVC`（`ff_ffplay.c:3160`）
3. `av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, ...)` 选出视频流下标（`ff_ffplay.c:3238`）
4. 调用 `stream_component_open(ffp, st_index[AVMEDIA_TYPE_VIDEO])`（`ff_ffplay.c:3276`）

到这里为止和编码格式无关，是标准 FFmpeg demux 流程。

## 2. `stream_component_open`：打开"软件" AVCodecContext + 触发 pipeline

[ijkmedia/ijkplayer/ff_ffplay.c:2798-3017](../ijkmedia/ijkplayer/ff_ffplay.c)

1. `avcodec_alloc_context3` + `avcodec_parameters_to_context` 把 `AV_CODEC_ID_HEVC` 灌进 `avctx`（`2814-2821`）
2. `avcodec_find_decoder(avctx->codec_id)` 找到 FFmpeg 内置的软件 hevc 解码器（`2823`）
3. **无论最终走不走硬解，`avcodec_open2(avctx, codec, &opts)` 都会被无条件调用**（`2867`）。硬解时这份 `avctx` 只用来读 `codecpar`（宽高、profile/level、extradata），不会被拿去跑真正的 `avcodec_receive_frame` 解码——这是一处"多余但无害"的资源开销
4. `AVMEDIA_TYPE_VIDEO` 分支（`2936-2993`）：
   - `decoder_init` 把 `avctx` 存进 `Decoder`
   - **`ffp->node_vdec = ffpipeline_open_video_decoder(ffp->pipeline, ffp)`（`2950`/`2956`）—— 真正决定走硬解还是软解的调用点**
   - `decoder_start(&is->viddec, video_thread, ...)` 启动解码线程（`2960`）

`stream_component_open` 本身完全不感知具体编码格式，profile 白名单逻辑下放到了具体的 pipenode 里。

## 3. Pipeline 抽象层分流

接口定义：[ff_ffpipeline.h:35-44](../ijkmedia/ijkplayer/ff_ffpipeline.h)，核心函数指针 `func_open_video_decoder`。

### Android — [ffpipeline_android.c:68-80](../ijkmedia/ijkplayer/android/pipeline/ffpipeline_android.c)

```c
static IJKFF_Pipenode *func_open_video_decoder(IJKFF_Pipeline *pipeline, FFPlayer *ffp)
{
    IJKFF_Pipenode *node = NULL;
    if (ffp->mediacodec_all_videos || ffp->mediacodec_avc || ffp->mediacodec_hevc || ffp->mediacodec_mpeg2)
        node = ffpipenode_create_video_decoder_from_android_mediacodec(ffp, pipeline, opaque->weak_vout);
    if (!node) {
        node = ffpipenode_create_video_decoder_from_ffplay(ffp);   // 回退软解
    }
    return node;
}
```

只要任一 mediacodec 开关打开，就先尝试创建硬解 node；创建失败（返回 NULL）自动 fallback 到软解 node。

### iOS — `ffpipeline_ios.c:38-57`

```c
static IJKFF_Pipenode *func_open_video_decoder(IJKFF_Pipeline *pipeline, FFPlayer *ffp)
{
    IJKFF_Pipenode* node = NULL;
    if (ffp->videotoolbox) {
        node = ffpipenode_create_video_decoder_from_ios_videotoolbox(ffp);
        if (!node)
            ALOGE("vtb fail!!! switch to ffmpeg decode!!!! \n");
    }
    if (node == NULL) {
        node = ffpipenode_create_video_decoder_from_ffplay(ffp);
    }
    return node;
}
```

结构与 Android 完全对称：单一 `videotoolbox` 开关 + 失败自动回退软解。

## 4. 硬解内部判定细节（两平台差异最大的地方）

### 4.1 Android — [ffpipenode_android_mediacodec_vdec.c](../ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c)

创建入口：`ffpipenode_create_video_decoder_from_android_mediacodec`（`1904-2086`）

1. `SDL_Android_GetApiLevel() < IJK_API_16_JELLY_BEAN` 直接返回 NULL（`1907-1908`）
2. `switch (opaque->codecpar->codec_id)`（`1943-2032`）：
   - `AV_CODEC_ID_H264`：检查开关后，有 **profile 白名单**，`FF_PROFILE_H264_HIGH_10`/`HIGH_422`/`HIGH_444` 等一律拒绝（`1965-1991`）
   - **`AV_CODEC_ID_HEVC`（`1997`）：只检查 `ffp->mediacodec_hevc || ffp->mediacodec_all_videos`，没有任何 profile/level 白名单**——比 H264 分支明显更"宽松"，能否真正硬解完全靠系统和 Java 层兜底
   - `default:` 不认识的编码格式一律走软解
3. `ffpipeline_select_mediacodec_l`（`2057`）——真正跨到 Java 层做设备能力探测和具体 decoder 选型：
   `ffpipeline_select_mediacodec_l`（`ffpipeline_android.c:271-281`）→ JNI 回调 `mediacodec_select_callback`（`android/ijkplayer_jni.c:859-878`）→ Java `IjkMediaPlayer.onSelectCodec` → `DefaultMediaCodecSelector` 遍历 `MediaCodecList.getCodecInfoAt()`（`android/ijkplayer/ijkplayer-cmake/src/main/java/tv/danmaku/ijk/media/player/IjkMediaPlayer.java:1217-1264`，不在 `ijkmedia/` C 核心代码里，但是完整链路的必要一环）
   如果 Java 侧没找到合适 codec，`ffpipenode_create_video_decoder_from_android_mediacodec` 直接失败，触发第 3 节的软解回退

**HEVC extradata 转换**：`recreate_format_l`（`172-272`）里判断是 hvcC box 格式时（`183-184`），调用 `convert_hevc_nal_units` 把 hvcC 转成 Annex-B 格式的 SPS/PPS/VPS 塞进 `csd-0`；逐包喂数据时 `feed_input_buffer`/`feed_input_buffer2`（`569`、`853-854`）对 H264/HEVC 都会做 Annex-B 转换。

同步/异步两种主循环由 `ffp->mediacodec_sync` 决定（`1924-1928`）：
- `func_run_sync`（`1562-1678`，默认）：独立线程喂 input buffer，主线程专职取输出
- `func_run_sync_loop`（`1513-1560`，`mediacodec-sync=1` 时启用）：单线程轮询喂入+取出

### 4.2 iOS — `ffpipenode_ios_videotoolbox_vdec.m` + `IJKVideoToolBoxSync/Async.m`

创建入口：`ffpipenode_create_video_decoder_from_ios_videotoolbox`（`94-134`）

1. iOS 版本 < 8.0 直接返回 NULL（`98-100`）
2. `switch (avctx->codec_id)`：只接受 `AV_CODEC_ID_H264` 和 `AV_CODEC_ID_HEVC`（`113-115`），不支持 MPEG2/MPEG4 硬解（与 Android 不同）
3. `ffp->vtb_async` 决定异步还是同步会话（`116-119`）

真正的能力探测在 `IJKVideoToolBoxSync.m` 的 `vtbformat_init`（`898-1014`）：

```objc
case AV_CODEC_ID_HEVC:
    format_id = kCMVideoCodecType_HEVC;
    if (@available(iOS 11.0, *)) {
        isHevcSupported = VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC);
    } else {
        isHevcSupported = false;
    }
    if (!isHevcSupported) {
        goto fail;
    }
    break;
```
（`940-951`）

**iOS 在 native 代码层面做了真实的硬件能力探测**（`VTIsHardwareDecodeSupported`），iOS 11 以下版本 HEVC 硬解必然失败（硬编码 `false`）。这与 Android 明显不同——Android 把能力探测这一步完全甩给了 Java 层，C 代码里看不到。

`VTDecompressionSessionCreate` 真正创建硬解会话（`449`），失败则触发第 3 节的软解回退。

## 5. 解码执行：三条路径完全不共享 avcodec 解码调用

线程入口统一：`video_thread`（`ff_ffplay.c:2329-2338`）—— 不管走哪条路径，都只是 `ffpipenode_run_sync(ffp->node_vdec)`，真正干活的是各 pipenode 自己的 `func_run_sync`。

- **软解路径**：`ffpipenode_ffplay_vdec.c` 的 `func_run_sync` → `ffp_video_thread` → `ffplay_video_thread`（`ff_ffplay.c:2166-2327`，标准 ffplay 解码循环）→ `get_video_frame`（`1679-1720`）→ `decoder_decode_frame`（`557-647`，真正的 `avcodec_send_packet`/`avcodec_receive_frame` 在这里）。产出标准 YUV420P 等像素格式的 `AVFrame`
- **Android MediaCodec 路径**：完全不调用 `avcodec_send_packet`/`avcodec_receive_frame`；输入走 `feed_input_buffer`（把裸 NAL 数据喂给 Java `MediaCodec`），输出走 `drain_output_buffer` + `amc_fill_frame`（`422-452`）——把 MediaCodec 输出的 buffer index 包成 `SDL_AMediaCodecBufferProxy` 挂到 `frame->opaque`，`frame->format` 设成 `IJK_AV_PIX_FMT__ANDROID_MEDIACODEC`（`440`）
- **iOS VideoToolbox 路径**：`videotoolbox_video_thread`（`43-69`）循环调用 `VTDecompressionSessionDecodeFrame`，解出的帧通过 `QueuePicture`（`IJKVideoToolBoxSync.m:227-244`）把 `frame->opaque` 设为 `CVPixelBufferRef`，`frame->format = IJK_AV_PIX_FMT__VIDEO_TOOLBOX`（`235`）

两条硬解路径殊途同归：**都不产出真正的像素数据拷贝，而是把"解码后缓冲区的引用/句柄"塞进 `AVFrame`**，靠特殊的 `frame->format` 值在渲染层分流处理，避免 CPU 拷贝。

## 6. 帧队列与渲染：三条路径汇合成同一个 pictq

无论哪条路径解出帧，最终都调用同一个入口 `ffp_queue_picture`（`ff_ffplay.c:4554`，内部是 `queue_picture`，`1503-1677`）：

1. `frame_queue_peek_writable(&is->pictq)` 拿一个可写的 `Frame` 槽位（`1605`）
2. 尺寸/格式变化时调用 `alloc_picture`（`1629`）：`SDL_Vout_CreateOverlay(vp->width, vp->height, frame_format, ffp->vout)`（`1474`）—— **`frame_format` 在这里首次按格式分流**
3. `SDL_VoutFillFrameYUVOverlay(vp->bmp, src_frame)`（`1650`）把帧数据/句柄填进 overlay
4. `frame_queue_push(&is->pictq)`（`1668`）发布到队列

### Overlay 创建按格式分流（Android）

`ijksdl_vout_android_nativewindow.c:96-104`：
```c
static SDL_VoutOverlay *func_create_overlay_l(int width, int height, int frame_format, SDL_Vout *vout)
{
    switch (frame_format) {
    case IJK_AV_PIX_FMT__ANDROID_MEDIACODEC:
        return SDL_VoutAMediaCodec_CreateOverlay(width, height, vout);
    default:
        return SDL_VoutFFmpeg_CreateOverlay(width, height, frame_format, vout);
    }
}
```

- 硬解帧走 `SDL_VoutAMediaCodec_CreateOverlay`，overlay 只持有 buffer 代理引用；渲染时 `AMediaCodec_releaseOutputBuffer(..., true)` 直接把 buffer 送显到 Surface，**零拷贝**
- 软解帧走 `SDL_VoutFFmpeg_CreateOverlay`，常规 YUV→纹理/`ANativeWindow` 拷贝路径

iOS 侧对称：`ijksdl_vout_overlay_videotoolbox.m` 的 `func_fill_frame` 把 `frame->opaque`（`CVPixelBufferRef`）直接 retain 进 overlay，后续由 `renderer_yuv420sp_vtb.m`（GLES + `CVOpenGLESTextureCache`）直接绑定为纹理采样渲染，同样零拷贝。

### 渲染线程消费

`video_refresh`（`ff_ffplay.c:1297`）→ `video_image_display2`（`863-914`）：
```c
SDL_VoutDisplayYUVOverlay(ffp->vout, vp->bmp);
```
（`902`）不管 `vp->bmp` 是哪种 overlay，走同一个 `SDL_Vout` 抽象接口分发到具体平台实现。

## 7. 全链路总图

```
demux (avformat_open_input/avformat_find_stream_info, ff_ffplay.c:3113/3160)
  → 识别 codecpar->codec_id == AV_CODEC_ID_HEVC
  → stream_component_open (ff_ffplay.c:2798)
       → avcodec_find_decoder + avcodec_open2 打开"软件" avctx（无条件执行, 2823/2867）
       → ffpipeline_open_video_decoder(ffp->pipeline, ffp) (2950/2956)
            ┌─ Android: ffpipeline_android.c:68 func_open_video_decoder
            │     if (mediacodec_hevc 等开关开) → ffpipenode_create_video_decoder_from_android_mediacodec
            │         → codec_id switch + ffp->mediacodec_hevc 开关检查 (vdec.c:1997)
            │         → ffpipeline_select_mediacodec_l → JNI → Java MediaCodecList 选型
            │         → 成功: MediaCodec 硬解node；失败: 回退 ffpipenode_create_video_decoder_from_ffplay
            │
            └─ iOS: ffpipeline_ios.c:38 func_open_video_decoder
                  if (ffp->videotoolbox) → ffpipenode_create_video_decoder_from_ios_videotoolbox
                      → codec_id 检查(H264/HEVC) + VTIsHardwareDecodeSupported(HEVC) (Sync.m:943)
                      → 成功: VideoToolbox硬解node；失败: 回退 ffpipenode_create_video_decoder_from_ffplay
       → decoder_start(video_thread) (2960)

video_thread (2329) → ffpipenode_run_sync(node_vdec)
  ┌─ 软解 node: ffp_video_thread → ffplay_video_thread(2166) → decoder_decode_frame(557,
  │     avcodec_send_packet/avcodec_receive_frame) → get_video_frame(1679) → queue_picture(1503)
  ├─ Android硬解 node: feed_input_buffer(喂裸NAL到MediaCodec) + drain_output_buffer(取输出buffer,
  │     amc_fill_frame设置 frame->format=IJK_AV_PIX_FMT__ANDROID_MEDIACODEC) → ffp_queue_picture
  └─ iOS硬解 node: VTDecompressionSessionDecodeFrame → QueuePicture(Sync.m:227,
        frame->format=IJK_AV_PIX_FMT__VIDEO_TOOLBOX) → ffp_queue_picture

queue_picture(1503) → alloc_picture → SDL_Vout_CreateOverlay(按frame_format分流创建overlay)
  → SDL_VoutFillFrameYUVOverlay → frame_queue_push(is->pictq)

video_refresh_thread → video_refresh(1297) → video_image_display2(863)
  → SDL_VoutDisplayYUVOverlay(ffp->vout, vp->bmp)
      ┌─ Android MediaCodec overlay: releaseOutputBuffer(render=true) 直接送显Surface(零拷贝)
      ├─ Android 软解 overlay: SDL_VoutFFmpeg_CreateOverlay，常规YUV拷贝/GLES渲染
      └─ iOS VideoToolbox overlay: CVPixelBuffer 绑定GLES纹理渲染(CVOpenGLESTextureCache, 零拷贝)
```

## 8. 反直觉的点（重点记忆）

1. **硬解开关是纯 option 驱动，不是按流的编码特征自动选择。** `ffp->mediacodec_hevc`/`ffp->videotoolbox` 默认都是 0，必须由 App 显式设置
2. **即使配置成硬解，FFmpeg 的软件 HEVC 解码器依然会被 `avcodec_open2` 打开**（`ff_ffplay.c:2867`），只是不会被拿去跑真正的逐帧解码——容易被忽略的资源开销
3. **Android 和 iOS 的"能力探测"层级不同**：iOS 在 native/OC 代码里有真实探测（`VTIsHardwareDecodeSupported`）；Android 的等效探测不在 `ijkmedia/` C 代码里，是通过 JNI 回调交给 Java 层的 `MediaCodecList` 遍历完成的
4. **HEVC 在 Android MediaCodec 侧没有 profile/level 白名单**（对比 H264 分支明确拒绝 Hi10P/4:4:4 等 profile），实际能否硬解完全取决于 Java 层挑出来的 codec 和系统底层实现，代码层面没有兜底过滤
5. **硬解帧在 `AVFrame` 里并不装像素数据**，而是把不透明句柄（MediaCodec buffer index 代理 / `CVPixelBufferRef`）塞进 `frame->opaque`，靠 `frame->format` 在渲染层分流，实现端到端零拷贝；软解帧则是标准 YUV 数据，走常规 overlay/GLES 拷贝路径

## 涉及的关键文件

- `ijkmedia/ijkplayer/ff_ffplay.c`
- `ijkmedia/ijkplayer/ff_ffplay_options.h`
- `ijkmedia/ijkplayer/ff_ffplay_def.h`
- `ijkmedia/ijkplayer/ff_ffpipeline.c` / `.h`
- `ijkmedia/ijkplayer/pipeline/ffpipenode_ffplay_vdec.c`
- `ijkmedia/ijkplayer/android/pipeline/ffpipeline_android.c` / `.h`
- `ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c`
- `ijkmedia/ijkplayer/android/ijkplayer_jni.c`
- `android/ijkplayer/ijkplayer-cmake/src/main/java/tv/danmaku/ijk/media/player/IjkMediaPlayer.java`（Java 侧 MediaCodecList 选型，非 `ijkmedia/` 但是链路必要一环）
- `ijkmedia/ijksdl/android/ijksdl_vout_android_nativewindow.c`
- `ijkmedia/ijksdl/android/ijksdl_vout_overlay_android_mediacodec.c`
- `ios/IJKMediaPlayer/IJKMediaPlayer/ijkmedia/ijkplayer/ios/pipeline/ffpipeline_ios.c`
- `ios/IJKMediaPlayer/IJKMediaPlayer/ijkmedia/ijkplayer/ios/pipeline/ffpipenode_ios_videotoolbox_vdec.m`
- `ios/IJKMediaPlayer/IJKMediaPlayer/ijkmedia/ijkplayer/ios/pipeline/IJKVideoToolBoxSync.m` / `IJKVideoToolBoxAsync.m`
- `ios/IJKMediaPlayer/IJKMediaPlayer/ijkmedia/ijksdl/ios/ijksdl_vout_overlay_videotoolbox.m`
- `ijkmedia/ijksdl/gles2/renderer_yuv420sp_vtb.m`
