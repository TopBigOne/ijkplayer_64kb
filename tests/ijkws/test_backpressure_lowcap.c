/*
 * 接收背压的暂停/恢复路径：用极小的水位编译 ijkws.c，让每轮 service 都触发暂停。
 * （默认 4MB/1MB 水位在 service 只由 read 驱动的模型下不会触发，见 test_lifecycle.c）
 * IJKWS_TEST_CFLAGS: -DIJKWS_RX_HIGH_WATER=1024 -DIJKWS_RX_LOW_WATER=512
 */
#include "testutil.h"
#include "libavutil/log.h"

#define WS_BASE "ws://127.0.0.1:18765"
#define FLOOD_TOTAL (20 * 1024 * 1024)

static int g_pauses;

static void count_log(void *avcl, int level, const char *fmt, va_list vl)
{
    if (strstr(fmt, "pause receiving"))
        g_pauses++;
    else if (level <= AV_LOG_WARNING)
        vfprintf(stderr, fmt, vl);
}

/* 每次都暂停、再恢复，数据必须完整且按序（/flood 每条消息 byte[i] = i & 0xFF） */
static void test_pause_resume_keeps_data(void)
{
    IjkWsContext *ctx = NULL;
    uint8_t buf[4096];
    size_t total = 0, i;
    int ret, bad = 0;
    av_log_set_level(AV_LOG_VERBOSE);
    av_log_set_callback(count_log);
    CHECK_EQ_INT(ijkws_open(&ctx, WS_BASE "/flood", NULL, NULL), 0);
    for (;;) {
        ret = ijkws_read(ctx, buf, sizeof(buf));
        if (ret <= 0)
            break;
        for (i = 0; i < (size_t)ret; i++)
            if (buf[i] != (uint8_t)((total + i) % (64 * 1024) & 0xFF))
                bad = 1;
        total += (size_t)ret;
    }
    ijkws_close(&ctx);
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(AV_LOG_INFO);
    fprintf(stderr, "  lowcap flood: ret=%d total=%zu pauses=%d\n", ret, total, g_pauses);
    CHECK_EQ_INT(ret, AVERROR_EOF);
    CHECK_EQ_INT(total, FLOOD_TOTAL);
    CHECK(!bad);
    CHECK(g_pauses > 0);
}

int main(void)
{
    RUN(test_pause_resume_keeps_data);
    return test_finish();
}
