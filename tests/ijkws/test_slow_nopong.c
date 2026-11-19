/*
 * 长时间测试（约 5.5 分钟），只在 IJKWS_SLOW_TESTS=1 时运行，否则跳过（计数 0/0）：
 *   IJKWS_SLOW_TESTS=1 tests/ijkws/run.sh test_slow_nopong
 *
 * 服务器 /nopong 持续慢速推流 330s 且从不回复 PING。lws 默认的 validity 策略会在距上次 PONG 300s 时
 * 发 PING、310s 仍无 PONG 就断开（数据帧不算“有效”）；ijkws 给连接设置了关闭 validity 的
 * retry_and_idle_policy，因此这条流必须完整读到 EOF，且服务器不应收到任何 PING。
 */
#include "testutil.h"

#ifndef WS_BASE
#define WS_BASE   "ws://127.0.0.1:18765"
#endif
#define NOPONG_SECS 330

static void test_no_validity_hangup(void)
{
    IjkWsContext *ctx = NULL;
    uint8_t *data = NULL;
    size_t len = 0;
    char url[128], line[4096];
    int64_t t0, dt;
    int ret, conn;

    /* id 区分连接（日志里按 query 找连接号，避免与同一服务器上的其他运行混淆） */
    snprintf(url, sizeof(url), WS_BASE "/nopong?secs=%d&id=%d", NOPONG_SECS, (int)getpid());
    t0 = now_ms();
    CHECK_EQ_INT(ijkws_open(&ctx, url, NULL, NULL), 0);
    ret = read_all(ctx, &data, &len);
    dt = now_ms() - t0;
    ijkws_close(&ctx);
    free(data);
    fprintf(stderr, "  nopong: ret=%d after %lld ms, %zu bytes\n", ret, (long long)dt, len);
    CHECK_EQ_INT(ret, AVERROR_EOF);
    CHECK(dt >= (NOPONG_SECS - 5) * 1000LL);

    snprintf(url, sizeof(url), "secs=%d&id=%d", NOPONG_SECS, (int)getpid());
    conn = log_conn_for_query(url);
    CHECK(conn > 0);
    if (log_wait_event(conn, "ping_received", 0, line, sizeof(line))) {
        fprintf(stderr, "  unexpected ping: %s", line);
        CHECK(!"client sent a validity PING");
    }
}

int main(void)
{
    const char *slow = getenv("IJKWS_SLOW_TESTS");
    if (!slow || strcmp(slow, "1")) {
        printf("[SKIP] test_no_validity_hangup (set IJKWS_SLOW_TESTS=1)\n");
        return test_finish();
    }
    RUN(test_no_validity_hangup);
    return test_finish();
}
