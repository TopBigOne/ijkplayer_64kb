/*
 * ijkws 日志测试（截获 av_log）：
 *   - URL 的 query / userinfo、ws_headers 的内容不出现在任何日志里（query 换成 "?<redacted>"）；
 *   - 用户取消正在进行的连接不输出 ERROR 级日志。
 */
#include <stdarg.h>

#include "testutil.h"
#include "libavutil/dict.h"
#include "libavutil/log.h"

#define WS_BASE "ws://127.0.0.1:18765"

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_log[65536];
static size_t g_log_len;
static int g_log_errors;   /* ERROR 及以上级别的行数 */

static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    char line[4096];
    int n;
    if (level > AV_LOG_VERBOSE)
        return;
    n = vsnprintf(line, sizeof(line), fmt, vl);
    if (n < 0)
        return;
    pthread_mutex_lock(&g_log_mutex);
    if (level <= AV_LOG_ERROR)
        g_log_errors++;
    if (g_log_len + strlen(line) + 8 < sizeof(g_log))
        g_log_len += (size_t)snprintf(g_log + g_log_len, sizeof(g_log) - g_log_len, "[%d] %s", level, line);
    pthread_mutex_unlock(&g_log_mutex);
    fputs(line, stderr);   /* 仍然打印出来，便于排查 */
}

static void log_reset(void)
{
    pthread_mutex_lock(&g_log_mutex);
    g_log[0] = '\0';
    g_log_len = 0;
    g_log_errors = 0;
    pthread_mutex_unlock(&g_log_mutex);
}

static int log_has(const char *s)
{
    int r;
    pthread_mutex_lock(&g_log_mutex);
    r = strstr(g_log, s) != NULL;
    pthread_mutex_unlock(&g_log_mutex);
    return r;
}

static void test_handshake_failure_redacts_query(void)
{
    IjkWsContext *ctx = NULL;
    log_reset();
    CHECK_EQ_INT(ijkws_open(&ctx, WS_BASE "/status/403?token=SECRET1&sig=SECRET1b", NULL, NULL),
                 AVERROR_HTTP_FORBIDDEN);
    CHECK(log_has("ws://127.0.0.1:18765/status/403?<redacted>"));
    CHECK(!log_has("SECRET1"));
}

static void test_invalid_url_redacts_query_and_userinfo(void)
{
    IjkWsContext *ctx = NULL;
    log_reset();
    CHECK_EQ_INT(ijkws_open(&ctx, "http://user:PASSWORD2@example.com/live?token=SECRET2#frag", NULL, NULL),
                 AVERROR(EINVAL));
    CHECK(log_has("invalid url http://example.com/live?<redacted>"));
    CHECK(!log_has("SECRET2"));
    CHECK(!log_has("PASSWORD2"));
    CHECK(!log_has("frag"));
}

/* 10.255.255.1：不可路由或被 TUN 代理挂住，都会停在连接/握手阶段直到 ws_timeout */
static void test_connect_timeout_redacts_query(void)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    int ret;
    log_reset();
    av_dict_set(&opts, "ws_timeout", "500", 0);
    ret = ijkws_open(&ctx, "ws://10.255.255.1/live/a.flv?token=SECRET3", &opts, NULL);
    av_dict_free(&opts);
    CHECK(ret < 0);
    CHECK(log_has("ws://10.255.255.1:80/live/a.flv?<redacted>"));
    CHECK(!log_has("SECRET3"));
}

static void test_malformed_header_not_logged(void)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    int ret;
    log_reset();
    av_dict_set(&opts, "ws_headers", "X-Ok: 1\r\nAuthorization Bearer SECRET4\r\n", 0);
    ret = ijkws_open(&ctx, WS_BASE "/flv", &opts, NULL);
    av_dict_free(&opts);
    CHECK_EQ_INT(ret, AVERROR(EINVAL));
    CHECK(log_has("malformed ws_headers line 2"));
    CHECK(!log_has("SECRET4"));
}

/*
 * 打断正在进行的连接：返回 AVERROR_EXIT，且不打 ERROR 级日志。
 * 打断后 ijkws_close → lws_context_destroy，lws 会对未建立的连接回调 CLIENT_CONNECTION_ERROR
 * （"closed before established" / "Closed before conn"），这不是错误。
 *   - wss 连到明文 ws 端口：TCP 已连上，停在 TLS 握手（服务器只等 HTTP 请求头），确定可复现；
 *   - 10.255.255.1 很早打断：停在 TCP connect（或本机 TUN 代理下停在等握手响应）。
 */
static void cancel_connect_no_error_log(const char *url, int ms)
{
    volatile int flag = 0;
    AVIOInterruptCB cb = { test_interrupt_cb, (void *)&flag };
    IjkWsContext *ctx = NULL;
    int ret, errors;
    log_reset();
    interrupt_after_ms(&flag, ms);
    ret = ijkws_open(&ctx, url, NULL, &cb);
    usleep((useconds_t)(ms + 50) * 1000);   /* 打断线程写完 flag 再离开本函数（flag 在栈上） */
    CHECK_EQ_INT(ret, AVERROR_EXIT);
    pthread_mutex_lock(&g_log_mutex);
    errors = g_log_errors;
    pthread_mutex_unlock(&g_log_mutex);
    if (errors)
        fprintf(stderr, "  %s: %d ERROR line(s)\n", url, errors);
    CHECK_EQ_INT(errors, 0);
    CHECK(!log_has("SECRET5"));
}

static void test_cancel_connect_no_error_log(void)
{
    cancel_connect_no_error_log("wss://127.0.0.1:18765/x?token=SECRET5", 100);
    if (g_test_cur_failed) return;
    cancel_connect_no_error_log("ws://10.255.255.1/x?token=SECRET5", 5);
}

int main(void)
{
    av_log_set_callback(log_cb);
    RUN(test_handshake_failure_redacts_query);
    RUN(test_invalid_url_redacts_query_and_userinfo);
    RUN(test_connect_timeout_redacts_query);
    RUN(test_malformed_header_not_logged);
    RUN(test_cancel_connect_no_error_log);
    return test_finish();
}
