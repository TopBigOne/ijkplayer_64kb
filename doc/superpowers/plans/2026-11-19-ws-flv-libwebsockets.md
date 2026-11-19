# ijkplayer WebSocket-FLV 拉流 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal：** App 调用 `setDataSource("ws://..." / "wss://...")` 就能播放 WebSocket 下发的 FLV 直播流，iOS 和 Android 都支持。

**Architecture：** 新增 C 模块 `ijkws`：用 libwebsockets 在播放器读线程里同步驱动连接和收数据，接收到的数据存进自写的环形缓冲。`ff_ffplay.c` 的 `read_thread` 识别 ws/wss 地址后，用 `avio_alloc_context(read_packet = ijkws_read)` 生成自定义 IO 交给 FFmpeg，强制使用 flv demuxer。不改动 FFmpeg。

**Tech Stack：** C99、libwebsockets v4.3-stable@bb1261254、FFmpeg 4.0（ijk 定制版，预编译）、OpenSSL 1.1.1w、Android CMake/NDK 27.0.12077973、Xcode 16.4。主机测试使用 macOS + `/usr/local/ffmpeg/4.4` + Homebrew openssl@3 + Python 3 标准库。

**Spec：** `doc/superpowers/specs/2026-11-19-ws-flv-libwebsockets-design.md`

## Global Constraints

- 不修改、不重编 FFmpeg 预编译库；不新增 URLProtocol。
- 进程内不得出现两份**互相可见**的 OpenSSL：Android 的 `libwebsockets.so` 中 OpenSSL 符号必须保持隐藏（导出数 = 0）。
- Android 只支持 `arm64-v8a`、`armeabi-v7a`，minSdk 21；iOS 最低 11.0。
- libwebsockets 固定 `bb1261254`，编译脚本已就绪：`tools/build-libwebsockets-android.sh`、`tools/build-libwebsockets-ios.sh`。
- ijkws 全程在调用方线程同步执行，**不创建线程**（DNS 解析由 `ijk_tcp_getaddrinfo_nonblock` 内部处理，不在此限）。
- 打断延迟 ≤ 200ms；`ijkws_close` 耗时 ≤ 150ms（包括最多 100ms 的优雅关闭）。
- 接收缓冲上限 4MB，超过即暂停接收，降到 1MB 以下再恢复。
- 选项：`ws_headers`、`user_agent`（只读不删）、`ws_origin`、`ws_protocol`、`ws_timeout`（毫秒，默认 10000）、`ws_tls_verify`（默认 0）、`ws_ca_file`。
- 不新装工具：测试服务器只用 Python 3 标准库；需要新依赖时先问用户。
- 只在本地 commit，不建 PR。

## Review Focus

1. **读取时网络突然断开**（Wi-Fi 关闭，没有 FIN/RST）：应在 `ws_timeout` 内返回 `AVERROR(ETIMEDOUT)`，不能永远挂住。归 Task 3 `test_silent_server_read_timeout`，真机部分归 Task 7。
2. **token 过期，握手返回 401/403**：App 需要能和普通网络错误区分开。open 返回 `AVERROR_HTTP_UNAUTHORIZED` / `AVERROR_HTTP_FORBIDDEN`，404 返回 `AVERROR_HTTP_NOT_FOUND`。归 Task 3 `test_handshake_http_status_mapped`。
3. **频繁切换通道**：每次关闭都要给服务端发 close 帧，且关闭不能被无响应的服务端拖住。归 Task 4 `test_close_sends_close_frame` 和 `test_close_bounded_when_peer_silent`。
4. **单条消息很大**（关键帧 1MB 以上，或服务器一次推送超过缓冲上限）：数据必须完整，内存有上限。归 Task 2 `test_large_message_1mb`、Task 4 `test_backpressure_caps_buffer`。
5. **退出发生在 DNS 解析或 TCP 连接阶段**（弱网、服务器地址不可达）：≤ 200ms 返回 `AVERROR_EXIT`。归 Task 4 `test_interrupt_during_connect`；DNS 阶段只能在真机上验证，归 Task 7。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `ijkmedia/ijkplayer/ijkavformat/ijkws.h` | 对外接口：open/read/close + 测试辅助函数 |
| `ijkmedia/ijkplayer/ijkavformat/ijkws.c` | lws 驱动、握手、接收、超时、打断、关闭 |
| `ijkmedia/ijkplayer/ijkavformat/ijkws_buf.h/.c` | 字节环形缓冲（不依赖 FFmpeg 的 FIFO API，方便在主机上测试） |
| `ijkmedia/ijkplayer/ijkavformat/ijkws_resolve.c` | 设备端 DNS：包装 `ijk_tcp_getaddrinfo_nonblock` |
| `tests/ijkws/resolve_host.c` | 主机测试用 DNS（直接调 getaddrinfo） |
| `tests/ijkws/ws_test_server.py` | 标准库实现的 WebSocket 测试服务器，支持各种故障注入 |
| `tests/ijkws/test_*.c`、`tests/ijkws/testutil.h` | 主机 C 测试 |
| `tests/ijkws/run.sh` | 一键运行：生成 FLV 和证书、启动服务器、编译、运行测试 |
| `tools/build-libwebsockets-host.sh` | 编译 macOS 版 lws（只用于测试） |
| `ijkmedia/ijkplayer/ff_ffplay_def.h`、`ff_ffplay.c` | 接入自定义 AVIO |
| Android CMake、gradle、`IjkMediaPlayer.java` | 链接、打包、加载 `libwebsockets.so` |
| `ios/IJKMediaPlayer/IJKMediaPlayer.xcodeproj` | 加入源文件，链接 `libwebsockets.a` |

---

### Task 1：提交已有产物，补齐 Android arm64 FFmpeg，收窄 ABI

**Files:**
- 提交：`tools/build-libwebsockets-android.sh`、`tools/build-libwebsockets-ios.sh`、`.gitignore`、`doc/superpowers/specs/2026-11-19-ws-flv-libwebsockets-design.md`、本计划
- Modify：`android/ijkplayer/ijkplayer-cmake/build.gradle:19`、`android/ijkplayer/ijkplayer-example/build.gradle:18`

**Interfaces:**
- Produces：`third-libs/ijkffmpeg/ijkffmpeg-arm64/`；两个 ABI 的 `libwebsockets.so`；iOS 的 `libwebsockets.a`

- [ ] **Step 1：下载 Android 预编译 FFmpeg**
  Run：`cd android && ./install-ffmpeg.sh`
  Expected：`ls FFToolChain/build/product/android/universal/ijkffmpeg` 列出 `ijkffmpeg-arm64` 和 `ijkffmpeg-armv7a`。如果下载失败（网络原因），停下来告诉用户。
- [ ] **Step 2：把两个 build.gradle 的 abiFilters 改为 `"arm64-v8a","armeabi-v7a"`**
- [ ] **Step 3：基线编译（这时还没有 ws 代码）**
  Run：`cd android/ijkplayer && ./gradlew :ijkplayer-cmake:assembleRelease`
  Expected：BUILD SUCCESSFUL。这一步确认补齐的 FFmpeg 可用，后面再出问题就不是 FFmpeg 的原因。
- [ ] **Step 4：确认 lws 产物存在**（不存在就运行两个 build 脚本）
  Run：`ls FFToolChain/build/product/android/universal/libwebsockets/*/lib/libwebsockets.so FFToolChain/build/product/ios/*/libwebsockets/lib/libwebsockets.a`
  Expected：列出 4 个文件。
- [ ] **Step 5：Commit**
  ```bash
  git add tools/build-libwebsockets-*.sh .gitignore doc/superpowers android/ijkplayer/ijkplayer-cmake/build.gradle android/ijkplayer/ijkplayer-example/build.gradle
  git commit -m "ws-flv: libwebsockets 编译脚本、设计与计划；Android ABI 收窄为 arm64/armv7a"
  ```

---

### Task 2：主机测试框架 + 环形缓冲 + 正常收流

**Files:**
- Create：`tools/build-libwebsockets-host.sh`、`tests/ijkws/{run.sh,ws_test_server.py,testutil.h,resolve_host.c,test_stream.c}`
- Create：`ijkmedia/ijkplayer/ijkavformat/{ijkws.h,ijkws.c,ijkws_buf.h,ijkws_buf.c}`
- Modify：`.gitignore`（加入 `tests/ijkws/build/`）

**Interfaces:**
- Produces（`ijkws_buf.h`）：
  ```c
  typedef struct IjkWsBuf IjkWsBuf;
  IjkWsBuf *ijkws_buf_alloc(size_t initial);          // 按需扩容，不设上限（上限由调用方控制）
  int       ijkws_buf_write(IjkWsBuf *b, const uint8_t *p, size_t n);  // 0 或 AVERROR(ENOMEM)
  size_t    ijkws_buf_read(IjkWsBuf *b, uint8_t *dst, size_t n);      // 返回实际读出的字节数
  size_t    ijkws_buf_size(const IjkWsBuf *b);
  void      ijkws_buf_free(IjkWsBuf **b);
  ```
- Produces（`ijkws.h`，后续 Task 都依赖）：
  ```c
  typedef struct IjkWsContext IjkWsContext;
  /* int_cb 按值拷贝保存。opts 中的 ws_* 键会被读取并删除；user_agent 只读不删 */
  int  ijkws_open(IjkWsContext **out, const char *url, AVDictionary **opts, const AVIOInterruptCB *int_cb);
  int  ijkws_read(void *opaque, uint8_t *buf, int size);  /* >0 字节数；AVERROR_EOF/EXIT/ETIMEDOUT/EIO */
  void ijkws_close(IjkWsContext **ctx);                   /* 允许传入 NULL 或 *ctx == NULL */
  size_t ijkws_buffered_bytes(const IjkWsContext *ctx);   /* 测试用 */
  /* DNS 解析：设备端由 ijkws_resolve.c 实现，主机端由 tests/ijkws/resolve_host.c 实现 */
  int  ijkws_resolve(const char *host, int port, char *ip, size_t ip_size,
                     int64_t timeout_us, const AVIOInterruptCB *int_cb);  /* 0 或 AVERROR */
  ```

- [ ] **Step 1：`tools/build-libwebsockets-host.sh`**
  编译 macOS 静态 lws 到 `tests/ijkws/build/lws`，OpenSSL 用 `/opt/homebrew/opt/openssl@3`，其余 CMake 选项照抄 iOS 脚本（去掉 iOS 相关参数）。
  Expected：生成 `tests/ijkws/build/lws/lib/libwebsockets.a`。
- [ ] **Step 2：`tests/ijkws/ws_test_server.py`**
  只用标准库（asyncio + hashlib + base64 + ssl）实现最小 WebSocket 服务器。
  - 参数：`--port`（ws）、`--tls-port`（wss）、`--cert`、`--key`、`--flv`（要推送的文件）、`--log`（JSONL 事件日志：每个连接的请求行、全部握手 Header、收到的 close 码、连接结束时间）。
  - 按 path 选择行为：

  | path | 行为 |
  |---|---|
  | `/flv` | 每条消息 4096 字节（二进制帧），推完后发 close 1000 |
  | `/chunked` | 每条消息 7 字节 |
  | `/fragmented` | 每条消息拆成 3 个分片（opcode 2 + 0 + 0，FIN 只设在最后一个分片） |
  | `/text` | 同 `/flv`，但使用文本帧 |
  | `/ping` | 每推 3 条消息插一个 ping |
  | `/big` | 第一条消息 1MB，之后按 `/flv` 继续 |
  | `/flood` | 不限速推送 20MB，消息 64KB |
  | `/drop` | 推 64KB 后直接关闭 TCP，不发 close 帧 |
  | `/silent` | 握手完成后什么都不发 |
  | `/status/<code>` | 握手阶段直接返回这个 HTTP 状态码 |
  | `/close-silent` | 收到客户端 close 帧后不回复，也不断开 |

  其余未列出的 path 一律按 `/flv` 处理；query 原样记入日志。
- [ ] **Step 3：`tests/ijkws/run.sh`**
  1. 如果 `tests/ijkws/build/test.flv` 不存在，就生成：`/usr/local/ffmpeg/4.4/bin/ffmpeg -f lavfi -i testsrc=size=320x240:rate=25 -f lavfi -i sine -t 5 -c:v flv -c:a aac -f flv tests/ijkws/build/test.flv`
  2. 用 `openssl req -x509` 生成 CN=localhost、SAN=DNS:localhost 的自签名证书 `build/server.{crt,key}`，再生成另一张无关的 `build/other-ca.crt`
  3. 后台启动服务器，ws 端口 18765，wss 端口 18766
  4. 用 clang 编译每个 `test_*.c`，链接：ijkws 源文件、`resolve_host.c`、lws、`-L/usr/local/ffmpeg/4.4/lib -lavformat -lavutil`、openssl@3
  5. 逐个运行，结束时关闭服务器
  6. 输出 `PASS n / FAIL m`，有失败时退出码非 0
- [ ] **Step 4：`testutil.h`**
  提供：`CHECK(cond)`、`CHECK_EQ_INT`、`now_ms()`、`read_all(ctx, &buf, &len)`（循环调用 ijkws_read 直到返回值 ≤ 0，返回最后一次的返回值）、`load_file()`、一个基于 `volatile int` 的 interrupt callback，以及 `interrupt_after_ms(flag, ms)`（用 pthread 延时置位，**只在测试中使用**）。
- [ ] **Step 5：写失败测试 `tests/ijkws/test_stream.c`**
  ```c
  // 每个测试：ijkws_open 返回 0，read_all 拿到的字节和 test.flv 完全一致，最后一次返回 AVERROR_EOF
  test_ws_stream_bytes_equal          // ws://127.0.0.1:18765/flv
  test_wss_self_signed_default_ok     // wss://localhost:18766/flv，不设任何 TLS 选项
  test_chunked_messages_bytes_equal   // /chunked
  test_fragmented_messages_bytes_equal// /fragmented
  test_text_frames_accepted           // /text
  test_ping_interleaved_bytes_equal   // /ping
  test_large_message_1mb              // /big：服务器日志里的 1MB 前缀 + 文件内容，逐字节一致
  test_buf_wraparound                 // 纯 ijkws_buf：交替写 3000、读 2000，循环 1000 次，和参考数组对比
  ```
- [ ] **Step 6：运行，确认失败**
  Run：`tests/ijkws/run.sh`
  Expected：编译失败（还没有 ijkws 符号）。
- [ ] **Step 7：实现 `ijkws_buf.c`**
  环形数组，满了就按 2 倍扩容；扩容时把数据线性化。
- [ ] **Step 8：实现 `ijkws.c` 的正常路径**
  - `av_url_split` 解析 URL，默认端口 ws 80 / wss 443；scheme 不是 ws/wss 或 host 为空时返回 `AVERROR(EINVAL)`。
  - 调用 `ijkws_resolve` 得到 IP；设置 `connect_info.address = ip`、`.host = .origin = 原始 host`（lws 的 SNI 取自 `host`，见 `lib/tls/openssl/openssl-client.c:253`），`.path` 为 path+query。
  - 每个上下文一个 `lws_context`，`options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT`，`port = CONTEXT_PORT_NO_LISTEN`。
  - 注册 100ms 周期的 `lws_sul` 唤醒定时器，回调里重新 schedule 自己。
  - `LWS_CALLBACK_CLIENT_RECEIVE`：把数据写入 buf。
  - 收到 `LWS_CALLBACK_WS_PEER_INITIATED_CLOSE` 时记下 `peer_closed = 1`；之后在 `CLIENT_CLOSED` 中，`peer_closed` 为 1 就标记为 EOF，否则标记为 EIO。
  - wss 默认 flags：`LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK | LCCSCF_ALLOW_EXPIRED`。
  - 首次 open 时设置一次 `lws_set_log_level(LLL_ERR | LLL_WARN, …)`，输出转发到 `av_log`。
- [ ] **Step 9：实现 `resolve_host.c`**
  `getaddrinfo` + `getnameinfo(NI_NUMERICHOST)`，忽略 timeout 和 int_cb。
- [ ] **Step 10：运行**
  Run：`tests/ijkws/run.sh`
  Expected：Step 5 的 8 个测试全部 PASS。
- [ ] **Step 11：Commit**
  ```bash
  git add tools/build-libwebsockets-host.sh tests/ijkws ijkmedia/ijkplayer/ijkavformat/ijkws* .gitignore
  git commit -m "ijkws: 基于 libwebsockets 的 ws/wss 收流模块与主机测试"
  ```

---

### Task 3：握手失败、超时与断线

**Files:**
- Create：`tests/ijkws/test_errors.c`
- Modify：`ijkmedia/ijkplayer/ijkavformat/ijkws.c`

**Interfaces:**
- Consumes：Task 2 中 `ijkws.h` 的全部接口。

- [ ] **Step 1：写失败测试**
  ```c
  test_handshake_http_status_mapped   // /status/401 → AVERROR_HTTP_UNAUTHORIZED；/status/403 → AVERROR_HTTP_FORBIDDEN；
                                      // /status/404 → AVERROR_HTTP_NOT_FOUND；/status/500 → AVERROR_HTTP_SERVER_ERROR
  test_connection_refused             // ws://127.0.0.1:18799/ → < 0，且耗时 < 1000ms
  test_connect_timeout                // ws://10.255.255.1/，ws_timeout=500 → AVERROR(ETIMEDOUT)，耗时在 450..800ms
  test_silent_server_read_timeout     // /silent，ws_timeout=500：open 返回 0；read → AVERROR(ETIMEDOUT)，耗时在 450..800ms
  test_drop_without_close_is_eio      // /drop：读完 64KB 后 → AVERROR(EIO)
  test_bad_url_einval                 // "ws://"、"http://x/" → AVERROR(EINVAL)
  ```
- [ ] **Step 2：运行**
  Run：`tests/ijkws/run.sh`
  Expected：上面这些测试 FAIL（Task 2 的测试仍然 PASS）。
- [ ] **Step 3：实现**
  - `LWS_CALLBACK_CLIENT_CONNECTION_ERROR` 中用 `lws_http_client_http_response(wsi)` 取 HTTP 状态码：401/403/404 映射到对应的 `AVERROR_HTTP_*`；其他 4xx 映射为 `AVERROR_HTTP_OTHER_4XX`；5xx 映射为 `AVERROR_HTTP_SERVER_ERROR`；没有状态码时返回 `AVERROR(EIO)`。失败原因要用 `AV_LOG_ERROR` 输出。
  - 连接超时从 open 开始计时；读超时从最后一次收到数据开始计时；两者都用 `ws_timeout`，默认 10000ms。
- [ ] **Step 4：运行**
  Expected：Task 2、Task 3 的测试全部 PASS。
- [ ] **Step 5：Commit** `ijkws: 握手失败映射 HTTP 错误码、连接/读超时、断线处理`

---

### Task 4：打断、优雅关闭、背压

**Files:**
- Create：`tests/ijkws/test_lifecycle.c`
- Modify：`ijkmedia/ijkplayer/ijkavformat/ijkws.c`

- [ ] **Step 1：写失败测试**
  ```c
  test_interrupt_during_read          // /silent，ws_timeout=10000，300ms 后置位打断 → read 返回 AVERROR_EXIT，耗时 ≤ 300+200ms
  test_interrupt_during_connect       // ws://10.255.255.1/，100ms 后置位打断 → open 返回 AVERROR_EXIT，耗时 ≤ 300ms
  test_close_sends_close_frame        // /flv 读 100KB 后 close：耗时 ≤ 150ms；服务器日志记录到 close 码 1000
  test_close_bounded_when_peer_silent // /close-silent：close 耗时 ≤ 150ms
  test_close_null_safe                // ijkws_close(NULL)；IjkWsContext *c = NULL; ijkws_close(&c); 都不崩溃
  test_backpressure_caps_buffer       // /flood：每次读 4KB，每次读完 sleep 1ms，全程 ijkws_buffered_bytes ≤ 4MB + 64KB；
                                      // 最终读到的总字节数 == 20MB
  ```
- [ ] **Step 2：运行**
  Expected：上面这些测试 FAIL。
- [ ] **Step 3：实现**
  - 在 open 和 read 循环的每一轮都检查 int_cb，触发时返回 `AVERROR_EXIT`。
  - close：
    1. 如果还处于 ESTABLISHED：先调用 `lws_close_reason(wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0)`，再 `lws_callback_on_writable`，在 WRITEABLE 回调里返回 -1。
    2. 继续 service，直到 `CLIENT_CLOSED` 或满 100ms。
    3. `lws_context_destroy`，然后释放 buf。
  - 背压：buf 超过 4MB 时调用 `lws_rx_flow_control(wsi, 0)`；read 之后 buf 低于 1MB 时调用 `lws_rx_flow_control(wsi, 1)`。
- [ ] **Step 4：运行**
  Expected：全部 PASS。连续运行 3 次，确认时间相关的测试结果稳定。
- [ ] **Step 5：Commit** `ijkws: 打断、限时优雅关闭、接收背压`

---

### Task 5：选项与 TLS 校验

**Files:**
- Create：`tests/ijkws/test_options.c`
- Modify：`ijkmedia/ijkplayer/ijkavformat/ijkws.c`

- [ ] **Step 1：写失败测试**
  ```c
  test_headers_forwarded   // url "/flv?token=abc&ch=2"，opts：ws_headers="X-Token: t1\r\nX-Dev: 9"、user_agent="UA/1"、
                           // ws_origin="http://o.example"、ws_protocol="flv"
                           // → 服务器日志：path 为 "/flv?token=abc&ch=2"，Header 中有 X-Token、X-Dev、User-Agent、Origin、
                           //   Sec-WebSocket-Protocol: flv
                           // （测试服务器在握手请求带子协议时，要在响应里回同一个子协议）
  test_opts_consumed       // open 之后 opts 中没有任何 "ws_" 开头的键，user_agent 仍在
  test_tls_verify_ok       // wss://localhost:18766/flv，ws_tls_verify=1，ws_ca_file=build/server.crt → open 返回 0，读到的数据与文件一致
  test_tls_verify_wrong_ca // 同上，但 ws_ca_file=build/other-ca.crt → open 返回 < 0
  test_tls_verify_no_ca    // ws_tls_verify=1，没有 ws_ca_file → open 返回 AVERROR(EINVAL)，并输出日志说明需要 CA 文件
  ```
- [ ] **Step 2：运行**
  Expected：上面这些测试 FAIL。
- [ ] **Step 3：实现**
  - 解析选项；在 `LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER` 回调里追加 `ws_headers` 和 User-Agent。
  - 校验模式：
    - flags 只用 `LCCSCF_USE_SSL`
    - 通过 `client_ssl_ca_filepath` 加载 CA。lws v4.3 中这个字段在 `lws_context_creation_info`，实现时以源码为准。
- [ ] **Step 4：运行**
  Expected：全部 PASS。
- [ ] **Step 5：Commit** `ijkws: 握手 Header、子协议、选项消费、可选证书校验`

---

### Task 6：接入 ff_ffplay（自定义 AVIO）+ 设备端 DNS

**Files:**
- Create：`ijkmedia/ijkplayer/ijkavformat/ijkws_resolve.c`
- Modify：
  - `ijkmedia/ijkplayer/ff_ffplay_def.h`：`VideoState` 中加 `struct IjkWsContext *ws; AVIOContext *ws_avio;`（约 275-421 行）
  - `ijkmedia/ijkplayer/ff_ffplay.c`：
    - `read_thread`：在 3113 行的 `avformat_open_input` 之前加入 ws 分支
    - `fail:`：约 3620 行
    - `stream_close`：1005 行之后

**Interfaces:**
- Consumes：`ijkws_open/ijkws_read/ijkws_close`。
- Produces：`static void ws_release(VideoState *is)`，可以重复调用。

- [ ] **Step 1：实现 `ijkws_resolve.c`**
  - 在文件内自行 extern 声明：
    ```c
    int ijk_tcp_getaddrinfo_nonblock(const char *hostname, const char *servname,
                                     const struct addrinfo *hints, struct addrinfo **res,
                                     int64_t timeout, const AVIOInterruptCB *int_cb, int one_by_one);
    ```
    签名来自 `FFToolChain/patches/ffmpeg-n4.0/0010-tcp-hook.patch:270`。
  - 取第一个结果，用 `getnameinfo(NI_NUMERICHOST)` 转成 IP 字符串。
  - 被打断时返回 `AVERROR_EXIT`。
- [ ] **Step 2：修改 `read_thread`**
  filename 以 `ws://` 或 `wss://` 开头（不区分大小写）时：
  1. `ijkws_open(&is->ws, is->filename, &ffp->format_opts, &ic->interrupt_callback)`，失败时设置 `last_error`，然后 `goto fail`
  2. 分配 32768 字节的 avio buffer，`avio_alloc_context(buf, 32768, 0, is->ws, ijkws_read, NULL, NULL)`，`seekable = 0`
  3. `ic->pb = is->ws_avio; ic->flags |= AVFMT_FLAG_CUSTOM_IO;`
  4. 如果 `is->iformat` 为空，设为 `av_find_input_format("flv")`
- [ ] **Step 3：实现 `ws_release(is)` 并接入两处**
  - `ws_release` 的内容：`av_freep(&is->ws_avio->buffer)`；`avio_context_free(&is->ws_avio)`；`ijkws_close(&is->ws)`。
  - `fail:` 中，在 `if (ic && !is->ic) avformat_close_input(&ic);` 之后，仅当 `!is->ic` 时调用。
  - `stream_close` 中，在 `avformat_close_input(&is->ic)` 之后调用。
- [ ] **Step 4：Android 编译通过**（`ijkws_resolve.c` 和 `ijkws*.c` 都在 GLOB 范围内）
  这一步要先完成 Task 7 Step 1 的 CMake 链接配置，否则链接不过。所以 Task 6 和 Task 7 Step 1 一起提交。
  Run：`cd android/ijkplayer && ./gradlew :ijkplayer-cmake:assembleRelease`
  Expected：BUILD SUCCESSFUL。
- [ ] **Step 5：Commit**（与 Task 7 Step 1 一起）`ff_ffplay: ws/wss 地址走 ijkws 自定义 AVIO`

---

### Task 7：Android 集成与真机验证

**Files:**
- Modify：
  - `android/ijkplayer/ijkplayer-cmake/src/main/cpp/CMakeLists.txt`：约 110 行的库路径、`include_directories`、`target_link_libraries`
  - `android/ijkplayer/ijkplayer-cmake/src/main/java/tv/danmaku/ijk/media/player/IjkMediaPlayer.java:190`
  - `android/ijkplayer/ijkplayer-example/.../fragments/SampleMediaListFragment.java`：加两个测试条目

- [ ] **Step 1：CMake 配置**
  - `add_library(websockets-lib SHARED IMPORTED)`，`IMPORTED_LOCATION` 为 `third-libs/libwebsockets/libwebsockets-${ARCH}/lib/libwebsockets.so`
  - include 目录加入 `third-libs/libwebsockets/libwebsockets-${ARCH}/include`
  - 主目标链接 `websockets-lib`
- [ ] **Step 2：Java 加载顺序**
  在 `libLoader.loadLibrary("ijkplayer")` 之前加 `libLoader.loadLibrary("websockets");`。
- [ ] **Step 3：检查打包和依赖**
  Run：`./gradlew :ijkplayer-cmake:assembleRelease && unzip -l ijkplayer-cmake/build/outputs/aar/*release.aar | grep '\.so'`
  Expected：`jni/arm64-v8a/` 和 `jni/armeabi-v7a/` 下都有 `libijkplayer.so` 和 `libwebsockets.so`。
  另外运行 `llvm-readelf -d` 检查 libijkplayer.so，`NEEDED` 里要有 `libwebsockets.so`。
  如果 AAR 里没有 libwebsockets.so，就改为复制到 `src/main/jniLibs/<abi>/`，然后重新检查。
- [ ] **Step 4：示例 App 加测试条目**
  - `ws-flv local`：`ws://127.0.0.1:18765/flv`
  - `wss-flv local`：`wss://localhost:18766/flv`

  在 Mac 上运行 `tests/ijkws/run.sh --serve-only`（给 run.sh 加这个参数：只启动服务器，不跑测试），并执行 `adb reverse tcp:18765 tcp:18765; adb reverse tcp:18766 tcp:18766`。
- [ ] **Step 5：真机验证（arm64 设备一台；有 armv7a 设备的话也测一台）**
  逐项记录结果：
  - 两个条目都能起播，画面和声音正常；
  - 播放中按返回键退出，用 logcat 看 ijkws close 日志，从退出到 close 完成 ≤ 200ms；
  - 在 Android 5.x 模拟器或设备上能启动（验证加载顺序）；如果没有这类设备，就注明"未验证"；
  - 播放 `/status/401` 时，App 收到的 error 码等于 `AVERROR_HTTP_UNAUTHORIZED` 对应的值；
  - **DNS 阶段打断**：开启飞行模式后再连 Wi-Fi，但让 DNS 不可用（或者把 URL 改成一个解析很慢的域名），起播后立刻退出，确认不卡顿、无 ANR；
  - 回归：示例 App 中原有的 http-flv 条目仍能正常播放。
- [ ] **Step 6：Commit** `android: 链接并加载 libwebsockets.so，示例增加 ws-flv 测试条目`

---

### Task 8：iOS 集成与真机验证

**Files:**
- Modify：
  - `ios/IJKMediaPlayer/IJKMediaPlayer.xcodeproj/project.pbxproj`
  - `ios/IJKMediaDemo`：不改代码，使用现有的 `IJKDemoInputURLViewController` 手动输入地址

- [ ] **Step 1：修改 Xcode 工程**
  - 把 `ijkws.c`、`ijkws.h`、`ijkws_buf.c`、`ijkws_buf.h`、`ijkws_resolve.c` 加入 IJKMediaFramework target。
  - Header Search Paths 和 Library Search Paths 按 sdk 区分（参照工程中 openssl 的现有写法，约 1071-1076 行）：
    - 真机：`../../FFToolChain/build/product/ios/universal/libwebsockets/{include,lib}`
    - 模拟器：`.../universal-simulator/libwebsockets/{include,lib}`
  - Other Linker Flags 加 `-lwebsockets`。
- [ ] **Step 2：编译并检查符号**
  Run：`cd ios && ./build-framework.sh && nm -gU IJKMediaPlayer/Release/Release-iphoneos/IJKMediaFramework.framework/IJKMediaFramework | grep -c lws_create_context`
  Expected：≥ 1。模拟器 framework 做同样检查。
- [ ] **Step 3：真机和模拟器验证**
  - 模拟器：输入 `ws://127.0.0.1:18765/flv` 和 `wss://localhost:18766/flv`，确认能起播、退出及时。
  - 真机：输入 `ws://<Mac 的局域网 IP>:18765/flv`，项目和 Android Step 5 相同（DNS 打断项用 Network Link Conditioner 的 100% Loss 配置测试）。
- [ ] **Step 4：Commit** `ios: IJKMediaFramework 链接 libwebsockets，接入 ijkws`

---

### Task 9：对接真实服务器的稳定性验证（手工执行，结果写进文档）

**Files:**
- Create：`doc/ws-flv-verification.md`（记录每一项的结论、日期、设备、App 版本）

- [ ] **Step 1：向用户要真实的 ws/wss 地址**（如果需要 token，也请用户提供获取方式）
- [ ] **Step 2：24 小时挂流**
  iOS 和 Android 各一台。每小时记录一次内存（Android `dumpsys meminfo`，iOS Xcode Memory gauge）。
  Expected：无崩溃，内存曲线平稳，24 小时内涨幅 < 10MB。
- [ ] **Step 3：弱网**
  - 场景：iOS Network Link Conditioner 的 3G / Very Bad Network；Android 用限速代理或路由器限速；中途断网 30 秒后恢复。
  - Expected：卡顿后能恢复；断网期间在 `ws_timeout` 内报错，App 收到 error 后重新 prepare 能恢复播放。
- [ ] **Step 4：频繁切换通道**
  每秒切一次，持续 30 分钟。
  Expected：无崩溃、无 ANR；服务端会话数不累积（请用户在服务端确认）。
- [ ] **Step 5：回归**
  http-flv、hls、rtsp、本地文件各播放一次。
- [ ] **Step 6：Commit** `doc: ws-flv 真机与稳定性验证记录`
