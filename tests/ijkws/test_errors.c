/*
 * ijkws 失败路径测试：握手 HTTP 状态码映射、连接被拒、连接/读超时、无 close 帧断线、非法 URL。
 */
#include <errno.h>

#include "testutil.h"
#include "libavutil/dict.h"

#define WS_BASE "ws://127.0.0.1:18765"

static int open_with_timeout(IjkWsContext **ctx, const char *url, int timeout_ms)
{
    AVDictionary *opts = NULL;
    int ret;
    char v[32];
    snprintf(v, sizeof(v), "%d", timeout_ms);
    av_dict_set(&opts, "ws_timeout", v, 0);
    ret = ijkws_open(ctx, url, &opts, NULL);
    /* ws_timeout 已被消费 */
    if (av_dict_get(opts, "ws_timeout", NULL, 0)) {
        fprintf(stderr, "  ws_timeout not removed from opts\n");
        ret = -12345;
    }
    av_dict_free(&opts);
    return ret;
}

static void expect_status(int code, int expect)
{
    char url[256];
    IjkWsContext *ctx = NULL;
    snprintf(url, sizeof(url), WS_BASE "/status/%d", code);
    CHECK_EQ_INT(ijkws_open(&ctx, url, NULL, NULL), expect);
    CHECK(ctx == NULL);
}

static void test_handshake_http_status_mapped(void)
{
    expect_status(401, AVERROR_HTTP_UNAUTHORIZED);
    if (g_test_cur_failed) return;
    expect_status(403, AVERROR_HTTP_FORBIDDEN);
    if (g_test_cur_failed) return;
    expect_status(404, AVERROR_HTTP_NOT_FOUND);
    if (g_test_cur_failed) return;
    expect_status(500, AVERROR_HTTP_SERVER_ERROR);
    if (g_test_cur_failed) return;
    /* 规则延伸：其他 4xx / 5xx */
    expect_status(418, AVERROR_HTTP_OTHER_4XX);
    if (g_test_cur_failed) return;
    expect_status(503, AVERROR_HTTP_SERVER_ERROR);
}

static void test_connection_refused(void)
{
    IjkWsContext *ctx = NULL;
    int64_t t0 = now_ms(), dt;
    int ret = ijkws_open(&ctx, "ws://127.0.0.1:18799/", NULL, NULL);
    dt = now_ms() - t0;
    fprintf(stderr, "  refused: ret=%d %lldms\n", ret, (long long)dt);
    CHECK(ret < 0);
    CHECK(ctx == NULL);
    CHECK(dt < 1000);
}

static void test_connect_timeout(void)
{
    IjkWsContext *ctx = NULL;
    int64_t t0 = now_ms(), dt;
    int ret = open_with_timeout(&ctx, "ws://10.255.255.1/", 500);
    dt = now_ms() - t0;
    fprintf(stderr, "  connect timeout: ret=%d %lldms\n", ret, (long long)dt);
    CHECK_EQ_INT(ret, AVERROR(ETIMEDOUT));
    CHECK(ctx == NULL);
    CHECK(dt >= 450 && dt <= 800);
}

static void test_silent_server_read_timeout(void)
{
    IjkWsContext *ctx = NULL;
    uint8_t buf[4096];
    int64_t t0, dt;
    int ret;
    CHECK_EQ_INT(open_with_timeout(&ctx, WS_BASE "/silent", 500), 0);
    CHECK(ctx != NULL);
    t0 = now_ms();
    ret = ijkws_read(ctx, buf, sizeof(buf));
    dt = now_ms() - t0;
    ijkws_close(&ctx);
    fprintf(stderr, "  read timeout: ret=%d %lldms\n", ret, (long long)dt);
    CHECK_EQ_INT(ret, AVERROR(ETIMEDOUT));
    CHECK(dt >= 450 && dt <= 800);
}

static void test_drop_without_close_is_eio(void)
{
    IjkWsContext *ctx = NULL;
    uint8_t *got = NULL;
    size_t got_len = 0;
    int ret;
    CHECK_EQ_INT(ijkws_open(&ctx, WS_BASE "/drop", NULL, NULL), 0);
    ret = read_all(ctx, &got, &got_len);
    ijkws_close(&ctx);
    free(got);
    CHECK_EQ_INT(got_len, 64 * 1024);
    CHECK_EQ_INT(ret, AVERROR(EIO));
}

static void test_bad_url_einval(void)
{
    IjkWsContext *ctx = NULL;
    CHECK_EQ_INT(ijkws_open(&ctx, "ws://", NULL, NULL), AVERROR(EINVAL));
    CHECK(ctx == NULL);
    CHECK_EQ_INT(ijkws_open(&ctx, "http://x/", NULL, NULL), AVERROR(EINVAL));
    CHECK(ctx == NULL);
}

int main(void)
{
    RUN(test_handshake_http_status_mapped);
    RUN(test_connection_refused);
    RUN(test_connect_timeout);
    RUN(test_silent_server_read_timeout);
    RUN(test_drop_without_close_is_eio);
    RUN(test_bad_url_einval);
    return test_finish();
}
