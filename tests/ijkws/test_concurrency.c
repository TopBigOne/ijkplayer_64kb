/*
 * ijkws 并发测试：模拟多路播放器（每个 IjkWsContext 独占一个 lws_context，各自在自己的线程里驱动）。
 *   - 4 线程同时 open /flv、读到 EOF、close，重复 5 轮：每路数据都必须与 test.flv 完全一致；
 *   - 4 线程同时 open /silent，各自在随机 50~300ms 后被打断再 close，重复 3 轮：
 *     都必须返回 AVERROR_EXIT（打断可能落在 open 或 read 阶段），且不崩溃。
 */
#include "testutil.h"

#define WS_BASE   "ws://127.0.0.1:18765"
#define NTHREADS  4
#define FLV_ROUNDS 5
#define INT_ROUNDS 3

typedef struct {
    int      idx;
    int      round;
    /* 结果 */
    int      open_ret;
    int      last_ret;
    uint8_t *data;
    size_t   len;
} FlvJob;

static void *flv_worker(void *arg)
{
    FlvJob *j = arg;
    IjkWsContext *ctx = NULL;
    char url[128];
    /* query 只用来区分连接，服务器按 path 选择行为 */
    snprintf(url, sizeof(url), WS_BASE "/flv?conc=r%dt%d", j->round, j->idx);
    j->open_ret = ijkws_open(&ctx, url, NULL, NULL);
    if (j->open_ret == 0) {
        j->last_ret = read_all(ctx, &j->data, &j->len);
        ijkws_close(&ctx);
    }
    return NULL;
}

static void test_concurrent_flv(void)
{
    char path[1024];
    uint8_t *ref = NULL;
    size_t ref_len = 0;
    int r, i;

    CHECK_EQ_INT(load_file(test_path(path, sizeof(path), "test.flv"), &ref, &ref_len), 0);
    for (r = 0; r < FLV_ROUNDS; r++) {
        pthread_t th[NTHREADS];
        FlvJob jobs[NTHREADS];
        int bad = 0;
        memset(jobs, 0, sizeof(jobs));
        for (i = 0; i < NTHREADS; i++) {
            jobs[i].idx = i;
            jobs[i].round = r;
            pthread_create(&th[i], NULL, flv_worker, &jobs[i]);
        }
        for (i = 0; i < NTHREADS; i++)
            pthread_join(th[i], NULL);
        for (i = 0; i < NTHREADS; i++) {
            FlvJob *j = &jobs[i];
            if (j->open_ret != 0 || j->last_ret != AVERROR_EOF || j->len != ref_len ||
                memcmp(j->data, ref, ref_len) != 0) {
                fprintf(stderr, "  round %d thread %d: open=%d last=%d len=%zu (want %zu)\n",
                        r, i, j->open_ret, j->last_ret, j->len, ref_len);
                bad = 1;
            }
            free(j->data);
        }
        if (bad) {
            free(ref);
            CHECK(!"concurrent flv mismatch");
        }
    }
    fprintf(stderr, "  %d rounds x %d threads, %zu bytes each: all equal\n", FLV_ROUNDS, NTHREADS, ref_len);
    free(ref);
}

typedef struct {
    int          idx;
    int          round;
    int          delay_ms;
    volatile int flag;
    /* 结果 */
    int          open_ret;
    int          read_ret;
    int64_t      dt_ms;
} IntJob;

static void *int_worker(void *arg)
{
    IntJob *j = arg;
    AVIOInterruptCB cb = { test_interrupt_cb, (void *)&j->flag };
    IjkWsContext *ctx = NULL;
    char url[128];
    uint8_t buf[4096];
    int64_t t0 = now_ms();

    snprintf(url, sizeof(url), WS_BASE "/silent?conc=int-r%dt%d", j->round, j->idx);
    interrupt_after_ms(&j->flag, j->delay_ms);
    j->open_ret = ijkws_open(&ctx, url, NULL, &cb);
    j->read_ret = 0;
    if (j->open_ret == 0) {
        j->read_ret = ijkws_read(ctx, buf, sizeof(buf));
        ijkws_close(&ctx);
    }
    j->dt_ms = now_ms() - t0;
    return NULL;
}

/* 静态存储：打断线程是 detached 的，万一 worker 提前失败返回，它稍后写 flag 也不会写到已失效的栈上 */
static IntJob g_int_jobs[INT_ROUNDS][NTHREADS];

static void test_concurrent_interrupt(void)
{
    int r, i;
    srand((unsigned)time(NULL));
    for (r = 0; r < INT_ROUNDS; r++) {
        pthread_t th[NTHREADS];
        IntJob *jobs = g_int_jobs[r];
        int bad = 0;
        for (i = 0; i < NTHREADS; i++) {
            jobs[i].idx = i;
            jobs[i].round = r;
            jobs[i].delay_ms = 50 + rand() % 251;
            pthread_create(&th[i], NULL, int_worker, &jobs[i]);
        }
        for (i = 0; i < NTHREADS; i++)
            pthread_join(th[i], NULL);
        for (i = 0; i < NTHREADS; i++) {
            IntJob *j = &jobs[i];
            /* 打断落在 open 阶段：open 返回 EXIT；落在 read 阶段：read 返回 EXIT */
            int ok = j->open_ret == AVERROR_EXIT || (j->open_ret == 0 && j->read_ret == AVERROR_EXIT);
            /* 打断后 ≤ 100ms 唤醒 + 100ms 优雅关闭上限，留余量 */
            if (!ok || j->dt_ms > j->delay_ms + 400) {
                fprintf(stderr, "  round %d thread %d: delay=%d open=%d read=%d dt=%lldms\n",
                        r, i, j->delay_ms, j->open_ret, j->read_ret, (long long)j->dt_ms);
                bad = 1;
            }
        }
        CHECK(!bad);
    }
    fprintf(stderr, "  %d rounds x %d threads interrupted: all AVERROR_EXIT\n", INT_ROUNDS, NTHREADS);
}

int main(void)
{
    RUN(test_concurrent_flv);
    RUN(test_concurrent_interrupt);
    return test_finish();
}
