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

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libwebsockets.h>
#if defined(LWS_OPENSSL_SUPPORT) && !defined(LWS_WITH_MBEDTLS)
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#define IJKWS_HAVE_OPENSSL 1
#endif

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
/*
 * lws 每线程的收发缓冲；握手请求（请求行 + 全部 Header）在其中拼装，默认 4096 放不下
 * 带长 token 的 URL 加上 ws_headers，因此放大。
 */
#define IJKWS_SERV_BUF_SIZE      (16 * 1024)
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
    int                    host_is_ip;  /* URL 主机是 IPv4/IPv6 字面量 */

    /* 握手选项（ws_headers / user_agent / ws_origin / ws_protocol），未给出时为 NULL */
    char                  *headers;
    char                  *user_agent;
    char                  *origin;
    char                  *protocol;
    /* ws_tls_verify / ws_ca_file */
    int                    tls_verify;
    char                  *ca_file;
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

/* 往握手请求追加一行 "name: value\r\n"（name 为 NULL 时 value 已是完整的一行）；空间不够返回 -1 */
static int append_header(char **p, char *end, const char *name, const char *value, size_t value_len)
{
    int n;
    if (name)
        n = snprintf(*p, (size_t)(end - *p), "%s: %.*s\r\n", name, (int)value_len, value);
    else
        n = snprintf(*p, (size_t)(end - *p), "%.*s\r\n", (int)value_len, value);
    if (n < 0 || n >= end - *p)
        return -1;
    *p += n;
    return 0;
}

/*
 * LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER：追加 User-Agent 和 ws_headers。
 * ws_headers 按 "\r\n"（也接受单独的 "\n"）分行，跳过空行；没有 ':' 的行在 parse_options 里已被拒绝。
 */
static int append_handshake_headers(IjkWsContext *ctx, char **p, size_t len)
{
    char *end = *p + len;
    const char *s;

    if (ctx->user_agent && append_header(p, end, "User-Agent", ctx->user_agent, strlen(ctx->user_agent)) < 0)
        goto overflow;
    for (s = ctx->headers; s && *s; ) {
        size_t n = strcspn(s, "\r\n");
        if (n > 0 && append_header(p, end, NULL, s, n) < 0)
            goto overflow;
        s += n;
        s += strspn(s, "\r\n");
    }
    return 0;

overflow:
    av_log(NULL, AV_LOG_ERROR, "ijkws: handshake headers too long (limit %d bytes)\n", IJKWS_SERV_BUF_SIZE);
    return -1;
}

#ifdef IJKWS_HAVE_OPENSSL
/*
 * IP 字面量的 wss（只在 ctx->host_is_ip 时注册 SSL_CB_HANDSHAKE_START）。
 *
 * lws 用 connect_info.host（即 Host 头，"[v6]:port" / "v4:port"）在第一个 ':' 处截断后，
 * 无条件 SSL_set_tlsext_host_name，并在需要校验主机名时先试 X509_VERIFY_PARAM_set1_ip_asc、
 * 失败再按 DNS 名校验（lib/tls/openssl/openssl-client.c lws_ssl_client_bio_create）。
 * 结果：IPv4 会发出 SNI "1.2.3.4"，IPv6 会发出 SNI "["、并拿 "[" 当主机名校验。
 * lws v4.3 没有单独设置 SNI 的字段或标志，因此在握手开始（ClientHello 生成之前）修正：
 *   - 清掉 SNI：RFC 6066 §3 规定 SNI 不能是 IP 字面量；
 *   - 校验模式下把主机名校验换成对 URL 中 IP 的校验（证书 SAN 的 iPAddress）。
 */
static int fix_tls_for_ip_literal(IjkWsContext *ctx, struct lws *wsi)
{
    SSL *ssl = lws_get_ssl(wsi);

    if (!ssl)
        return 0;
    SSL_set_tlsext_host_name(ssl, NULL);
    if (ctx->tls_verify) {
        X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
        /*
         * 先设 IP、成功后再清掉 lws 设的主机名：失败时保留原来的（必然不匹配的）主机名校验，
         * 这样在 lws 异步断开之前，握手也不会在没有任何名称约束的情况下通过。
         */
        if (!X509_VERIFY_PARAM_set1_ip_asc(param, ctx->host)) {
            av_log(NULL, AV_LOG_ERROR, "ijkws: cannot verify certificate against IP %s\n", ctx->host);
            return -1;
        }
        X509_VERIFY_PARAM_set1_host(param, NULL, 0);
    }
    return 0;
}
#endif

static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                       void *user, void *in, size_t len)
{
    IjkWsContext *ctx = (IjkWsContext *)lws_context_user(lws_get_context(wsi));

    if (!ctx)
        return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        return append_handshake_headers(ctx, (char **)in, len);

#ifdef IJKWS_HAVE_OPENSSL
    case LWS_CALLBACK_SSL_INFO: {
        const struct lws_ssl_info *si = (const struct lws_ssl_info *)in;
        if (ctx->host_is_ip && si && (si->where & SSL_CB_HANDSHAKE_START))
            return fix_tls_for_ip_literal(ctx, wsi);
        break;
    }
#endif

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

/*
 * 把 opts 中 key 的值复制到 *dst（av_strdup，调用方释放；不存在时为 NULL），del 非 0 时从 opts 删除。
 * 复制失败返回 AVERROR(ENOMEM)，此时也照样删除。
 */
static int take_option(AVDictionary **opts, const char *key, char **dst, int del)
{
    AVDictionaryEntry *e = av_dict_get(*opts, key, NULL, 0);
    *dst = NULL;
    if (!e)
        return 0;
    *dst = av_strdup(e->value);
    if (del)
        av_dict_set(opts, key, NULL, 0);
    return *dst ? 0 : AVERROR(ENOMEM);
}

/* 握手 Header 的值不能含 CR/LF，否则可以注入任意 Header */
static int has_crlf(const char *s)
{
    return s && strpbrk(s, "\r\n") != NULL;
}

/*
 * 读取 ws_* 选项并从 opts 中删除（无论成功与否都删，避免 avformat_open_input 报 "Option not found"）；
 * user_agent 与 FFmpeg http 共用，只读不删。
 * 选项非法时返回 AVERROR(EINVAL)（ws_timeout / ws_tls_verify 非法只告警，用默认值）。
 */
static int parse_options(IjkWsContext *ctx, AVDictionary **opts)
{
    char *timeout = NULL, *verify = NULL;
    int ret = 0;

    ctx->timeout_ms = IJKWS_DEFAULT_TIMEOUT_MS;
    if (!opts)
        return 0;

    /* 先全部取出（并删除），再校验：任何一步失败都不能把 ws_* 留在 opts 里 */
    ret |= take_option(opts, "ws_timeout",    &timeout,         1);
    ret |= take_option(opts, "ws_tls_verify", &verify,          1);
    ret |= take_option(opts, "ws_headers",    &ctx->headers,    1);
    ret |= take_option(opts, "ws_origin",     &ctx->origin,     1);
    ret |= take_option(opts, "ws_protocol",   &ctx->protocol,   1);
    ret |= take_option(opts, "ws_ca_file",    &ctx->ca_file,    1);
    ret |= take_option(opts, "user_agent",    &ctx->user_agent, 0);
    if (ret < 0) {
        av_freep(&timeout);
        av_freep(&verify);
        return AVERROR(ENOMEM);
    }

    if (timeout) {
        char *end = NULL;
        long v = strtol(timeout, &end, 10);
        if (end && end != timeout && *end == '\0' && v > 0 && v <= INT32_MAX)
            ctx->timeout_ms = (int)v;
        else
            av_log(NULL, AV_LOG_WARNING, "ijkws: invalid ws_timeout '%s', using %d ms\n",
                   timeout, IJKWS_DEFAULT_TIMEOUT_MS);
    }
    if (verify) {
        if (!strcmp(verify, "0") || !strcmp(verify, "1"))
            ctx->tls_verify = verify[0] == '1';
        else
            av_log(NULL, AV_LOG_WARNING, "ijkws: invalid ws_tls_verify '%s', using 0\n", verify);
    }
    av_freep(&timeout);
    av_freep(&verify);

    /* 空字符串等同于未设置 */
    if (ctx->headers && !ctx->headers[0])       av_freep(&ctx->headers);
    if (ctx->user_agent && !ctx->user_agent[0]) av_freep(&ctx->user_agent);
    if (ctx->origin && !ctx->origin[0])         av_freep(&ctx->origin);
    if (ctx->protocol && !ctx->protocol[0])     av_freep(&ctx->protocol);
    if (ctx->ca_file && !ctx->ca_file[0])       av_freep(&ctx->ca_file);

    if (has_crlf(ctx->user_agent) || has_crlf(ctx->origin) || has_crlf(ctx->protocol)) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: user_agent / ws_origin / ws_protocol must not contain CR/LF\n");
        ret = AVERROR(EINVAL);
    }
    if (ctx->headers) {
        const char *s = ctx->headers;
        while (*s) {
            size_t n = strcspn(s, "\r\n");
            const char *colon = memchr(s, ':', n);
            if (n > 0 && (!colon || colon == s)) {
                av_log(NULL, AV_LOG_ERROR, "ijkws: malformed ws_headers line '%.*s'\n", (int)n, s);
                ret = AVERROR(EINVAL);
                break;
            }
            s += n;
            s += strspn(s, "\r\n");
        }
    }
    return ret;
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
     * wss 的 SNI 也取自 connect_info.host，lws 会在第一个 ':' 处截断：主机名时 SNI 是裸主机名；
     * IP 字面量时由 fix_tls_for_ip_literal 去掉 SNI。
     */
    {
        int is_default = port == (av_strcasecmp(ctx->scheme, "wss") ? 80 : 443);
        int is_ipv6 = strchr(ctx->host, ':') != NULL;   /* 主机名不可能含 ':' */
        struct in_addr a4;
        ctx->host_is_ip = is_ipv6 || inet_pton(AF_INET, ctx->host, &a4) == 1;
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

    ret = parse_options(ctx, opts);
    if (ret < 0)
        goto fail;
    /* 连接超时从 open 开始计时，覆盖 DNS、TCP、TLS 和 WebSocket 握手 */
    deadline_us = av_gettime_relative() + (int64_t)ctx->timeout_ms * 1000;

    ret = parse_url(ctx, url);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: invalid url %s\n", url);
        goto fail;
    }
    is_wss = !av_strcasecmp(ctx->scheme, "wss");

    /*
     * 证书校验只对 wss 有意义。设备上没有 OpenSSL 可用的系统 CA 文件，
     * 所以校验模式必须由 App 提供 CA bundle；提前检查可读，给出明确的错误。
     */
    if (is_wss && ctx->tls_verify) {
        if (!ctx->ca_file) {
            av_log(NULL, AV_LOG_ERROR,
                   "ijkws: ws_tls_verify=1 requires ws_ca_file (path to a PEM CA bundle)\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (access(ctx->ca_file, R_OK) != 0) {
            ret = AVERROR(errno);
            av_log(NULL, AV_LOG_ERROR, "ijkws: cannot read ws_ca_file %s: %s\n",
                   ctx->ca_file, av_err2str(ret));
            goto fail;
        }
    }

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
    info.pt_serv_buf_size     = IJKWS_SERV_BUF_SIZE;
    /*
     * 不开 LWS_SERVER_OPTION_VALIDATE_UTF8：文本帧里也可能是二进制 FLV。
     * ws_origin 原样发送（lws 默认会改写成 "http(s)://<origin>"，client-http.c lws_generate_client_handshake）。
     */
    if (ctx->origin)
        info.options |= LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
    if (is_wss && ctx->tls_verify) {
        /*
         * client_ssl_ca_filepath 在 lws_context_creation_info 中；lws_create_context 用同一份 info
         * 创建默认 vhost（context.c），vhost 创建时 lws_context_init_client_ssl 取它（优先于
         * ssl_ca_filepath，tls-client.c），由 lws_tls_client_create_vhost_context 加载到该 vhost 的
         * 客户端 SSL_CTX（openssl-client.c，SSL_CTX_load_verify_file/locations）。
         * 同时关掉 OS CA（LWS_SSL_CLIENT_USE_OS_CA_CERTS 下默认会 set_default_verify_paths），
         * 只信任 ws_ca_file，主机与设备行为一致。
         */
        info.client_ssl_ca_filepath = ctx->ca_file;
        info.options |= LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS;
    }
#ifdef IJKWS_HAVE_OPENSSL
    /* IP 字面量：握手开始时去掉 SNI、改为按 IP 校验（见 fix_tls_for_ip_literal） */
    if (is_wss && ctx->host_is_ip)
        info.ssl_info_event_mask = SSL_CB_HANDSHAKE_START;
#endif
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
    /*
     * Origin：RFC 6455 §4.1 只要求浏览器发送，非浏览器客户端可以不带。未给 ws_origin 时不发
     * （lws 只在 origin 非 NULL 时才写 Origin 头），避免伪造一个与页面无关的来源。
     */
    ci.origin   = ctx->origin;
    /* Sec-WebSocket-Protocol；服务器回的子协议必须在其中（client-ws.c），回调仍绑定到本地的 "ijkws" */
    ci.protocol = ctx->protocol;
    ci.local_protocol_name = ws_protocols[0].name;
    ci.pwsi     = &ctx->wsi;
    if (is_wss && ctx->tls_verify) {
        /* 严格校验：证书链（只信任 ws_ca_file）+ 主机名（IP 字面量按 IP） */
        ci.ssl_connection = LCCSCF_USE_SSL;
    } else if (is_wss) {
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
    av_freep(&ctx->headers);
    av_freep(&ctx->user_agent);
    av_freep(&ctx->origin);
    av_freep(&ctx->protocol);
    av_freep(&ctx->ca_file);
    free(ctx);
    *pctx = NULL;
}

size_t ijkws_buffered_bytes(const IjkWsContext *ctx)
{
    return ctx ? ijkws_buf_size(ctx->buf) : 0;
}
