/*
 * ijkws_cat —— 本地开发工具：用 ijkws 拉 ws/wss 流，原样写到 stdout。
 *
 * 构建：tests/ijkws/build.sh cat      （产物 tests/ijkws/build/ijkws_cat）
 * 用法：ijkws_cat [-v] [-o key=value]... <ws-or-wss-url>
 *   例：ijkws_cat ws://127.0.0.1:18780/live/test.flv | /usr/local/ffmpeg/4.4/bin/ffplay -
 *   -o  放进传给 ijkws_open 的 AVDictionary（如 -o user_agent=xxx）
 *   -v  每秒向 stderr 打印一次累计字节数和速率
 * SIGINT/SIGTERM -> 中断标志 -> ijkws 返回 AVERROR_EXIT -> 干净退出 0。
 * SIGPIPE 被忽略，stdout 写 EPIPE 视为正常停止（退出 0）。
 * 出错（open/read）退出 1。
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "libavutil/error.h"
#include "ijkws.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static int interrupt_cb(void *opaque) { (void)opaque; return g_stop; }

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int write_all(const uint8_t *p, int n)
{
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, p, (size_t)n);
        if (w < 0) {
            if (errno == EINTR) {
                if (g_stop) return -1;
                continue;
            }
            return -1;
        }
        p += w;
        n -= (int)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    AVDictionary *opts = NULL;
    int verbose = 0, i, ret = 0, exit_code = 0;
    const char *url = NULL;
    IjkWsContext *ws = NULL;
    AVIOInterruptCB cb = { interrupt_cb, NULL };
    uint8_t buf[64 * 1024];
    long long total = 0, last_total = 0;
    int64_t last_ms;
    struct sigaction sa;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
            char *kv = argv[++i];
            char *eq = strchr(kv, '=');
            if (!eq) { fprintf(stderr, "bad -o '%s' (want key=value)\n", kv); return 2; }
            *eq = 0;
            av_dict_set(&opts, kv, eq + 1, 0);
        } else if (argv[i][0] == '-' && argv[i][1]) {
            url = NULL;
            break;
        } else {
            url = argv[i];
        }
    }
    if (!url) {
        fprintf(stderr, "usage: %s [-v] [-o key=value]... <ws-or-wss-url>\n", argv[0]);
        return 2;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;   /* 不设 SA_RESTART，让阻塞调用能被打断 */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    ret = ijkws_open(&ws, url, &opts, &cb);
    if (ret < 0) {
        if (ret == AVERROR_EXIT) { av_dict_free(&opts); return 0; }
        fprintf(stderr, "ijkws_cat: open %s failed: %s\n", url, av_err2str(ret));
        av_dict_free(&opts);
        return 1;
    }

    last_ms = mono_ms();
    for (;;) {
        ret = ijkws_read(ws, buf, sizeof(buf));
        if (ret > 0) {
            if (write_all(buf, ret) < 0) {
                if (errno != EPIPE && !g_stop)
                    fprintf(stderr, "ijkws_cat: write stdout: %s\n", strerror(errno));
                break;  /* EPIPE：下游关闭，正常停止 */
            }
            total += ret;
        } else if (ret == AVERROR_EOF || ret == AVERROR_EXIT) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "ijkws_cat: read failed: %s\n", av_err2str(ret));
            exit_code = 1;
            break;
        }
        if (verbose) {
            int64_t now = mono_ms();
            if (now - last_ms >= 1000) {
                fprintf(stderr, "[ijkws_cat] total=%lld bytes rate=%.1f KB/s\n", total,
                        (total - last_total) / 1024.0 * 1000.0 / (double)(now - last_ms));
                last_total = total;
                last_ms = now;
            }
        }
        if (g_stop) break;
    }

    ijkws_close(&ws);
    av_dict_free(&opts);
    if (verbose) fprintf(stderr, "[ijkws_cat] done total=%lld bytes\n", total);
    return exit_code;
}
