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
#include "libavutil/error.h"
#include "libavutil/log.h"

#include "ijkws_buf.h"

#define IJKWS_WAKE_INTERVAL_MS   100
#define IJKWS_DEFAULT_TIMEOUT_MS 10000
#define IJKWS_BUF_INITIAL        (256 * 1024)
/* 每个 lws_context 只有一条连接；限制 fd 表大小，避免按进程 ulimit 分配大数组 */
#define IJKWS_FD_LIMIT           8

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
    int                    closing;     /* ijkws_close 中，不再输出断线日志 */

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
                     IJKWS_WAKE_INTERVAL_MS * LWS_US_PER_MS);
}

static void set_error(IjkWsContext *ctx, int err)
{
    if (ctx->state != IJKWS_STATE_ERROR && ctx->state != IJKWS_STATE_EOF) {
        ctx->state = IJKWS_STATE_ERROR;
        ctx->error = err;
    }
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
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        av_log(NULL, AV_LOG_ERROR, "ijkws: connect %s://%s:%d%s failed: %s\n",
               ctx->scheme, ctx->host, ctx->port, ctx->path,
               in ? (const char *)in : "(unknown)");
        set_error(ctx, AVERROR(EIO));
        ctx->wsi = NULL;
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (len > 0 && ijkws_buf_write(ctx->buf, (const uint8_t *)in, len) < 0) {
            av_log(NULL, AV_LOG_ERROR, "ijkws: out of memory buffering %zu bytes\n", len);
            set_error(ctx, AVERROR(ENOMEM));
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

/* 跑一轮 service；lws 自身出错时把连接标记为 EIO */
static void service_once(IjkWsContext *ctx)
{
    if (lws_service(ctx->lws, 0) < 0)
        set_error(ctx, AVERROR(EIO));
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

    ret = ijkws_resolve(ctx->host, ctx->port, ctx->ip, sizeof(ctx->ip),
                        (int64_t)IJKWS_DEFAULT_TIMEOUT_MS * 1000, &ctx->int_cb);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: resolve %s failed\n", ctx->host);
        goto fail;
    }

    memset(&info, 0, sizeof(info));
    info.port                = CONTEXT_PORT_NO_LISTEN;
    info.protocols           = ws_protocols;
    info.options             = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user                = ctx;
    info.fd_limit_per_thread = IJKWS_FD_LIMIT;
    ctx->lws = lws_create_context(&info);
    if (!ctx->lws) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: lws_create_context failed\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    lws_sul_schedule(ctx->lws, 0, &ctx->wake_sul, wake_cb,
                     IJKWS_WAKE_INTERVAL_MS * LWS_US_PER_MS);

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

    if (!ctx || !buf || size <= 0)
        return AVERROR(EINVAL);

    for (;;) {
        if (ijkws_buf_size(ctx->buf) > 0)
            return (int)ijkws_buf_read(ctx->buf, buf, (size_t)size);
        if (ctx->state == IJKWS_STATE_ERROR)
            return ctx->error ? ctx->error : AVERROR(EIO);
        if (ctx->state == IJKWS_STATE_EOF)
            return AVERROR_EOF;
        if (check_interrupt(ctx))
            return AVERROR_EXIT;
        service_once(ctx);
    }
}

void ijkws_close(IjkWsContext **pctx)
{
    IjkWsContext *ctx;

    if (!pctx || !*pctx)
        return;
    ctx = *pctx;

    ctx->closing = 1;
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
