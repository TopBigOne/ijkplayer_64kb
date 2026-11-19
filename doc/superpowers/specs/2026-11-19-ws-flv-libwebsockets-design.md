# ijkplayer 支持 WebSocket-FLV 拉流（libwebsockets + 自定义 AVIO）设计

日期：2026-11-19

## 1. 背景与目的

基于 ijkplayer 开发安防类 App（iOS + Android）。直播流通过 WebSocket 下发，WebSocket 消息的 payload 拼起来就是与 HTTP-FLV 相同的 FLV 字节流。播放器要能直接打开 `ws://` / `wss://` 地址播放，线上走 TLS（wss）。

用户明确要求：WebSocket 协议层使用经过大量实际项目验证的库（libwebsockets），不自研协议实现，以降低长时间挂流、弱网、异常断开时的风险。

## 2. 目标 / 非目标 / 反目标

**目标**
- App 层（Android/iOS）照常调用 `setDataSource("ws://..." / "wss://...")`，**Java/ObjC 接口不变**。C 层用 libwebsockets 建立连接、接收数据，通过 `avio_alloc_context` 把数据交给 FFmpeg，由 FFmpeg 自带的 flv demuxer 解析。
- iOS 和 Android 共用同一份 C 实现。Android 只支持 **arm64-v8a、armeabi-v7a**（用户决定）。
- 支持：URL query 原样透传（用于 token）、自定义握手 Header、User-Agent、Origin、Sec-WebSocket-Protocol 子协议、连接超时和读超时。
- 打断及时：DNS 解析、连接、握手、读取任一阶段调用 stop/release，ws 层都应在 ≤ 200ms 内返回。

**非目标（第一版不做）**
- 向服务器发送业务消息（开始播放指令、云台控制等）。结构上预留，以后需要时再加接口。
- permessage-deflate 压缩扩展。
- ws 层内部自动重连（断开后向上报错，由 App 决定是否重连，与现有 http-flv 一致）。
- 低延迟调优（缓冲策略、追帧），另行处理。

**反目标（出现即视为失败）**
- 退出播放时卡死、Android ANR，或残留线程、内存泄漏。
- 引入第二份 OpenSSL，与 FFmpeg 已链接的 OpenSSL 1.1.1w 发生符号冲突。
- 修改或重新编译 FFmpeg 预编译库。
- 影响现有 http/hls/rtsp/本地文件播放。

## 3. 约束（已核实的现状）

- FFmpeg 为 FFToolChain 预编译的 **4.0**（lavf 58.12），只有旧版 `AVFifoBuffer` 接口。
- **预编译 libavformat.a 不能新增 URLProtocol**：ijkplayer 的 `IJK_REGISTER_PROTOCOL` 只能用 memcpy 填进 7 个预留槽位（async、ijkio、ijkhttphook、ijktcphook、ijksegment、ijklongurl、ijkmediadatasource），见 `FFToolChain/patches/ffmpeg/0006-*.patch`，`url_protocols` 是静态表。这也是选择自定义 AVIO 的原因。
- libavformat.a 导出了可打断的 DNS 解析函数 `ijk_tcp_getaddrinfo_nonblock`（符号类型 T），但公共头文件里没有声明。
- OpenSSL **1.1.1w** 预编译库：
  - Android：`third-libs/openssl/openssl-${ARCH}`（四个 ABI 齐全）
  - iOS：`FFToolChain/build/product/ios/{universal,universal-simulator}/openssl`
- `/Users/dev/Documents/AV/openssl` 是 master（4.1.0-dev），**不使用**。
- **Android FFmpeg 目前只有 armv7a**（`third-libs/ijkffmpeg/ijkffmpeg-armv7a`），需先用 `android/install-ffmpeg.sh`（预编译标签 `ijkffmpeg-4.0-260402143959`）补齐 arm64。gradle 的 abiFilters（`ijkplayer-cmake/build.gradle`、`ijkplayer-example/build.gradle`）目前配了 4 个 ABI，要收窄为 arm64-v8a、armeabi-v7a。
- `android/ijkplayer/ijkplayer-cmake/src/main/cpp/third-libs` 是软链接，指向 `FFToolChain/build/product/android/universal`，三方库产物不进 git，由脚本生成或下载。
- Android 源码通过 `file(GLOB_RECURSE ... ${ijkmedia}/ijkplayer/*.c)` 收集，新增 .c 文件会自动编译。
- Java 层在 `IjkMediaPlayer.java:190` 调用 `libLoader.loadLibrary("ijkplayer")`。
- iOS 的 IJKMediaFramework 是**动态 framework**（`MACH_O_TYPE = mh_dylib`），最低 iOS 11.0（`build-framework.sh`）。
- 打开流程在 `ijkmedia/ijkplayer/ff_ffplay.c` 的 `read_thread()`：
  - `avformat_alloc_context()` 之后设置 `interrupt_callback = decode_interrupt_cb`（读取 `is->abort_request`）
  - 再调用 `avformat_open_input(&ic, is->filename, is->iformat, &ffp->format_opts)`
  - 关闭在 `stream_close()` 中，`SDL_WaitThread(read_tid)` 之后调用 `avformat_close_input(&is->ic)`
- Android 使用 CMake（`ijkplayer-cmake/src/main/cpp/CMakeLists.txt`），minSdk 21，三方库全部以 `.a` 静态链接进 `libijkplayer.so`，带 16KB 页对齐选项。iOS 使用 `ios/IJKMediaPlayer/IJKMediaPlayer.xcodeproj`，`ios/build-framework.sh` 产出 xcframework。
- NDK：工程未锁定版本，README 写 r27c；本机使用 `27.0.12077973` 编译 libwebsockets。

## 4. 方案比较（已决策）

**网络库**

| 方案 | 取舍 |
|---|---|
| **libwebsockets（选定）** | 握手、拆帧、ping/pong、close、TLS 全部由库完成，验证最充分 |
| wslay + 自写握手 | 握手仍是自研代码 |
| IXWebSocket | 自带线程，需桥接，退出时有竞态风险 |

**接入 FFmpeg 的方式**

| 方案 | 取舍 |
|---|---|
| **A. 自定义 AVIO（选定）** | 不碰 FFmpeg、不占槽位、FFmpeg 升级无影响；需改 `read_thread` 打开流程几十行 |
| B. 借用现有协议槽位 | 会牺牲一个已有功能，是隐性风险 |
| C. 重编 FFmpeg 增加槽位 | 所有架构都要重编，补丁需长期维护 |

用户选择 libwebsockets + 方案 A。

## 5. 设计

### 5.1 整体数据流

```
App: setDataSource("wss://host/live?token=...")      ← Java/ObjC 接口不变
  ↓
read_thread(): 判断 filename 以 ws:// 或 wss:// 开头
  ↓
ijkws_open(url, opts, interrupt_cb)
  DNS(可打断) → lws 连接 + 握手 → ESTABLISHED
  ↓
avio_alloc_context(buf, 32KB, read_packet = ijkws_read)
ic->pb = avio; ic->flags |= AVFMT_FLAG_CUSTOM_IO
iformat 未指定时强制使用 "flv"（跳过探测，加快起播）
  ↓
avformat_open_input(&ic, filename, flv, &format_opts)  → 之后流程与 http-flv 完全相同
  ↓
stream_close(): avformat_close_input 之后释放 avio 和 ijkws
```

### 5.2 libwebsockets 版本与编译

- 源码：`/Users/dev/Documents/AV/libwebsockets`，固定到 `v4.3-stable` 分支提交 **`bb1261254`**（在 v4.3.5 之后回补了 12 个修复，包括 client-http 退出路径和 close 处理），MIT 许可。
- **已编译完成**，脚本已就绪，可以重复执行：
  - **Android**：[tools/build-libwebsockets-android.sh](../../../tools/build-libwebsockets-android.sh)，产出**独立动态库 `libwebsockets.so`**（用户要求单独的 so），默认编 arm64、armv7a。
    - OpenSSL 1.1.1w 静态链接进 so，并用 `-Wl,--exclude-libs,ALL` 隐藏 OpenSSL 符号，避免和 libijkplayer.so 里的那份 OpenSSL 互相串用。已验证导出的 OpenSSL 符号为 0 个，lws API 导出 317 个。
    - SONAME 为 `libwebsockets.so`（不带版本号），16KB 页对齐，只依赖 libc、libm、libdl；strip 后 arm64 约 2.8MB，armv7a 约 2.2MB。
  - **iOS**：[tools/build-libwebsockets-ios.sh](../../../tools/build-libwebsockets-ios.sh)，产出**静态库 `libwebsockets.a`**：真机 arm64（iOS 11.0）一份，模拟器 arm64 + x86_64 一份。
    - 链接进 IJKMediaFramework（本身是动态 framework），和 FFmpeg 共用 OpenSSL，App 不需要额外 embed 任何东西。
    - 已对三个 slice 做过链接测试（lws + OpenSSL），无未解析符号。
- 编译工具：NDK `27.0.12077973`，ninja 用 Android SDK 自带的（`sdk/cmake/3.31.6/bin/ninja`；depot_tools 里的 ninja 不能用）。
- 关键 CMake 选项：
    - `LWS_WITH_SSL=ON`，`LWS_OPENSSL_INCLUDE_DIRS` / `LWS_OPENSSL_LIBRARIES` 指向对应架构的 OpenSSL 1.1.1w
    - `LWS_WITHOUT_SERVER=ON`，`LWS_WITHOUT_TESTAPPS=ON`，`LWS_WITH_MINIMAL_EXAMPLES=OFF`
    - Android：`LWS_WITH_SHARED=ON`、`LWS_WITH_STATIC=OFF`、`CMAKE_PLATFORM_NO_VERSIONED_SONAME=ON`；iOS：`LWS_WITH_SHARED=OFF`、`LWS_WITH_STATIC=ON`
    - `LWS_WITH_HTTP2=OFF`，`LWS_WITHOUT_EXTENSIONS=ON`
    - `LWS_WITH_LIBUV/LIBEV/LIBEVENT/GLIB=OFF`（使用默认 poll 事件循环）
    - `LWS_WITH_SYS_ASYNC_DNS=OFF`（DNS 在 lws 外部解析，见 5.3）
    - `LWS_IPV6=ON`
- 产物位置：
  - Android：`third-libs/libwebsockets/libwebsockets-{arm64,armv7a}/{include,lib/libwebsockets.so}`（实际在 `FFToolChain/build/product/android/universal/` 下）
  - iOS：`FFToolChain/build/product/ios/{universal,universal-simulator}/libwebsockets/{include,lib/libwebsockets.a}`
- 编译中间目录 `android/build-libwebsockets/`、`ios/build-libwebsockets/` 加入 `.gitignore`。
- libwebsockets 本地仓库已新建分支 `v4.3-stable`，指向 `bb1261254`；脚本会校验 HEAD 是否为这个提交。

### 5.3 ijkws 模块

新文件 `ijkmedia/ijkplayer/ijkavformat/ijkws.c` / `ijkws.h`，对 ff_ffplay.c 只暴露：

```c
typedef struct IjkWsContext IjkWsContext;
int  ijkws_open(IjkWsContext **out, const char *url, AVDictionary **opts,
                const AVIOInterruptCB *int_cb);   // 成功返回 0；失败返回 AVERROR
int  ijkws_read(void *opaque, uint8_t *buf, int size);  // 签名与 avio read_packet 一致
void ijkws_close(IjkWsContext **ctx);
```

每个播放实例独占一个 `lws_context`，实例之间完全隔离，不需要全局状态和锁。全部操作都在 read_thread 中同步驱动，**不额外起线程**。

**ijkws_open**
1. `av_url_split` 解析 scheme、host、port（默认 ws 80 / wss 443）、path（含 query，原样透传）。
2. **DNS（可打断）**：调用 libavformat 导出的 `ijk_tcp_getaddrinfo_nonblock`，传入 `int_cb` 和超时（ijkws.c 内自行 extern 声明，签名以 FFToolChain 的 ffmpeg-n4.0 补丁为准）。得到 IP 字符串后交给 lws：`connect_info.address = IP`，`connect_info.host = 原始域名`（用于 Host 头）。wss 的 SNI 必须是原始域名：已确认 lws v4.3 的 SNI 取自 `host`（`lib/tls/openssl/openssl-client.c:253`，`_WSI_TOKEN_CLIENT_HOST`），不需要额外处理。
3. 创建 lws_context（不监听端口，只注册一个 client protocol），调用 `lws_client_connect_via_info`。wss 设置 `LCCSCF_USE_SSL`，证书策略见 5.5。
4. 握手 Header：Origin、子协议通过 connect_info 设置；`ws_headers`、`user_agent` 在 `LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER` 回调中追加。
5. 循环 service（见"驱动与打断"），直到出现以下之一：
   - `LWS_CALLBACK_CLIENT_ESTABLISHED`：成功
   - `LWS_CALLBACK_CLIENT_CONNECTION_ERROR`：如果拿得到 HTTP 状态码，就映射为对应的 FFmpeg 错误码：401 → `AVERROR_HTTP_UNAUTHORIZED`，403 → `AVERROR_HTTP_FORBIDDEN`，404 → `AVERROR_HTTP_NOT_FOUND`，其他 4xx → `AVERROR_HTTP_OTHER_4XX`，5xx → `AVERROR_HTTP_SERVER_ERROR`；拿不到状态码时返回 `AVERROR(EIO)`。这样 App 可以把 token 过期和网络错误区分开。失败原因同时用 av_log 输出。
   - 超时：返回 `AVERROR(ETIMEDOUT)`
   - 被打断：返回 `AVERROR_EXIT`
6. 从 `opts` 中删除所有已消费的 `ws_*` 选项，避免 avformat_open_input 打印 "Option not found"。

**接收**
- `LWS_CALLBACK_CLIENT_RECEIVE`：把 payload 追加到自写的字节环形缓冲 `ijkws_buf`（不依赖 FFmpeg 的 FIFO API，可以直接在 macOS 上用 FFmpeg 4.4 做主机测试）。
- 断线判定：收到过对端 close 帧（`LWS_CALLBACK_WS_PEER_INITIATED_CLOSE`）的，按 EOF 处理；没有 close 帧就断开的，按 `AVERROR(EIO)` 处理。二进制帧和文本帧都接受，分片消息按到达顺序拼接；FLV 是连续字节流，不关心消息边界。
- 缓冲上限 4MB，超过时 `lws_rx_flow_control(wsi, 0)` 暂停接收，读到低于 1MB 再恢复。由于 service 只在 read 中驱动，正常情况不会触发，这里只是保护。
- `LWS_CALLBACK_CLIENT_CLOSED` / `CLIENT_CONNECTION_ERROR`：记录状态、关闭码和原因，用 av_log 输出。
- ping/pong 由 lws 自动处理。

**ijkws_read**
```
loop:
  fifo 有数据      → 取出 min(size, 可用) 字节，返回字节数
  已出错           → AVERROR(EIO)
  已关闭           → AVERROR_EOF
  int_cb 返回非 0  → AVERROR_EXIT
  距上次收到数据超过 ws_timeout → AVERROR(ETIMEDOUT)
  service 一轮
```

**驱动与打断**：lws v4.3 的 `lws_service(ctx, timeout_ms)` 超时参数实际无效（`lib/plat/unix/unix-service.c:99-103`：非负值一律按 23 天处理，负值则变成忙轮询）。因此每个 lws_context 注册一个 **100ms 周期的 `lws_sul` 定时器**，到点唤醒 poll，回到上面的检查点。这样打断延迟 ≤ 200ms。

**ijkws_close（优雅关闭，有时间上限）**
1. 如果连接仍然 ESTABLISHED：调用 `lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, ...)`，并请求一次可写回调，回调中返回 -1，由 lws 发送 close 帧。
2. 继续 service，最多 100ms，或直到 `CLIENT_CLOSED`。
3. `lws_context_destroy`（只断开 TCP、释放 SSL，不会发 close 帧，所以需要上面两步），然后释放 fifo 和选项字符串。

目的：让服务端及时回收会话和通道配额。安防平台常有并发路数限制，频繁切换通道时这一点很重要。同时把退出耗时控制在 ≤ 100ms。

**发送（预留，不实现）**：内部保留 wsi 指针和发送队列的位置，以后通过 `LWS_CALLBACK_CLIENT_WRITEABLE` + `lws_write` 实现，并在 ijkplayer 层加 API。

### 5.4 ff_ffplay.c 改动

- `VideoState`（或 `FFPlayer`）新增 `IjkWsContext *ws; AVIOContext *ws_avio;`。
- `read_thread()` 中，在 `avformat_open_input` 之前加：
  ```c
  if (av_stristart(is->filename, "ws://", NULL) || av_stristart(is->filename, "wss://", NULL)) {
      ijkws_open(&is->ws, is->filename, &ffp->format_opts, &ic->interrupt_callback);  // 失败 → goto fail
      buf = av_malloc(32768);
      is->ws_avio = avio_alloc_context(buf, 32768, 0, is->ws, ijkws_read, NULL, NULL);
      is->ws_avio->seekable = 0;
      ic->pb = is->ws_avio;
      ic->flags |= AVFMT_FLAG_CUSTOM_IO;
      if (!is->iformat) is->iformat = av_find_input_format("flv");
  }
  ```
- 释放：设置了 `AVFMT_FLAG_CUSTOM_IO` 后，`avformat_close_input` 不会释放 pb。所以在 `stream_close()` 的 `avformat_close_input(&is->ic)` 之后，以及 `read_thread` 的 `fail:` 路径（ic 未交给 is 时）都要执行：
  1. `av_freep(&is->ws_avio->buffer)`
  2. `avio_context_free(&is->ws_avio)`
  3. `ijkws_close(&is->ws)`

  提取成一个 `ws_release(is)` 函数，保证各条路径只释放一次。
- FFmpeg 4.0 中 `avformat_open_input` 失败时会释放 ic，但不会释放自定义 pb，fail 路径要覆盖这种情况。
- 其他流程（读包、缓冲、统计、错误上报）不变。

### 5.5 选项（App 通过现有 `setOption(FORMAT, key, value)` 传入）

ijkws_open 从 `ffp->format_opts` 中读取以下选项并删除：

| 名称 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `ws_headers` | string | 空 | 追加的握手 Header，`\r\n` 分隔 |
| `user_agent` | string | 空 | User-Agent；与 http 共用这个选项名，读取后**不删除**，让 http 场景继续可用（ws 场景下 FFmpeg 会忽略） |
| `ws_origin` | string | 空 | Origin |
| `ws_protocol` | string | 空 | Sec-WebSocket-Protocol |
| `ws_timeout` | int（毫秒） | 10000 | 连接+握手超时；同时作为读取期间无数据的超时 |
| `ws_tls_verify` | int | 0 | 1 = 校验服务器证书和主机名 |
| `ws_ca_file` | string | 空 | CA bundle 路径；`ws_tls_verify=1` 时必须提供，因为 iOS/Android 都没有 OpenSSL 可用的系统 CA 文件，需要 App 打包一份 |

### 5.6 TLS 证书校验

工程现有的 https 使用 FFmpeg OpenSSL 默认的 `tls_verify=0`，不校验证书。第一版默认与之保持一致：**不校验**（`LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK | LCCSCF_ALLOW_EXPIRED`），以兼容安防场景常见的自签名证书。App 可以通过 `ws_tls_verify=1` + `ws_ca_file` 开启严格校验。

### 5.7 日志

首次 open 时设置一次 `lws_set_log_level(LLL_ERR | LLL_WARN, cb)`，cb 转发到 `av_log`。连接失败原因、HTTP 状态码、关闭码和原因都以 ERROR/WARNING 级别输出，便于线上排查。

### 5.8 工程集成

**Android**
- `CMakeLists.txt`：
  - `add_library(websockets-lib SHARED IMPORTED)`，`IMPORTED_LOCATION` 指向 `third-libs/libwebsockets/libwebsockets-${ARCH}/lib/libwebsockets.so`，include 目录加入 `third-libs/libwebsockets/libwebsockets-${ARCH}/include`，主目标链接 `websockets-lib`。
  - `ijkws.c` 放在 `ijkmedia/ijkplayer/ijkavformat/` 下，会被 GLOB 自动收集。
  - `ijkws.c` 调用的 `ijk_tcp_getaddrinfo_nonblock` 在 libavformat.a 中，属于 libijkplayer.so 内部链接，不涉及 libwebsockets.so。
- **打包**：确认 AAR 里包含 `jni/<abi>/libwebsockets.so`。AGP 4.0+ 会自动打包 CMake 链接的 IMPORTED 动态库；如果没打包进去，就改为放到 `src/main/jniLibs/<abi>/`。
- **加载顺序**：minSdk 21，Android 5.x（API 21/22）的动态链接器不会到 App 的 lib 目录里自动查找依赖的 so。所以在 `IjkMediaPlayer.java:190` 加载 ijkplayer **之前**先调用 `libLoader.loadLibrary("websockets")`。
- **ABI**：两个 build.gradle 的 abiFilters 收窄为 `"arm64-v8a","armeabi-v7a"`。
- **前置步骤**：补齐 arm64 的 FFmpeg 预编译库。

**iOS**（`IJKMediaPlayer.xcodeproj`）
- 加入 `ijkws.c` / `ijkws.h`。
- Header Search Paths、Library Search Paths 按 sdk 区分（与现有 openssl 的配置方式相同），增加 libwebsockets 的真机路径和模拟器路径；Other Linker Flags 增加 `-lwebsockets`。
- 验证：`build-framework.sh` 产出的 framework 中包含 `lws_create_context` 等符号（`nm` 检查）。

## 6. 验证

libwebsockets 的协议正确性由上游保证（上游持续跑 Autobahn 一致性测试），这里重点验证**集成层**。

1. **本地测试服务器**（放 `tools/ws-flv-test-server/`）：读取一个 FLV 文件，按真实码率推送。覆盖：
   - ws、wss（自签名证书）
   - FLV 被任意切分到多个消息中、分片消息
   - 服务器发 ping
   - 服务器正常 close
   - 服务器直接断开 TCP
   - 握手返回 401/404
   - 连接后不推数据（读超时）
   - 域名解析很慢或失败
   - 用 `ws_tls_verify=1` + `ws_ca_file` 验证证书校验通过和失败两种情况
2. **打断测试**：在 DNS、连接、握手、读取各阶段调用 stop/release，测量返回时间（≤ 200ms），无卡死、无泄漏（Android 用 ASan，iOS 用 Instruments Leaks）；同时确认服务器收到了 close 帧。
3. **真实服务器、真机**（iOS 和 Android 各自）：
   - 连续挂流 24 小时：无崩溃，内存平稳
   - 弱网：丢包、高延迟、断网后恢复
   - 频繁切换通道：每秒一次，持续 30 分钟，服务端会话数不累积
4. **回归**：http-flv、hls、rtsp、本地文件播放不受影响。

## 7. 本设计中由 Claude 做出的决策（请确认）

- libwebsockets 固定 `v4.3-stable@bb1261254`，而非 v4.3.5 tag。
- 使用工程现有 OpenSSL 1.1.1w，不使用本地 openssl master。
- NDK 用 `27.0.12077973`。
- Android 单独 so 中静态带一份隐藏符号的 OpenSSL；iOS 用静态库链接进 IJKMediaFramework（用户只对 Android 要求单独 so）。
- 第一版只收流，不支持上行发消息；token 通过 URL query 或 `ws_headers` 传递。
- wss 默认不校验证书（与现有 https 一致），可通过选项开启。
- ws 层不自动重连。
- 未指定 iformat 时强制使用 flv，跳过探测以加快起播。
- DNS 复用 libavformat 导出的 `ijk_tcp_getaddrinfo_nonblock`，以保证可打断。
- 退出时优雅关闭，最多等 100ms。

## 8. 留给实现者决定的

- 测试服务器用 Python 还是 Node。
- `ws` / `ws_avio` 字段放在 `VideoState` 还是 `FFPlayer`。
