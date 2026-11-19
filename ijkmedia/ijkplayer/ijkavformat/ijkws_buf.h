/*
 * ijkws_buf.h —— ijkws 使用的字节环形缓冲
 *
 * 设备端 FFmpeg 为 4.0，主机测试用 4.4，两者的 FIFO API 不一致，
 * 因此这里自写一个最小实现：满了按 2 倍扩容，不设上限（上限由调用方控制）。
 * 非线程安全，只在 read_thread 中使用。
 */
#ifndef IJKAVFORMAT_IJKWS_BUF_H
#define IJKAVFORMAT_IJKWS_BUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct IjkWsBuf IjkWsBuf;

IjkWsBuf *ijkws_buf_alloc(size_t initial);
/* 返回 0 或 AVERROR(ENOMEM) */
int       ijkws_buf_write(IjkWsBuf *b, const uint8_t *p, size_t n);
/* 返回实际读出的字节数 */
size_t    ijkws_buf_read(IjkWsBuf *b, uint8_t *dst, size_t n);
size_t    ijkws_buf_size(const IjkWsBuf *b);
void      ijkws_buf_free(IjkWsBuf **b);

#endif
