/*
 * ijkws 正常收流测试：ws/wss、各种消息切分方式，以及 ijkws_buf 的环绕/扩容。
 */
#include "testutil.h"
#include "ijkws_buf.h"

#define WS_BASE  "ws://127.0.0.1:18765"
#define WSS_BASE "wss://localhost:18766"

/* 打开 url，读到结束，结果必须是 expect 且最后返回 AVERROR_EOF */
static void stream_expect(const char *url, const uint8_t *expect, size_t expect_len)
{
    IjkWsContext *ctx = NULL;
    uint8_t *got = NULL;
    size_t got_len = 0;
    int ret = ijkws_open(&ctx, url, NULL, NULL);
    CHECK_EQ_INT(ret, 0);
    CHECK(ctx != NULL);
    ret = read_all(ctx, &got, &got_len);
    ijkws_close(&ctx);
    CHECK(ctx == NULL);
    if (got_len != expect_len || memcmp(got, expect, expect_len) != 0) {
        size_t i = 0;
        while (i < got_len && i < expect_len && got[i] == expect[i])
            i++;
        fprintf(stderr, "  %s: got %zu bytes, expect %zu, first diff at %zu\n", url, got_len, expect_len, i);
        free(got);
        CHECK(!"bytes differ");
    }
    free(got);
    CHECK_EQ_INT(ret, AVERROR_EOF);
}

static void stream_expect_flv(const char *url)
{
    char path[1024];
    uint8_t *flv = NULL;
    size_t flv_len = 0;
    CHECK_EQ_INT(load_file(test_path(path, sizeof(path), "test.flv"), &flv, &flv_len), 0);
    CHECK(flv_len > 0);
    stream_expect(url, flv, flv_len);
    free(flv);
}

static void test_ws_stream_bytes_equal(void)          { stream_expect_flv(WS_BASE "/flv"); }
static void test_wss_self_signed_default_ok(void)     { stream_expect_flv(WSS_BASE "/flv"); }
static void test_chunked_messages_bytes_equal(void)   { stream_expect_flv(WS_BASE "/chunked"); }
static void test_fragmented_messages_bytes_equal(void){ stream_expect_flv(WS_BASE "/fragmented"); }
static void test_text_frames_accepted(void)           { stream_expect_flv(WS_BASE "/text"); }
static void test_ping_interleaved_bytes_equal(void)   { stream_expect_flv(WS_BASE "/ping"); }

static void test_large_message_1mb(void)
{
    const size_t prefix = 1024 * 1024;
    char path[1024];
    uint8_t *flv = NULL, *expect;
    size_t flv_len = 0, i;
    CHECK_EQ_INT(load_file(test_path(path, sizeof(path), "test.flv"), &flv, &flv_len), 0);
    expect = malloc(prefix + flv_len);
    /* 与 ws_test_server.py 的 big_prefix() 一致 */
    for (i = 0; i < prefix; i++)
        expect[i] = (uint8_t)(i % 251);
    memcpy(expect + prefix, flv, flv_len);
    free(flv);
    stream_expect(WS_BASE "/big", expect, prefix + flv_len);
    free(expect);
}

static void test_buf_wraparound(void)
{
    enum { ROUNDS = 1000, WR = 3000, RD = 2000 };
    IjkWsBuf *b = ijkws_buf_alloc(4096);
    uint8_t *ref = malloc((size_t)ROUNDS * WR);
    uint8_t wbuf[WR], rbuf[WR];
    size_t wpos = 0, rpos = 0, n;
    int r, i;
    uint32_t seed = 12345;
    CHECK(b != NULL);
    for (i = 0; i < ROUNDS * WR; i++) {
        seed = seed * 1103515245u + 12345u;
        ref[i] = (uint8_t)(seed >> 16);
    }
    for (r = 0; r < ROUNDS; r++) {
        memcpy(wbuf, ref + wpos, WR);
        CHECK_EQ_INT(ijkws_buf_write(b, wbuf, WR), 0);
        wpos += WR;
        CHECK_EQ_INT(ijkws_buf_size(b), wpos - rpos);
        n = ijkws_buf_read(b, rbuf, RD);
        CHECK_EQ_INT(n, RD);
        CHECK(memcmp(rbuf, ref + rpos, RD) == 0);
        rpos += RD;
    }
    while ((n = ijkws_buf_read(b, rbuf, WR)) > 0) {
        CHECK(memcmp(rbuf, ref + rpos, n) == 0);
        rpos += n;
    }
    CHECK_EQ_INT(rpos, wpos);
    CHECK_EQ_INT(ijkws_buf_size(b), 0);
    ijkws_buf_free(&b);
    CHECK(b == NULL);
    free(ref);
}

int main(void)
{
    RUN(test_ws_stream_bytes_equal);
    RUN(test_wss_self_signed_default_ok);
    RUN(test_chunked_messages_bytes_equal);
    RUN(test_fragmented_messages_bytes_equal);
    RUN(test_text_frames_accepted);
    RUN(test_ping_interleaved_bytes_equal);
    RUN(test_large_message_1mb);
    RUN(test_buf_wraparound);
    return test_finish();
}
