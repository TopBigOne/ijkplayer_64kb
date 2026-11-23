/*
 * ff_e2e_latency.h
 *
 * 直播端到端延迟：解析推流端写入的 SEI 时间戳，在帧显示时计算「显示时刻 - 推流端时刻」。
 *
 * SEI 格式（H.264 NAL type 6 / H.265 NAL type 39、40）：
 *   payloadType = 5 (user_data_unregistered)
 *   payload     = 16 字节 UUID "AIJK_E2E_LATENCY" + 8 字节大端 int64（Unix 毫秒，推流端已做 NTP 校正）
 * 推流端实现见 tools/latency/flv_sei_inject.py。
 *
 * 只被 ff_ffplay.c 包含（全部是 static 函数），这样 iOS 工程不用新增源文件。
 */

#ifndef FFPLAY__FF_E2E_LATENCY_H
#define FFPLAY__FF_E2E_LATENCY_H

#include <math.h>
#include <string.h>
#include "ff_ffplay_def.h"

#define E2E_SEI_UUID        "AIJK_E2E_LATENCY"
#define E2E_SEI_UUID_LEN    16
#define E2E_RING_SIZE       ((int)(sizeof(((VideoState *)0)->e2e_ring) / sizeof(((VideoState *)0)->e2e_ring[0])))
#define E2E_STALE_US        (3 * 1000 * 1000)   /* 超过 3 秒没更新视为没有 SEI */
#define E2E_MAX_EXTRAPOLATE 5.0                 /* 只用 5 秒内的 SEI 推算（SEI 只打在关键帧上时也能用） */
#define E2E_SEI_SCAN_MAX    512                 /* SEI NAL 只解析前 512 字节 */

static int frame_queue_nb_remaining(FrameQueue *f);   /* 定义在 ff_ffplay.c */

/* 去掉防竞争字节（00 00 03 -> 00 00），返回输出长度 */
static int e2e_unescape(const uint8_t *src, int len, uint8_t *dst, int cap)
{
    int i, n = 0, zeros = 0;
    for (i = 0; i < len && n < cap; i++) {
        if (zeros >= 2 && src[i] == 0x03) {
            zeros = 0;
            continue;
        }
        zeros = src[i] == 0 ? zeros + 1 : 0;
        dst[n++] = src[i];
    }
    return n;
}

/* 解析一个 NAL，找到我们的时间戳 SEI 返回 1 */
static int e2e_parse_nal(const uint8_t *nal, int len, int hevc, int64_t *wall_ms)
{
    uint8_t rbsp[E2E_SEI_SCAN_MAX];
    int hdr, n, pos = 0;

    if (len < 3)
        return 0;
    if (hevc) {
        int type = (nal[0] >> 1) & 0x3f;
        if (type != 39 && type != 40)
            return 0;
        hdr = 2;
    } else {
        if ((nal[0] & 0x1f) != 6)
            return 0;
        hdr = 1;
    }

    n = e2e_unescape(nal + hdr, len - hdr, rbsp, sizeof(rbsp));
    while (pos < n && rbsp[pos] != 0x80) {   /* 0x80 = rbsp_trailing_bits */
        int type = 0, size = 0;
        while (pos < n && rbsp[pos] == 0xff) { type += 255; pos++; }
        if (pos >= n) break;
        type += rbsp[pos++];
        while (pos < n && rbsp[pos] == 0xff) { size += 255; pos++; }
        if (pos >= n) break;
        size += rbsp[pos++];
        if (pos + size > n)
            break;
        if (type == 5 && size >= E2E_SEI_UUID_LEN + 8 &&
            !memcmp(rbsp + pos, E2E_SEI_UUID, E2E_SEI_UUID_LEN)) {
            const uint8_t *p = rbsp + pos + E2E_SEI_UUID_LEN;
            int64_t v = 0;
            int i;
            for (i = 0; i < 8; i++)
                v = (v << 8) | p[i];
            *wall_ms = v;
            return 1;
        }
        pos += size;
    }
    return 0;
}

/* NAL 长度前缀字节数：avcC / hvcC 里取，Annex B 返回 0 */
static int e2e_nal_length_size(const AVCodecParameters *par, int hevc)
{
    const uint8_t *ex = par->extradata;
    if (!ex || par->extradata_size < 7 || ex[0] != 1)
        return 0;
    if (hevc)
        return par->extradata_size >= 23 ? (ex[21] & 3) + 1 : 0;
    return (ex[4] & 3) + 1;
}

static int e2e_scan_packet(const uint8_t *d, int size, int len_size, int hevc, int64_t *wall_ms)
{
    int i;

    /* 数据本身是 Annex B 起始码时按 Annex B 解析，不管 extradata 怎么说 */
    if (size >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1)))
        len_size = 0;

    if (len_size > 0) {
        int pos = 0;
        while (pos + len_size <= size) {
            uint32_t nal_len = 0;
            for (i = 0; i < len_size; i++)
                nal_len = (nal_len << 8) | d[pos + i];
            pos += len_size;
            if (nal_len == 0 || nal_len > (uint32_t)(size - pos))
                return 0;
            if (e2e_parse_nal(d + pos, (int)nal_len, hevc, wall_ms))
                return 1;
            pos += nal_len;
        }
        return 0;
    }

    /* Annex B */
    for (i = 0; i + 3 <= size; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            int start = i + 3, end = start;
            while (end + 3 <= size && !(d[end] == 0 && d[end + 1] == 0 && (d[end + 2] == 1 || d[end + 2] == 0)))
                end++;
            if (end + 3 > size)
                end = size;
            if (e2e_parse_nal(d + start, end - start, hevc, wall_ms))
                return 1;
            i = end - 1;
        }
    }
    return 0;
}

/* 读包线程：视频包入队前调用 */
static void ffp_e2e_on_video_packet(FFPlayer *ffp, VideoState *is, const AVPacket *pkt, const AVStream *st)
{
    int hevc;
    int64_t wall_ms, ts;

    if (!is->e2e_mutex || !st || !pkt->data || pkt->size <= 0)
        return;
    if (st->codecpar->codec_id == AV_CODEC_ID_H264)
        hevc = 0;
    else if (st->codecpar->codec_id == AV_CODEC_ID_HEVC)
        hevc = 1;
    else
        return;

    ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (ts == AV_NOPTS_VALUE)
        return;
    if (!e2e_scan_packet(pkt->data, pkt->size, e2e_nal_length_size(st->codecpar, hevc), hevc, &wall_ms))
        return;

    /* 到达延迟：只含推流 -> 服务器 -> 网络 -> 解封装，不含播放端缓冲和解码渲染 */
    ffp->stat.e2e_recv_latency_ms = av_gettime() / 1000 + ffp->e2e_clock_offset_ms - wall_ms;
    ffp->stat.e2e_recv_update_us  = av_gettime_relative();

    SDL_LockMutex(is->e2e_mutex);
    is->e2e_ring[is->e2e_ring_head].pts     = ts * av_q2d(st->time_base);
    is->e2e_ring[is->e2e_ring_head].wall_ms = wall_ms;
    is->e2e_ring_head = (is->e2e_ring_head + 1) % E2E_RING_SIZE;
    if (is->e2e_ring_count < E2E_RING_SIZE)
        is->e2e_ring_count++;
    SDL_UnlockMutex(is->e2e_mutex);
}

/* 显示线程：一帧送显后调用 */
static void ffp_e2e_on_display(FFPlayer *ffp, double pts)
{
    VideoState *is = ffp->is;
    double best_pts = NAN;
    int64_t best_ms = 0, capture_ms, now_ms, latency, now_us;
    int i;

    if (!is || !is->e2e_mutex || isnan(pts) || pts == is->e2e_last_display_pts)
        return;
    is->e2e_last_display_pts = pts;

    /* 找 pts 不大于当前帧的最近一条 SEI（每帧都打 SEI 时就是这一帧自己） */
    SDL_LockMutex(is->e2e_mutex);
    for (i = 0; i < is->e2e_ring_count; i++) {
        double p = is->e2e_ring[i].pts;
        if (p <= pts + 0.0005 && (isnan(best_pts) || p > best_pts)) {
            best_pts = p;
            best_ms  = is->e2e_ring[i].wall_ms;
        }
    }
    SDL_UnlockMutex(is->e2e_mutex);

    if (isnan(best_pts) || pts - best_pts > E2E_MAX_EXTRAPOLATE)
        return;

    capture_ms = best_ms + (int64_t)llround((pts - best_pts) * 1000.0);
    now_ms     = av_gettime() / 1000 + ffp->e2e_clock_offset_ms;
    latency    = now_ms - capture_ms;
    now_us     = av_gettime_relative();

    ffp->stat.e2e_latency_ms = latency;
    ffp->stat.e2e_update_us  = now_us;

    if (now_us - is->e2e_last_log_us > 2 * 1000 * 1000) {
        is->e2e_last_log_us = now_us;
        av_log(ffp, AV_LOG_INFO, "e2e latency: %"PRId64" ms (recv %"PRId64" ms, vcache %"PRId64" ms, acache %"PRId64" ms, pictq %d, sampq %d, clock offset %"PRId64" ms)\n",
               latency, ffp->stat.e2e_recv_latency_ms, ffp->stat.video_cache.duration, ffp->stat.audio_cache.duration,
               frame_queue_nb_remaining(&is->pictq), frame_queue_nb_remaining(&is->sampq), ffp->e2e_clock_offset_ms);
    }
}

static int64_t ffp_e2e_get_recv_latency(FFPlayer *ffp)
{
    if (!ffp->stat.e2e_recv_update_us || av_gettime_relative() - ffp->stat.e2e_recv_update_us > E2E_STALE_US)
        return -1;
    return ffp->stat.e2e_recv_latency_ms;
}

static int64_t ffp_e2e_get_latency(FFPlayer *ffp)
{
    if (!ffp->stat.e2e_update_us || av_gettime_relative() - ffp->stat.e2e_update_us > E2E_STALE_US)
        return -1;
    return ffp->stat.e2e_latency_ms;
}

#endif
