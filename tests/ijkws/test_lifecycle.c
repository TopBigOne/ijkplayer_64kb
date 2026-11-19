/*
 * ijkws 生命周期测试：打断（连接/读取阶段）、限时优雅关闭（发 close 帧）、接收背压。
 */
#include "testutil.h"
#include "libavutil/dict.h"

#define WS_BASE "ws://127.0.0.1:18765"

#define FLOOD_TOTAL (20 * 1024 * 1024)
#define BUF_CAP     (4 * 1024 * 1024)

static int open_opts(IjkWsContext **ctx, const char *url, int timeout_ms, const AVIOInterruptCB *cb)
{
    AVDictionary *opts = NULL;
    char v[32];
    int ret;
    if (timeout_ms > 0) {
        snprintf(v, sizeof(v), "%d", timeout_ms);
        av_dict_set(&opts, "ws_timeout", v, 0);
    }
    ret = ijkws_open(ctx, url, &opts, cb);
    av_dict_free(&opts);
    return ret;
}

static void test_interrupt_during_read(void)
{
    volatile int flag = 0;
    AVIOInterruptCB cb = { test_interrupt_cb, (void *)&flag };
    IjkWsContext *ctx = NULL;
    uint8_t buf[4096];
    int64_t t0, dt;
    int ret;
    CHECK_EQ_INT(open_opts(&ctx, WS_BASE "/silent", 10000, &cb), 0);
    t0 = now_ms();
    interrupt_after_ms(&flag, 300);
    ret = ijkws_read(ctx, buf, sizeof(buf));
    dt = now_ms() - t0;
    ijkws_close(&ctx);
    fprintf(stderr, "  interrupt read: ret=%d %lldms\n", ret, (long long)dt);
    CHECK_EQ_INT(ret, AVERROR_EXIT);
    CHECK(dt >= 300 && dt <= 300 + 200);
}

/*
 * 10.255.255.1 不可路由：正常网络下停在 TCP connect；本机 IPv4 走 TUN 代理时，
 * 代理先接受连接、约 3s 后才断开，此时停在等握手响应。两种情况都应在打断后 ≤ 200ms 返回。
 */
static void test_interrupt_during_connect(void)
{
    volatile int flag = 0;
    AVIOInterruptCB cb = { test_interrupt_cb, (void *)&flag };
    IjkWsContext *ctx = NULL;
    int64_t t0, dt;
    int ret;
    t0 = now_ms();
    interrupt_after_ms(&flag, 100);
    ret = open_opts(&ctx, "ws://10.255.255.1/", 0, &cb);
    dt = now_ms() - t0;
    fprintf(stderr, "  interrupt connect: ret=%d %lldms\n", ret, (long long)dt);
    CHECK_EQ_INT(ret, AVERROR_EXIT);
    CHECK(ctx == NULL);
    CHECK(dt <= 300);
}

/* close 后在服务器日志里找该连接的 close 帧：码必须是 1000，且带掩码（RFC 6455 §5.3） */
static void expect_close_frame_logged(const char *query)
{
    char line[4096];
    int conn = log_conn_for_query(query);
    CHECK(conn > 0);
    if (!log_wait_event(conn, "close_received", 2000, line, sizeof(line))) {
        fprintf(stderr, "  %s: no close_received in server log\n", query);
        CHECK(!"close frame not received");
    }
    if (!strstr(line, "\"code\": 1000,") || !strstr(line, "\"masked\": true")) {
        fprintf(stderr, "  %s: %s", query, line);
        CHECK(!"close frame code/mask mismatch");
    }
}

/* 打开 path（带唯一 query），读 read_bytes 字节后 close；返回 close 耗时，失败返回 -1 */
static int64_t open_read_close(const char *path, const char *query, size_t read_bytes)
{
    char url[256];
    IjkWsContext *ctx = NULL;
    uint8_t buf[32768];
    size_t got = 0;
    int64_t t0;
    int ret;
    snprintf(url, sizeof(url), WS_BASE "%s?%s", path, query);
    if (ijkws_open(&ctx, url, NULL, NULL) < 0)
        return -1;
    while (got < read_bytes) {
        size_t want = read_bytes - got;
        ret = ijkws_read(ctx, buf, (int)(want < sizeof(buf) ? want : sizeof(buf)));
        if (ret <= 0) {
            ijkws_close(&ctx);
            return -1;
        }
        got += (size_t)ret;
    }
    t0 = now_ms();
    ijkws_close(&ctx);
    return ctx == NULL ? now_ms() - t0 : -1;
}

static void test_close_sends_close_frame(void)
{
    char query[64];
    int64_t dt;

    /* /flv：读 100KB 后关闭 */
    snprintf(query, sizeof(query), "closeflv=%lld", (long long)now_ms());
    dt = open_read_close("/flv", query, 100 * 1024);
    fprintf(stderr, "  close /flv: %lldms\n", (long long)dt);
    CHECK(dt >= 0 && dt <= 150);
    expect_close_frame_logged(query);
    if (g_test_cur_failed)
        return;

    /*
     * /silent：服务器自己从不发 close 帧，因此日志里的 close 一定是 ijkws_close 主动发出的，
     * 而不是 lws 对服务器 close 的自动回复（/flv 上两者都可能出现）。
     */
    snprintf(query, sizeof(query), "closesilent=%lld", (long long)now_ms());
    dt = open_read_close("/silent", query, 0);
    fprintf(stderr, "  close /silent: %lldms\n", (long long)dt);
    CHECK(dt >= 0 && dt <= 150);
    expect_close_frame_logged(query);
}

/* 服务器收到 close 后既不回复也不断开：close 仍须在上限内返回 */
static void test_close_bounded_when_peer_silent(void)
{
    char query[64];
    int64_t dt;
    snprintf(query, sizeof(query), "closepeersilent=%lld", (long long)now_ms());
    dt = open_read_close("/close-silent", query, 4096);
    fprintf(stderr, "  close /close-silent: %lldms\n", (long long)dt);
    CHECK(dt >= 0 && dt <= 150);
    /* close 帧照样发出了，只是没有回应 */
    expect_close_frame_logged(query);
}

static void test_close_null_safe(void)
{
    IjkWsContext *c = NULL;
    ijkws_close(NULL);
    ijkws_close(&c);
    CHECK(c == NULL);
}

static void test_backpressure_caps_buffer(void)
{
    IjkWsContext *ctx = NULL;
    uint8_t buf[4096];
    size_t total = 0, maxbuf = 0, b;
    int ret;
    int64_t t0 = now_ms();
    CHECK_EQ_INT(ijkws_open(&ctx, WS_BASE "/flood", NULL, NULL), 0);
    for (;;) {
        ret = ijkws_read(ctx, buf, sizeof(buf));
        b = ijkws_buffered_bytes(ctx);
        if (b > maxbuf)
            maxbuf = b;
        if (b > BUF_CAP + 64 * 1024)
            break;
        if (ret <= 0)
            break;
        total += (size_t)ret;
        usleep(1000);
    }
    ijkws_close(&ctx);
    fprintf(stderr, "  flood: ret=%d total=%zu max_buffered=%zu %lldms\n",
            ret, total, maxbuf, (long long)(now_ms() - t0));
    CHECK(maxbuf <= BUF_CAP + 64 * 1024);
    CHECK_EQ_INT(ret, AVERROR_EOF);
    CHECK_EQ_INT(total, FLOOD_TOTAL);
}

int main(void)
{
    RUN(test_interrupt_during_read);
    RUN(test_interrupt_during_connect);
    RUN(test_close_sends_close_frame);
    RUN(test_close_bounded_when_peer_silent);
    RUN(test_close_null_safe);
    RUN(test_backpressure_caps_buffer);
    return test_finish();
}
