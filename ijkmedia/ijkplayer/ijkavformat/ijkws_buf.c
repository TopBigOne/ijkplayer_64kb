/*
 * ijkws_buf.c —— ijkws 使用的字节环形缓冲
 *
 * 环形数组：head 为读位置，size 为有效字节数。写满时按 2 倍扩容，
 * 扩容时把数据线性化（从新数组的 0 开始连续存放）。
 */
#include "ijkws_buf.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/error.h"

struct IjkWsBuf {
    uint8_t *data;
    size_t   cap;
    size_t   head;
    size_t   size;
};

IjkWsBuf *ijkws_buf_alloc(size_t initial)
{
    IjkWsBuf *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    if (initial == 0)
        initial = 4096;
    b->data = malloc(initial);
    if (!b->data) {
        free(b);
        return NULL;
    }
    b->cap = initial;
    return b;
}

/* 从环形区拷出 n 字节（n <= size），不移动 head */
static void peek(const IjkWsBuf *b, uint8_t *dst, size_t n)
{
    size_t first = b->cap - b->head;
    if (first > n)
        first = n;
    memcpy(dst, b->data + b->head, first);
    if (n > first)
        memcpy(dst + first, b->data, n - first);
}

static int grow(IjkWsBuf *b, size_t need)
{
    size_t cap = b->cap;
    uint8_t *nd;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2)
            return AVERROR(ENOMEM);
        cap *= 2;
    }
    nd = malloc(cap);
    if (!nd)
        return AVERROR(ENOMEM);
    peek(b, nd, b->size);
    free(b->data);
    b->data = nd;
    b->cap  = cap;
    b->head = 0;
    return 0;
}

int ijkws_buf_write(IjkWsBuf *b, const uint8_t *p, size_t n)
{
    size_t tail, first;
    if (n == 0)
        return 0;
    if (n > ((size_t)-1) - b->size)
        return AVERROR(ENOMEM);
    if (b->size + n > b->cap) {
        int ret = grow(b, b->size + n);
        if (ret < 0)
            return ret;
    }
    tail = (b->head + b->size) % b->cap;
    first = b->cap - tail;
    if (first > n)
        first = n;
    memcpy(b->data + tail, p, first);
    if (n > first)
        memcpy(b->data, p + first, n - first);
    b->size += n;
    return 0;
}

size_t ijkws_buf_read(IjkWsBuf *b, uint8_t *dst, size_t n)
{
    if (n > b->size)
        n = b->size;
    if (n == 0)
        return 0;
    peek(b, dst, n);
    b->head = (b->head + n) % b->cap;
    b->size -= n;
    if (b->size == 0)
        b->head = 0;
    return n;
}

size_t ijkws_buf_size(const IjkWsBuf *b)
{
    return b ? b->size : 0;
}

void ijkws_buf_free(IjkWsBuf **b)
{
    if (!b || !*b)
        return;
    free((*b)->data);
    free(*b);
    *b = NULL;
}
