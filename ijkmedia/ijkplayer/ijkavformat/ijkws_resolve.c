/*
 * ijkws_resolve.c —— ijkws 的设备端 DNS 解析
 *
 * 复用 libavformat（ijk 补丁版 tcp.c）导出的 ijk_tcp_getaddrinfo_nonblock：
 * 它在后台线程里做 getaddrinfo，调用方这边按 int_cb 和 timeout 可打断。
 * 主机测试不编译本文件，改用 tests/ijkws/resolve_host.c。
 */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "ijkws.h"

/* 签名以 FFToolChain/patches/ffmpeg-n4.0/0010-tcp-hook.patch 为准 */
int ijk_tcp_getaddrinfo_nonblock(const char *hostname, const char *servname,
                                 const struct addrinfo *hints, struct addrinfo **res,
                                 int64_t timeout,
                                 const AVIOInterruptCB *int_cb, int one_by_one);

static int interrupted(const AVIOInterruptCB *int_cb)
{
    return int_cb && int_cb->callback && int_cb->callback(int_cb->opaque);
}

int ijkws_resolve(const char *host, int port, char *ip, size_t ip_size,
                  int64_t timeout_us, const AVIOInterruptCB *int_cb)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags   |= AI_ADDRCONFIG;  /* 只返回本机有对应地址族的结果（无 IPv6 时不给 AAAA） */
    snprintf(portstr, sizeof(portstr), "%d", port);

    ret = ijk_tcp_getaddrinfo_nonblock(host, portstr, &hints, &res, timeout_us, int_cb, 0);
    if (ret != 0 || !res) {
        if (res)
            freeaddrinfo(res);
        if (interrupted(int_cb))
            return AVERROR_EXIT;
        if (ret == AVERROR_EXIT) {
            /* 未被打断却返回 EXIT，说明是解析超时 */
            av_log(NULL, AV_LOG_ERROR, "ijkws: resolve %s timeout\n", host);
            return AVERROR(ETIMEDOUT);
        }
        if (ret == AVERROR(ENOMEM)) {
            av_log(NULL, AV_LOG_ERROR, "ijkws: resolve %s out of memory\n", host);
            return ret;
        }
        av_log(NULL, AV_LOG_ERROR, "ijkws: resolve %s failed (%d)\n", host, ret);
        return AVERROR(EIO);
    }

    ret = getnameinfo(res->ai_addr, res->ai_addrlen, ip, (socklen_t)ip_size, NULL, 0, NI_NUMERICHOST);
    freeaddrinfo(res);
    if (ret != 0) {
        av_log(NULL, AV_LOG_ERROR, "ijkws: getnameinfo %s failed (%d)\n", host, ret);
        return AVERROR(EIO);
    }
    return 0;
}
