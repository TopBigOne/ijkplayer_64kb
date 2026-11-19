/*
 * ijkws.h —— 基于 libwebsockets 的 ws/wss 收流模块
 *
 * 每个实例独占一个 lws_context，全部操作在调用线程（read_thread）中同步驱动，
 * 不创建线程。ijkws_read 的签名与 avio_alloc_context 的 read_packet 一致，
 * 供 ff_ffplay.c 以自定义 AVIOContext 的方式接入 FLV 解封装。
 */
#ifndef IJKAVFORMAT_IJKWS_H
#define IJKAVFORMAT_IJKWS_H

#include <stddef.h>
#include <stdint.h>

#include "libavformat/avio.h"
#include "libavutil/dict.h"

typedef struct IjkWsContext IjkWsContext;

/* int_cb 按值拷贝保存。opts 中的 ws_* 键会被读取并删除；user_agent 只读不删 */
int  ijkws_open(IjkWsContext **out, const char *url, AVDictionary **opts, const AVIOInterruptCB *int_cb);
int  ijkws_read(void *opaque, uint8_t *buf, int size);  /* >0 字节数；AVERROR_EOF/EXIT/ETIMEDOUT/EIO */
void ijkws_close(IjkWsContext **ctx);                   /* 允许传入 NULL 或 *ctx == NULL */
size_t ijkws_buffered_bytes(const IjkWsContext *ctx);   /* 测试用 */
/* DNS 解析：设备端由 ijkws_resolve.c 实现，主机端由 tests/ijkws/resolve_host.c 实现 */
int  ijkws_resolve(const char *host, int port, char *ip, size_t ip_size,
                   int64_t timeout_us, const AVIOInterruptCB *int_cb);  /* 0 或 AVERROR */

#endif
