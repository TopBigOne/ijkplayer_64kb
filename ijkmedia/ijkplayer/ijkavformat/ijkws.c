/*
 * ijkws.c —— 基于 libwebsockets 的 ws/wss 收流模块
 *
 * 每个实例独占一个 lws_context，所有 lws 调用都发生在调用线程（read_thread）中，
 * 不创建线程。收到的 payload（二进制/文本帧、分片消息均可）按到达顺序追加到
 * ijkws_buf，ijkws_read 再从中取出，作为自定义 AVIOContext 的数据源。
 *
 * lws v4.3 的 lws_service() 超时参数实际无效（非负值会一直等到下一个调度事件），
 * 因此注册一个 100ms 周期的 lws_sul 定时器，保证每轮 service 最多阻塞约 100ms，
 * 回到调用方的检查点（打断、超时等）。
 */
#include "ijkws.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libwebsockets.h>

#include "libavformat/avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/time.h"

#include "ijkws_buf.h"

#define IJKWS_WAKE_INTERVAL_MS   100
/* ijkws_close 里优雅关闭（发 close 帧、等对端回应）的时间上限，以及这期间的唤醒间隔 */
#define IJKWS_CLOSE_WAIT_MS      100
#define IJKWS_CLOSE_WAKE_MS      10
#define IJKWS_DEFAULT_TIMEOUT_MS 10000
#define IJKWS_BUF_INITIAL        (256 * 1024)
/* 每个 lws_context 只有一条连接；限制 fd 表大小，避免按进程 ulimit 分配大数组 */
#define IJKWS_FD_LIMIT           8
/*
 * 接收背压：缓冲超过高水位时暂停接收（lws_rx_flow_control），read 取走数据后低于低水位再恢复。
 * service 只在 read 等数据时驱动，正常情况下缓冲不会涨到高水位，这里只是保护。
 * 允许编译时覆盖，仅供主机测试用小水位验证暂停/恢复路径。
 */
#ifndef IJKWS_RX_HIGH_WATER
#define IJKWS_RX_HIGH_WATER      (4 * 1024 * 1024)
#endif
#ifndef IJKWS_RX_LOW_WATER
#define IJKWS_RX_LOW_WATER       (1 * 1024 * 1024)
#endif

enum {
    IJKWS_STATE_CONNECTING = 0,
    IJKWS_STATE_ESTABLISHED,
    IJKWS_STATE_EOF,     /* 对端发过 close 帧后关闭 */
    IJKWS_STATE_ERROR,   /* 连接失败或无 close 帧断开；错误码见 error */
};

struct IjkWsContext {
    struct lws_context    *lws;
    struct lws            *wsi;
    lws_sorted_usec_list_t wake_sul;

    IjkWsBuf              *buf;
    AVIOInterruptCB        int_cb;

    int                    state;
    int                    error;
    int                    peer_closed;
    int                    closing;     /* ijkws_close 中，不再输出断线日志、不再缓存数据 */
    int                    close_pending; /* 下一次 WRITEABLE 回调返回 -1，由 lws 发 close 帧 */
    int                    rx_paused;   /* 已 lws_rx_flow_control(wsi, 0) */
    int                    wake_ms;     /* wake_sul 的周期 */

    int                    timeout_ms;  /* ws_timeout：连接+握手超时，也是读取期间无数据的超时 */
    int64_t                last_rx_us;  /* 最近一次收到数据（或握手成功）的时间，av_gettime_relative */

    char                   scheme[8];
    char                   host[256];
    char                   host_hdr[272];   /* Host 头：host 或 host:port，IPv6 字面量加 [] */
    char                   ip[64];
    char                   path[4096];
    int                    port;
};

/* ---------------------------------------------------------------- 日志 */

static void lws_log_to_av_log(int level, const char *line)
{
    int av_level = (level & LLL_ERR) ? AV_LOG_ERROR : AV_LOG_WARNING;
    av_log(NULL, av_level, "ijkws/lws: %s", line);
}

static pthread_once_t g_log_once = PTHREAD_ONCE_INIT;

static void init_lws_log(void)
{
    lws_set_log_level(LLL_ERR | LLL_WARN, lws_log_to_av_log);
}

/* ---------------------------------------------------------------- lws 回调 */

static void wake_cb(lws_sorted_usec_list_t *sul)
{
    IjkWsContext *ctx = lws_container_of(sul, IjkWsContext, wake_sul);
    lws_sul_schedule(ctx->lws, 0, &ctx->wake_sul, wake_cb,
                     (lws_usec_t)ctx->wake_ms * LWS_US_PER_MS);
}

static void set_error(IjkWsContext *ctx, int err)
{
    if (ctx->state != IJKWS_STATE_ERROR && ctx->state != IJKWS_STATE_EOF) {
        ctx->state = IJKWS_STATE_ERROR;
        ctx->error = err;
    }
}

/* 握手 HTTP 状态码 → FFmpeg 错误码；没有状态码（或非错误码）时为 EIO */
static int http_status_to_averror(unsigned int status)
{
    switch (status) {
    case 401: return AVERROR_HTTP_UNAUTHORIZED;
    case 403: return AVERROR_HTTP_FORBIDDEN;
    case 404: return AVERROR_HTTP_NOT_FOUND;
    default:  break;
    }
    if (status >= 400 && status < 500)
        return AVERROR_HTTP_OTHER_4XX;
    if (status >= 500 && status < 600)
        return AVERROR_HTTP_SERVER_ERROR;
    return AVERROR(EIO);
}

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    IjkWsContext *ctx = (IjkWsContext *)lws_context_user(lws_get_context(wsi));

    if (!ctx)
        return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        ctx->state = IJKWS_STATE_ESTABLISHED;
        ctx->last_rx_us = av_gettime_relative();
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        /*
         * 握手被拒时（lws_client_interpret_server_handshake -> bail3），lws 先回调这里，
         * 之后才关闭 wsi 并释放 ah，因此此刻 lws_http_client_http_response 仍能取到状态码；
         * TCP/TLS 阶段失败时还没有响应，返回 0。in 是 lws 给出的原因字符串（可能为 NULL）。
         */
        unsigned int status = wsi ? lws_http_client_http_response(wsi) : 0;
        int err = http_status_to_averror(status);
        if (status)
            av_log(NULL, AV_LOG_ERROR, "ijkws: connect %s://%s:%d%s failed: HTTP %u (%s)\n",
                   ctx->scheme, ctx->host, ctx->port, ctx->path, status,
                   in ? (const char *)in : "unknown");
        else
            av_log(NULL, AV_LOG_ERROR, "ijkws: connect %s://%s:%d%s failed: %s\n",
                   ctx->scheme, ctx->host, ctx->port, ctx->path,
                   in ? (const char *)in : "unknown");
        set_error(ctx, err);
        ctx->wsi = NULL;
        break;
    }

    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (ctx->closing)
            break;   /* 正在关闭，数据已没人读 */
        ctx->last_rx_us = av_gettime_relative();
        if (len > 0 && ijkws_buf_write(ctx->buf, (const uint8_t *)in, len) < 0) {
            av_log(NULL, AV_LOG_ERROR, "ijkws: out of memory buffering %zu bytes\n", len);
            set_error(ctx, AVERROR(ENOMEM));
            return -1;
        }
        if (!ctx->rx_paused && ijkws_buf_size(ctx->buf) > IJKWS_RX_HIGH_WATER) {
            /* 回调内调用时 lws 在本次回调返回后生效，余下已读入的数据由 lws 暂存 */
            av_log(NULL, AV_LOG_VERBOSE, "ijkws: buffered %zu bytes, pause receiving\n",
                   ijkws_buf_size(ctx->buf));
            ctx->rx_paused = 1;
            lws_rx_flow_control(wsi, 0);
        }
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        /*
         * 只有 ijkws_close 会请求可写回调。返回 -1 让 lws 关闭 wsi：由于之前用
         * lws_close_reason 设置了关闭码，rops_close_via_role_protocol_ws 会进入
         * LRS_WAITING_TO_SEND_CLOSE，发出 close 帧后进入 LRS_AWAITING_CLOSE_ACK 等对端回应。
         * 只能返回一次 -1：发完 close 帧后 lws 还会再给一次 WRITEABLE，此时再返回 -1，
         * __lws_close_free_wsi 会认为“已经礼貌关闭过”直接断 TCP，不再等对端回应；
         * 若本地还有未读数据，断开会发 RST，对端可能连 close 帧都来不及读到。
         */
        if (ctx->close_pending) {
            ctx->close_pending = 0;
            return -1;
        }
        break;

    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        ctx->peer_closed = 1;
        if (len >= 2) {
            const uint8_t *p = (const uint8_t *)in;
            int code = (p[0] << 8) | p[1];
            av_log(NULL, code == LWS_CLOSE_STATUS_NORMAL ? AV_LOG_VERBOSE : AV_LOG_WARNING,
                   "ijkws: peer close %d %.*s\n", code, (int)(len - 2), (const char *)p + 2);
        }
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        if (ctx->peer_closed) {
            if (ctx->state != IJKWS_STATE_ERROR)
                ctx->state = IJKWS_STATE_EOF;
        } else {
            if (!ctx->closing && ctx->state == IJKWS_STATE_ESTABLISHED)
                av_log(NULL, AV_LOG_ERROR, "ijkws: connection closed without close frame\n");
            set_error(ctx, AVERROR(EIO));
        }
        ctx->wsi = NULL;
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols ws_protocols[] = {
    { "ijkws", ws_callback, 0, 0, 0, NULL, 0 },
    LWS_PROTOCOL_LIST_TERM
};

/* ---------------------------------------------------------------- 内部工具 */

static int check_interrupt(const IjkWsContext *ctx)
{
    return ctx->int_cb.callback && ctx->int_cb.callback(ctx->int_cb.opaque);
}

/* 缓冲已降到低水位以下时恢复接收 */
static void maybe_resume_rx(IjkWsContext *ctx)
{
    if (ctx->rx_paused && ctx->wsi && ijkws_buf_size(ctx->buf) < IJKWS_RX_LOW_WATER) {
        ctx->rx_paused = 0;
        lws_rx_flow_control(ctx->wsi, 1);
    }
}

/* 跑一轮 service；lws 自身出错时把连接标记为 EIO */
static void service_once(IjkWsContext *ctx)
{
    if (lws_service(ctx->lws, 0) < 0)
        set_error(ctx, AVERROR(EIO));
}

/* 读取并删除 ws_timeout（毫秒）；缺省或非法时用默认值 */
static void parse_options(IjkWsContext *ctx, AVDictionary **opts)
{
    AVDictionaryEntry *e;

    ctx->timeout_ms = IJKWS_DEFAULT_TIMEOUT_MS;
    if (!opts)
        return;
    e = av_dict_get(*opts, "ws_timeout", NULL, 0);
    if (e) {
        char *end = NULL;
        long v = strtol(e->value, &end, 10);
        if (end && end != e->value && *end == '\0' && v > 0 && v <= INT32_MAX)
            ctx->timeout_ms = (int)v;
        else
            av_log(NULL, AV_LOG_WARNING, "ijkws: invalid ws_timeout '%s', using %d ms\n",
                   e->value, IJKWS_DEFAULT_TIMEOUT_MS);
        av_dict_set(opts, "ws_timeout", NULL, 0);
    }
}

static int parse_url(IjkWsContext *ctx, const char *url)
{
    char path[sizeof(ctx->path)];
    int port = -1;

    av_url_split(ctx->scheme, sizeof(ctx->scheme), NULL, 0,
                 ctx->host, sizeof(ctx->host), &port,
                 path, sizeof(path), url);

    if (av_strcasecmp(ctx->scheme, "ws") && av_strcasecmp(ctx->scheme, "wss"))
        return AVERROR(EINVAL);
    if (!ctx->host[0])
        return AVERROR(EINVAL);

    if (port <= 0)
        port = av_strcasecmp(ctx->scheme, "wss") ? 80 : 443;
    ctx->port = port;

    /*
     * Host 头（RFC 6455 §4.1 / RFC 7230 §5.4）：默认端口只写主机名，否则写 host:port；
     * IPv6 字面量（av_url_split 已去掉方括号）需要重新加上 []。
     * wss 的 SNI 也取自 connect_info.host，lws 会在第一个 ':' 处截断，因此 SNI 仍是裸主机名。
     */
    {
        int is_default = port == (av_strcasecmp(ctx->scheme, "wss") ? 80 : 443);
        int is_ipv6 = strchr(ctx->host, ':') != NULL;
        if (is_default)
            snprintf(ctx->host_hdr, sizeof(ctx->host_hdr), is_ipv6 ? "[%s]" : "%s", ctx->host);
        else
            snprintf(ctx->host_hdr, sizeof(ctx->host_hdr), is_ipv6 ? "[%s]:%d" : "%s:%d", ctx->host, port);
    }

    /* av_url_split 给出的 path 已包含 query；为空或直接以 '?' 开头时补上 '/' */
    if (path[0] == '/')
        av_strlcpy(ctx->path, path, sizeof(ctx->path));
    else
        snprintf(ctx->path, sizeof(ctx->path), "/%s", path);
    return 0;
}

/* ---------------------------------------------------------------- 对外接口 */

int ijkws_open(IjkWsContext **out, const char *url, AVDictionary **opts, const AVIOInterruptCB *int_cb)
{
    struct lws_context_creation_info info;
    struct lws_client_connect_info ci;
    IjkWsContext *ctx;
    int64_t deadline_us, remain_us;
    int is_wss;
    int ret;

    if (!out)
        return AVERROR(EINVAL);
    *out = NULL;
    if (!url)
        return AVERROR(EINVAL);

    pthread_once(&g_log_once, init_lws_log);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return AVERROR(ENOMEM);
    if (int_cb)
        ctx->int_cb = *int_cb;
    ctx->wake_ms = IJKWS_WAKE_INTERVAL_MS;

    parse_options(ctx, opts);
    /* 连接超时从 open 开始计时，覆盖 DNS、TCP、TLS 和 WebSocket 握手 */
    deadline_us = av_gettime_relative() + (int64_t)ctx->timeout_ms * 1000;

    ret = parse_url(ctx, url);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: invalid url %s\n", url);
        goto fail;
    }
    is_wss = !av_strcasecmp(ctx->scheme, "wss");

    ctx->buf = ijkws_buf_alloc(IJKWS_BUF_INITIAL);
    if (!ctx->buf) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* 失败原因由 ijkws_resolve 自己输出（每次失败只记一行） */
    ret = ijkws_resolve(ctx->host, ctx->port, ctx->ip, sizeof(ctx->ip),
                        deadline_us - av_gettime_relative(), &ctx->int_cb);
    if (ret < 0)
        goto fail;

    memset(&info, 0, sizeof(info));
    info.port                = CONTEXT_PORT_NO_LISTEN;
    info.protocols           = ws_protocols;
    info.options             = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user                = ctx;
    info.fd_limit_per_thread = IJKWS_FD_LIMIT;
    /*
     * lws 自己的超时放宽到 ws_timeout 之后，以本模块的计时为准：
     * timeout_secs 管握手阶段的 pending 超时（默认 20s，client-http.c），
     * connect_timeout_secs 管 TCP connect（vhost 默认 20s，vhost.c / connect3.c）。
     */
    info.timeout_secs         = (unsigned int)(ctx->timeout_ms / 1000 + 2);
    info.connect_timeout_secs = (unsigned int)(ctx->timeout_ms / 1000 + 2);
    ctx->lws = lws_create_context(&info);
    if (!ctx->lws) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: lws_create_context failed\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    lws_sul_schedule(ctx->lws, 0, &ctx->wake_sul, wake_cb,
                     (lws_usec_t)ctx->wake_ms * LWS_US_PER_MS);

    memset(&ci, 0, sizeof(ci));
    ci.context  = ctx->lws;
    ci.address  = ctx->ip;     /* 已解析的 IP，lws 不再做 DNS */
    ci.port     = ctx->port;
    ci.path     = ctx->path;
    ci.host     = ctx->host_hdr; /* Host 头；wss 的 SNI 也取自这里（lws 去掉 :port） */
    ci.origin   = ctx->host;
    ci.protocol = NULL;
    ci.pwsi     = &ctx->wsi;
    if (is_wss) {
        /*
         * 默认不校验证书（与工程现有 https 的 tls_verify=0 一致）。
         * ALLOW_INSECURE 用于接受“找不到签发者”的证书：设备上没有 OpenSSL 可用的
         * 系统 CA，否则任何正规 CA 签发的证书都会因 UNABLE_TO_GET_ISSUER_CERT_LOCALLY 失败。
         */
        ci.ssl_connection = LCCSCF_USE_SSL |
                            LCCSCF_ALLOW_SELFSIGNED |
                            LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK |
                            LCCSCF_ALLOW_EXPIRED |
                            LCCSCF_ALLOW_INSECURE;
    }

    if (!lws_client_connect_via_info(&ci) && ctx->state != IJKWS_STATE_ERROR) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: lws_client_connect_via_info failed\n");
        set_error(ctx, AVERROR(EIO));
    }

    while (ctx->state == IJKWS_STATE_CONNECTING) {
        if (check_interrupt(ctx)) {
            ret = AVERROR_EXIT;
            goto fail;
        }
        remain_us = deadline_us - av_gettime_relative();
        if (remain_us <= 0) {
            av_log(NULL, AV_LOG_ERROR, "ijkws: connect %s://%s:%d%s timeout after %d ms\n",
                   ctx->scheme, ctx->host, ctx->port, ctx->path, ctx->timeout_ms);
            ret = AVERROR(ETIMEDOUT);
            goto fail;
        }
        service_once(ctx);
    }
    if (ctx->state != IJKWS_STATE_ESTABLISHED) {
        ret = ctx->error ? ctx->error : AVERROR(EIO);
        goto fail;
    }

    *out = ctx;
    return 0;

fail:
    ijkws_close(&ctx);
    return ret;
}

int ijkws_read(void *opaque, uint8_t *buf, int size)
{
    IjkWsContext *ctx = (IjkWsContext *)opaque;
    int64_t wait_start_us;

    if (!ctx || !buf || size <= 0)
        return AVERROR(EINVAL);

    /*
     * 读超时只计本次 read 等待期间的静默：起点取 max(进入本次 read 的时刻, 最近收到数据的时刻)。
     * 调用方两次 read 之间停顿（如暂停、包队列已满）不计入，否则停顿超过 ws_timeout 后
     * 第一次 read 会在 service 之前就误报超时，而 socket 里可能已有数据。
     */
    wait_start_us = av_gettime_relative();
    for (;;) {
        if (ijkws_buf_size(ctx->buf) > 0) {
            int n = (int)ijkws_buf_read(ctx->buf, buf, (size_t)size);
            maybe_resume_rx(ctx);
            return n;
        }
        if (ctx->state == IJKWS_STATE_ERROR)
            return ctx->error ? ctx->error : AVERROR(EIO);
        if (ctx->state == IJKWS_STATE_EOF)
            return AVERROR_EOF;
        if (check_interrupt(ctx))
            return AVERROR_EXIT;
        if (av_gettime_relative() - FFMAX(wait_start_us, ctx->last_rx_us) >=
            (int64_t)ctx->timeout_ms * 1000) {
            /* 不改变连接状态：调用方若重试，会再等一个完整的 ws_timeout */
            av_log(NULL, AV_LOG_ERROR, "ijkws: no data for %d ms, read timeout\n", ctx->timeout_ms);
            return AVERROR(ETIMEDOUT);
        }
        maybe_resume_rx(ctx);
        service_once(ctx);
    }
}

/*
 * 优雅关闭：发 close 帧（1000），再 service 到对端回应（CLIENT_CLOSED）或满 IJKWS_CLOSE_WAIT_MS。
 * lws_context_destroy 不发 close 帧（close.c：CONTEXT_DESTROY 直接 just_kill_connection），
 * 所以要先走这一步，让服务端及时回收会话。
 */
static void graceful_close(IjkWsContext *ctx)
{
    int64_t deadline_us;

    if (!ctx->lws || !ctx->wsi || ctx->state != IJKWS_STATE_ESTABLISHED)
        return;

    /* 暂停接收时收不到对端的 close 回应 */
    if (ctx->rx_paused) {
        ctx->rx_paused = 0;
        lws_rx_flow_control(ctx->wsi, 1);
    }
    lws_close_reason(ctx->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
    ctx->close_pending = 1;
    lws_callback_on_writable(ctx->wsi);

    /* 缩短唤醒周期，保证最后一轮 service 不会越过上限太多 */
    ctx->wake_ms = IJKWS_CLOSE_WAKE_MS;
    lws_sul_schedule(ctx->lws, 0, &ctx->wake_sul, wake_cb,
                     (lws_usec_t)ctx->wake_ms * LWS_US_PER_MS);

    deadline_us = av_gettime_relative() + IJKWS_CLOSE_WAIT_MS * 1000;
    while (ctx->wsi && av_gettime_relative() < deadline_us)
        service_once(ctx);
    if (ctx->wsi)
        av_log(NULL, AV_LOG_VERBOSE, "ijkws: no close reply within %d ms\n", IJKWS_CLOSE_WAIT_MS);
}

void ijkws_close(IjkWsContext **pctx)
{
    IjkWsContext *ctx;

    if (!pctx || !*pctx)
        return;
    ctx = *pctx;

    ctx->closing = 1;
    graceful_close(ctx);
    if (ctx->lws) {
        lws_sul_cancel(&ctx->wake_sul);
        lws_context_destroy(ctx->lws);
        ctx->lws = NULL;
    }
    ijkws_buf_free(&ctx->buf);
    free(ctx);
    *pctx = NULL;
}

size_t ijkws_buffered_bytes(const IjkWsContext *ctx)
{
    return ctx ? ijkws_buf_size(ctx->buf) : 0;
}
