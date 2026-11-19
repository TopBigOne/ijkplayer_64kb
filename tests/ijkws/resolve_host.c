/*
 * 主机测试用的 ijkws_resolve：直接 getaddrinfo + getnameinfo(NI_NUMERICHOST)，
 * 忽略 timeout 和 int_cb。设备端实现见 ijkmedia/ijkplayer/ijkavformat/ijkws_resolve.c。
 */
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "libavutil/error.h"
#include "ijkws.h"

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
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return AVERROR(EIO);
    ret = getnameinfo(res->ai_addr, res->ai_addrlen, ip, (socklen_t)ip_size, NULL, 0, NI_NUMERICHOST);
    freeaddrinfo(res);
    return ret == 0 ? 0 : AVERROR(EIO);
}
