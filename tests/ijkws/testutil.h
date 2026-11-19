/*
 * ijkws 主机测试的极简测试框架（header-only，每个 test_*.c 各自包含一次）。
 *
 * 用法：
 *   static void test_xxx(void) { CHECK(...); CHECK_EQ_INT(a, b); }
 *   int main(void) { RUN(test_xxx); return test_finish(); }
 *
 * CHECK 失败时打印位置并从当前测试函数 return（因此只能在返回 void 的测试函数里用）。
 * 结束时打印 "#counts pass=N fail=M"，由 run.sh 汇总。
 */
#ifndef IJKWS_TESTUTIL_H
#define IJKWS_TESTUTIL_H

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libavutil/error.h"
#include "ijkws.h"

static int g_test_pass;
static int g_test_fail;
static int g_test_cur_failed;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        g_test_cur_failed = 1; \
        return; \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  %s:%d: CHECK_EQ_INT failed: %s (%lld) != %s (%lld)\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        g_test_cur_failed = 1; \
        return; \
    } \
} while (0)

#define RUN(fn) run_test(#fn, fn)

static inline void run_test(const char *name, void (*fn)(void))
{
    g_test_cur_failed = 0;
    fn();
    if (g_test_cur_failed) {
        g_test_fail++;
        printf("[FAIL] %s\n", name);
    } else {
        g_test_pass++;
        printf("[PASS] %s\n", name);
    }
    fflush(stdout);
}

static inline int test_finish(void)
{
    printf("#counts pass=%d fail=%d\n", g_test_pass, g_test_fail);
    return g_test_fail ? 1 : 0;
}

static inline int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 测试产物目录（test.flv、证书、服务器日志），由 run.sh 通过环境变量传入 */
static inline const char *test_build_dir(void)
{
    const char *d = getenv("IJKWS_TEST_BUILD");
    return d ? d : ".";
}

static inline char *test_path(char *dst, size_t size, const char *name)
{
    snprintf(dst, size, "%s/%s", test_build_dir(), name);
    return dst;
}

/* 循环 ijkws_read 直到返回值 <= 0；返回最后一次的返回值。*buf 由调用方 free */
static inline int read_all(IjkWsContext *ctx, uint8_t **buf, size_t *len)
{
    size_t cap = 1 << 20;
    uint8_t *p = malloc(cap);
    size_t n = 0;
    int ret;
    for (;;) {
        if (cap - n < 32768) {
            cap *= 2;
            p = realloc(p, cap);
        }
        ret = ijkws_read(ctx, p + n, 32768);
        if (ret <= 0)
            break;
        n += (size_t)ret;
    }
    *buf = p;
    *len = n;
    return ret;
}

/* 读入整个文件；成功返回 0，*buf 由调用方 free */
static inline int load_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    *buf = malloc(sz > 0 ? (size_t)sz : 1);
    *len = fread(*buf, 1, (size_t)sz, f);
    fclose(f);
    return *len == (size_t)sz ? 0 : -1;
}

/* interrupt callback：opaque 指向一个 volatile int，非 0 即打断 */
static inline int test_interrupt_cb(void *opaque)
{
    return *(volatile int *)opaque;
}

typedef struct {
    volatile int *flag;
    int ms;
} TestIntArg;

static inline void *test_interrupt_thread(void *arg)
{
    TestIntArg a = *(TestIntArg *)arg;
    free(arg);
    usleep((useconds_t)a.ms * 1000);
    *a.flag = 1;
    return NULL;
}

/* 在 ms 毫秒后把 *flag 置 1（用 pthread 实现，只在测试中使用） */
static inline void interrupt_after_ms(volatile int *flag, int ms)
{
    pthread_t th;
    TestIntArg *a = malloc(sizeof(*a));
    a->flag = flag;
    a->ms = ms;
    pthread_create(&th, NULL, test_interrupt_thread, a);
    pthread_detach(th);
}

#endif
