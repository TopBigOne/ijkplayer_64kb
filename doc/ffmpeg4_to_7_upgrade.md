# ijkplayer 中 FFmpeg 从 4.0 升级到 7.1

本文记录 Android 端把 ijkplayer 依赖的 FFmpeg 从 bilibili 的 `ff4.0--ijk0.8.8` 升级到官方 FFmpeg 7.1 的方案、改动、验证结果和遗留事项。代码在 `ffmpeg7-migration` 分支，iOS 尚未迁移。

## 1. 结论

| 项 | 升级前 | 升级后 |
|---|---|---|
| FFmpeg 源码 | bilibili/FFmpeg `ff4.0--ijk0.8.8--20210426--001`（ijk 改动直接提交在 fork 里） | 官方 FFmpeg `n7.1.3` + `FFToolChain/patches/ffmpeg-n7.1.1`（32 个补丁） |
| 工具链目标 | `ijkffmpeg` | `ijkffmpeg7` |
| 裁剪配置 | `ijk-ffmpeg-config/module-lite.sh` | `ijk-ffmpeg-config/module-lite-ff7.sh` |
| 三方依赖 | OpenSSL 1.1.1w | OpenSSL 1.1.1w（不变） |
| 汇编 | 关闭 | arm64 / armv7a 开启 NEON |
| arm64 release `libijkplayer.so` | 7.9 MB | 9.0 MB |
| 真机回归 | — | 与 4.0 对照无差异（见第 7 节） |

## 2. 方案：官方 FFmpeg + ijk 补丁

ijkmedia 依赖 bilibili fork 中的私有接口，官方 FFmpeg 没有：

- `av_application_*`：HTTP / TCP / IO 事件回调、async 统计
- `av_dict_set_intptr` / `av_dict_get_intptr`：在 options 里传指针
- `ijkav_register_*`：注册 ijk 自定义协议与 demuxer（ijkio、ijkhttphook、ijksegment、ijktcphook、ijkmediadatasource、ijklivehook、ijklas）

`patches/ffmpeg-n7.1.1` 中的 `restore-ijk-*` 补丁把它们加回官方 FFmpeg。自定义协议与 demuxer 采用**占位 + 运行时替换**：

1. 补丁在 FFmpeg 内注册同名空壳（`ff_ijkio_protocol`、`ff_ijklas_demuxer` 等，`open` 直接返回失败）；
2. 补丁导出 `ijkav_register_xxx_protocol/demuxer(ptr, size)`，校验结构体大小后用 `memcpy` 把真实实现覆盖到空壳上；
3. 真实实现仍在 `ijkmedia/ijkplayer/ijkavformat/`，`ijkav_register_all()` 启动时调用上述函数。

因此 FFmpeg 侧只维护一层很薄的挂钩，ijk 逻辑留在本仓库。升级到更高版本 FFmpeg 时，主要工作是把这组补丁 rebase 到新版本。

> 补丁不包含 `async` 与 `ijklongurl` 的注册入口，处理方式见第 5 节。

## 3. 构建

### 3.1 工具链（FFToolChain 子模块，`ijk-custom` 分支）

| 文件 | 作用 |
|---|---|
| `configs/libs/ijkffmpeg7.sh` | 源码与版本：官方 `n7.1.3`，补丁目录 `ffmpeg-n7.1.1`，产物名 `ijkffmpeg7-<arch>` |
| `configs/ijk-ffmpeg-config/module-lite-ff7.sh` | 由 `module-lite.sh` 移植的裁剪配置 |
| `do-compile/android/ijkffmpeg7.sh` | Android 编译脚本：lite 配置 + 只探测 OpenSSL |

`module-lite-ff7.sh` 相对 4.0 配置的差异：

- 去掉 FFmpeg 5 已删除的 `--disable-avresample`；
- 显式启用 `ijklivehook`、`ijklas` demuxer。FFmpeg 7 的 demuxer 列表在编译期固定，`--disable-demuxers` 后不打开，运行时就找不到；
- 启用 `mp4` 与 `mov` muxer（视频流转存 mp4），并保留其依赖的 `vp9_superframe` bsf（4.0 配置里禁用了它）。

`do-compile/android/ijkffmpeg7.sh` 不再使用 fsplayer 的 `module-full.sh` + `auto-detect-third-libs.sh`，因此：

- 不链接 dav1d、bluray、dvdread、smb2、opus、xml2 等三方库；
- `--disable-jni --disable-mediacodec`：ijk 有自己的 MediaCodec 管线，不需要 FFmpeg 自带的 MediaCodec 解码器，也就不依赖 `libmediandk.so`；
- 保持 `--disable-gpl --disable-nonfree`。FFmpeg 只在 GPL 构建时才要求 OpenSSL 1.1 配合 `--enable-nonfree`。

### 3.2 编译命令

```bash
cd android
./install-ffmpeg.sh
```

脚本依次执行：

1. `main.sh install -l 'soundtouch yuv ijkffmpeg'`：下载 soundtouch、yuv 预编译包；OpenSSL 1.1.1w 只打包在 ijkffmpeg 4.0 的 zip 里，借此装上；
2. `main.sh init -p android -l ijkffmpeg7 -a "armv7a arm64"`：clone 官方 FFmpeg 并打补丁；
3. `main.sh compile -p android -l ijkffmpeg7 -a "armv7a arm64" -c rebuild`。

FFmpeg 7 没有上游预编译包，原 `--prebuilt` 选项已移除。首次 init 约 3 分钟（clone 官方仓库），两个 ABI 编译约 1 分钟。产物在 `FFToolChain/build/product/android/universal/ijkffmpeg7/ijkffmpeg7-{arm64,armv7a}/`，CMake 通过 `third-libs` 软链接读取。

快速校验：

```bash
P=FFToolChain/build/product/android/universal/ijkffmpeg7/ijkffmpeg7-arm64/include/libffmpeg/config_components.h
grep -E "CONFIG_(RTMPS_PROTOCOL|MP4_MUXER|IJKLAS_DEMUXER|IJKIO_PROTOCOL) " $P   # 均应为 1
```

### 3.3 CMake（`ijkplayer-cmake/src/main/cpp/CMakeLists.txt`）

- 链接 `third-libs/ijkffmpeg7/ijkffmpeg7-${ARCH}`；
- **OpenSSL 必须仍是 1.1.1w**。`ijkws.c` 会直接调用 OpenSSL API（`SSL_get0_param`、`X509_VERIFY_PARAM_set1_ip_asc` 等）去操作 `libijkwebsockets.so` 内部的 `SSL` 对象，两边版本不一致会在运行时写坏内存，编译期发现不了；
- `-Wl,-Bsymbolic`：NEON 汇编以 PC 相对寻址引用 `ff_tx_tab_*` 等全局表，链接进 .so 时 lld 要求这些符号不可抢占。FFmpeg 自身构建动态库时也这样做；
- `-Werror=incompatible-pointer-types`（`discards-qualifiers` 仍为警告）：FFmpeg 7 多个 API 的指针参数类型变了（`int*` → `size_t*`、`AVIOContext*` → `FFIOContext*`），传错类型只报警告，运行时却会越界写。迁移过程中靠它找出了 6 处问题。

## 4. 代码适配清单

### 4.1 ijkavformat

| 变化 | 处理 | 文件 |
|---|---|---|
| `application.h` 从 `libavutil/` 移到 `libavformat/`（补丁位置变化） | 改 include | ff_ffinc.h、ijkasync / ijkurlhook / ijksegment / ijkiourlhook / ijklivehook.c |
| `AVFifoBuffer` 删除 | 改用 `AVFifo`（`av_fifo_alloc2` / `av_fifo_peek` / `av_fifo_write_from_cb` 等），对齐上游 `libavformat/async.c` | ijkasync.c |
| demuxer 回调移到内部结构 `FFInputFormat` | 定义改为 `FFInputFormat`，公开字段放进 `.p`；probe 参数改为 `const AVProbeData *` | ijklas.c、ijklivehook.c |
| `ffio_init_context` 要求 `FFIOContext*` | 内嵌 IO 改为 `FFIOContext`，对外用 `.pub` | ijklas.c |
| `av_register_all` / `av_register_input_format` / `av_iformat_next` 删除 | 去掉；demuxer 注册按 `FFInputFormat` 大小 | allformats.c |
| `AVFormatContext.filename` 删除 | 改用 `url` | ijklivehook.c、ff_ffplay.c |
| FFmpeg 头不再间接引入 `string.h`、`inttypes.h`、`error.h` | 补齐 include | ijkiourlhook.c、ijkiocache.c |

### 4.2 ff_ffplay.c

| 变化 | 处理 |
|---|---|
| 声道布局 API（`channels` / `channel_layout` 字段、`av_get_default_channel_layout`、`swr_alloc_set_opts`）删除 | 内部仍用"声道掩码 + 声道数"（AudioParams、audio_open、SDL aout 均不变），与 FFmpeg 交互处经 `ffp_ch_layout_to_mask` / `ffp_default_ch_mask` 转换；改用 `swr_alloc_set_opts2` |
| `avcodec_encode_video2` 删除（截图 PNG 编码） | `avcodec_send_frame` + `avcodec_receive_packet` |
| `av_codec_{get,set}_pkt_timebase / lowres / max_lowres` 删除 | 直接读写字段 |
| `av_register_all`、`avcodec_register_all`、`av_lockmgr_register` 删除 | 去掉（FFmpeg 4.0 起内部自行加锁） |
| `avdevice_register_all` | 不再调用（不链接 libavdevice） |
| `iformat->read_seek` 移到内部结构 | `ffifmt(iformat)->read_seek`（include `libavformat/demux.h`） |
| `AVClass.child_class_next` 删除 | 改为 `child_class_iterate` |
| `pts_correction_num_faulty_*` 私有化 | 删除该调试统计 |

### 4.3 MediaCodec 管线（ffpipenode_android_mediacodec_vdec.c）

| 变化 | 处理 |
|---|---|
| `avcodec_decode_video2` 删除（分辨率切换探测） | 新增 `amc_probe_decode()`：`send_packet` 后必须 `receive_frame` 一次才会真正解码，`EAGAIN` 不算错 |
| `av_packet_get_side_data` 的 size 参数改为 `size_t*` | 变量改为 `size_t` |
| `av_packet_split_side_data` 删除 | 去掉（side data 不再合并进包数据） |
| `av_frame_get_pkt_pos` 删除 | `frame->pkt_pos` |
| `AVBitStreamFilterContext` 删除 | 移入默认关闭的 `AMC_USE_AVBITSTREAM_FILTER` 分支 |

### 4.4 其他

- `ijkmeta.c`：`codecpar->channels` / `channel_layout` 改为 `ch_layout`，上报给 Java 层的仍是旧声道掩码；HDR 静态元数据由 `av_stream_get_side_data` 改为 `av_packet_side_data_get(codecpar->coded_side_data, ...)`。
- `ijk config.h` 中 `CONFIG_AVFILTER` 为 0，`ff_ffplay.c` 里 avfilter 分支的代码**没有迁移**。以后要启用 avfilter，这些分支需要一起改。

## 5. 行为变化与取舍

| 项 | 说明 |
|---|---|
| `async:` 协议 | 补丁未预留 ijk 版的注册入口，改用 FFmpeg 官方 async。ijk 版额外上报的 `FFP_PROP_INT64_ASYNC_STATISTIC_*` 将没有数据（App 未使用 `async:` 前缀）。`ijkasync.c` 已迁移但未注册，保留备用 |
| `ijklongurl` | 删除长 URL 改写。FFmpeg 4.1 起 URL 为动态长度，不再有 1024 字节限制 |
| mp4 / mov muxer | 启用。4.0 时也启用，FFmpeg 7 中需额外保留 `vp9_superframe` bsf |
| FFmpeg 自带 MediaCodec | 未启用，硬解仍走 ijk 自己的管线 |

## 6. 迁移中顺带修复的问题

`-Werror=incompatible-pointer-types` 找出的问题，其中部分在 4.0 下就存在：

| 位置 | 问题 | 是否 4.0 就存在 |
|---|---|---|
| mediacodec_vdec.c | 分辨率探测用的 `AVFrame` 只 `unref` 不 `free`，出错路径也未释放，每次切换泄漏一帧 | 是 |
| ijkdict.c | `ijk_av_dict_get_intptr` 误用 FFmpeg 的 `av_dict_get` 查 ijk 字典，只因结构体布局相同才碰巧可用 | 是 |
| ijklas.c | `ffurl_open_whitelist` 的 parent 误传 `AVFormatContext*`，选项拷贝从未生效（改为 NULL，行为不变） | 是 |
| ijklas.c | `ffio_init_context` 需要 `FFIOContext*` | 否，FFmpeg 7 引入 |
| mediacodec_vdec.c、ijkmeta.c | side data 的 size 用 `int*` 接 `size_t*`，64 位上越界写 | 否，FFmpeg 7 引入 |

## 7. 验证

设备：Galaxy A54（SM-A5460，Android 14）。用同一套脚本、同一批源，分别跑 4.0（master）和 7.1（本分支）做对照：

| 类别 | 用例 | 结果 |
|---|---|---|
| 点播 | MP4 H264 / HEVC / 带音频，HLS（TS、fMP4），软解 + MediaCodec 硬解 | 两版一致，均通过 |
| 直播 | rtmp、rtmps、http-flv、ws、wss，软解 + 硬解 | 两版一致，均通过 |
| seek | 软解 + 硬解，seek 完成且之后重新出画面 | 两版一致，均通过 |
| ijkws | 握手失败（502 → `AVERROR_HTTP_SERVER_ERROR`）、连接被拒 / DNS 失败 / TLS 失败（`-5`） | 两版一致，与文档相符 |
| ijkws | 频繁切换 20 次：无崩溃 / ANR，服务端连接全部释放 | 两版一致 |
| ijkws | 播放中服务端断开（无 close 帧）：上报 `-5`，不崩溃 | 两版一致 |
| 长时间 | ws 硬解 5 分钟、wss 硬解 3 分钟：无卡顿、无报错，内存增长与 4.0 相当 | 两版一致 |
| 功能 | SEI 端到端延迟测量、ijk HTTP 事件回调 | 正常 |

两个版本表现相同、与迁移无关的现象：

- 开启"直播低延迟"（`probesize=64K`）时，部分 HLS 源的 AAC 参数探测不出来，没有声音；
- Apple 多码率 HLS 示例（尤其 HEVC 版）起播时要逐个打开所有码率档，起播很慢。

未覆盖：HEVC MediaCodec 硬解与 HEVC 分辨率切换（example 没有暴露 `mediacodec-hevc` 开关）。

## 8. 遗留事项

1. **iOS 未迁移**：ijkmedia 已改为 FFmpeg 7 API，本分支上 iOS 无法编译。需迁移 VideoToolbox 管线与 iOS 的 FFmpeg 编译（`do-compile/apple/` 对应脚本）。
2. **FFmpeg 8 前需替换的弃用接口**：`AVFrame.pkt_pos`、`av_init_packet`（7.1 中仅弃用告警）。
3. **HEVC 硬解回归**：给 example 加 `mediacodec-hevc` 开关后补测。
4. **avfilter 分支**：见 4.4。
5. **ijk async 统计**：如需 `FFP_PROP_INT64_ASYNC_STATISTIC_*`，要在补丁里给 async 加占位与注册入口。

## 9. 常见问题

- **configure 日志里出现 `'stdbit.h' file not found`**：这是 configure 探测 C23 特性的正常输出，不是错误。真正的错误看 `make: ***` 那一行。
- **`make install` 报 `/usr/local/share/ffmpeg: Permission denied`**：编译脚本缺 `--prefix`，会装到系统目录。`ijkffmpeg7.sh` 已设 `--prefix=$MR_BUILD_PREFIX`。
- **链接报 `relocation R_AARCH64_ADR_PREL_PG_HI21 cannot be used against symbol 'ff_tx_tab_*'`**：缺 `-Wl,-Bsymbolic`。
- **链接报 `undefined symbol: avdevice_register_all`**：FFmpeg 配置里 `CONFIG_AVDEVICE=1` 但未链接 libavdevice。播放器不需要它，已去掉调用。
- **运行时 ijk demuxer 找不到**：检查 `config_components.h` 中 `CONFIG_IJKLAS_DEMUXER` / `CONFIG_IJKLIVEHOOK_DEMUXER` 是否为 1。
