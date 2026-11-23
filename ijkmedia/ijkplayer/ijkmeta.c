/*
 * ijkmeta.c
 *
 * Copyright (c) 2014 Bilibili
 * Copyright (c) 2014 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ijkmeta.h"
#include "ff_ffinc.h"
#include "ijksdl/ijksdl_misc.h"
#include "libavutil/pixdesc.h"
#include "libavutil/mastering_display_metadata.h"

#define IJK_META_INIT_CAPACITY 13

struct IjkMediaMeta {
    SDL_mutex *mutex;

    AVDictionary *dict;

    size_t children_count;
    size_t children_capacity;
    IjkMediaMeta **children;
};

IjkMediaMeta *ijkmeta_create()
{
    IjkMediaMeta *meta = (IjkMediaMeta *)calloc(1, sizeof(IjkMediaMeta));
    if (!meta)
        return NULL;

    meta->mutex = SDL_CreateMutex();
    if (!meta->mutex)
        goto fail;

    return meta;
fail:
    ijkmeta_destroy(meta);
    return NULL;
}

void ijkmeta_reset(IjkMediaMeta *meta)
{
    if (meta && meta->dict)
        av_dict_free(&meta->dict);
}

void ijkmeta_destroy(IjkMediaMeta *meta)
{
    if (!meta)
        return;

    if (meta->dict) {
        av_dict_free(&meta->dict);
    }

    if (meta->children) {
        for(int i = 0; i < meta->children_count; ++i) {
            IjkMediaMeta *child = meta->children[i];
            if (child) {
                ijkmeta_destroy(child);
            }
        }
        free(meta->children);
        meta->children = NULL;
    }

    SDL_DestroyMutexP(&meta->mutex);
    free(meta);
}

void ijkmeta_destroy_p(IjkMediaMeta **meta)
{
    if (!meta)
        return;

    ijkmeta_destroy(*meta);
    *meta = NULL;
}

void ijkmeta_lock(IjkMediaMeta *meta)
{
    if (!meta || !meta->mutex)
        return;

    SDL_LockMutex(meta->mutex);
}

void ijkmeta_unlock(IjkMediaMeta *meta)
{
    if (!meta || !meta->mutex)
        return;

    SDL_UnlockMutex(meta->mutex);
}

void ijkmeta_append_child_l(IjkMediaMeta *meta, IjkMediaMeta *child)
{
    if (!meta || !child)
        return;

    if (!meta->children) {
        meta->children = (IjkMediaMeta **)calloc(IJK_META_INIT_CAPACITY, sizeof(IjkMediaMeta *));
        if (!meta->children)
            return;
        meta->children_count    = 0;
        meta->children_capacity = IJK_META_INIT_CAPACITY;
    } else if (meta->children_count >= meta->children_capacity) {
        size_t new_capacity = meta->children_capacity * 2;
        IjkMediaMeta **new_children = (IjkMediaMeta **)calloc(new_capacity, sizeof(IjkMediaMeta *));
        if (!new_children)
            return;

        memcpy(new_children, meta->children, meta->children_capacity * sizeof(IjkMediaMeta *));
        free(meta->children);
        meta->children          = new_children;
        meta->children_capacity = new_capacity;
    }

    meta->children[meta->children_count] = child;
    meta->children_count++;
}

void ijkmeta_set_int64_l(IjkMediaMeta *meta, const char *name, int64_t value)
{
    if (!meta)
        return;

    av_dict_set_int(&meta->dict, name, value, 0);
}

void ijkmeta_set_string_l(IjkMediaMeta *meta, const char *name, const char *value)
{
    if (!meta)
        return;

    av_dict_set(&meta->dict, name, value, 0);
}

static int64_t get_bit_rate(AVCodecParameters *codecpar)
{
    int64_t bit_rate;
    int bits_per_sample;

    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
        case AVMEDIA_TYPE_DATA:
        case AVMEDIA_TYPE_SUBTITLE:
        case AVMEDIA_TYPE_ATTACHMENT:
            bit_rate = codecpar->bit_rate;
            break;
        case AVMEDIA_TYPE_AUDIO:
            bits_per_sample = av_get_bits_per_sample(codecpar->codec_id);
            bit_rate = bits_per_sample ? codecpar->sample_rate * codecpar->channels * bits_per_sample : codecpar->bit_rate;
            break;
        default:
            bit_rate = 0;
            break;
    }
    return bit_rate;
}

static int get_video_bit_depth(AVCodecParameters *codecpar)
{
    if (codecpar->format != AV_PIX_FMT_NONE) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(codecpar->format);
        if (desc && desc->nb_components > 0)
            return desc->comp[0].depth;
    }
    if (codecpar->bits_per_raw_sample > 0)
        return codecpar->bits_per_raw_sample;
    if (codecpar->codec_id == AV_CODEC_ID_HEVC && codecpar->profile == FF_PROFILE_HEVC_MAIN_10)
        return 10;
    if (codecpar->codec_id == AV_CODEC_ID_H264 && codecpar->profile == FF_PROFILE_H264_HIGH_10)
        return 10;
    return 0;
}

/*
 * 判定规则：
 *   - transfer = PQ(SMPTE2084)  -> hdr10
 *   - transfer = HLG(ARIB-B67)  -> hlg
 *   - transfer 明确为其它值     -> sdr
 *   - transfer 未知：位深 >= 10 或色域 BT.2020 -> unknown（疑似 HDR，需要关注）
 *                    位深已知为 8 / 色域明确非 2020 -> sdr
 *                    什么都不知道 -> unknown
 */
static const char *get_hdr_type(AVCodecParameters *codecpar, int bit_depth)
{
    switch (codecpar->color_trc) {
        case AVCOL_TRC_SMPTE2084:
            return IJKM_VAL_HDR_TYPE__HDR10;
        case AVCOL_TRC_ARIB_STD_B67:
            return IJKM_VAL_HDR_TYPE__HLG;
        case AVCOL_TRC_UNSPECIFIED:
        case AVCOL_TRC_RESERVED0:
        case AVCOL_TRC_RESERVED:
            break;
        default:
            return IJKM_VAL_HDR_TYPE__SDR;
    }

    if (bit_depth >= 10 || codecpar->color_primaries == AVCOL_PRI_BT2020)
        return IJKM_VAL_HDR_TYPE__UNKNOWN;
    if (bit_depth == 8 ||
        (codecpar->color_primaries != AVCOL_PRI_UNSPECIFIED && codecpar->color_primaries != AVCOL_PRI_RESERVED0 && codecpar->color_primaries != AVCOL_PRI_RESERVED))
        return IJKM_VAL_HDR_TYPE__SDR;
    return IJKM_VAL_HDR_TYPE__UNKNOWN;
}

static void set_string_if_valid(IjkMediaMeta *meta, const char *key, const char *value)
{
    if (value && value[0])
        ijkmeta_set_string_l(meta, key, value);
}

static void fill_video_color_meta(IjkMediaMeta *stream_meta, AVStream *st)
{
    AVCodecParameters *codecpar = st->codecpar;
    char buf[32];

    int bit_depth = get_video_bit_depth(codecpar);
    if (bit_depth > 0)
        ijkmeta_set_int64_l(stream_meta, IJKM_KEY_BIT_DEPTH, bit_depth);

    if (codecpar->color_range != AVCOL_RANGE_UNSPECIFIED)
        set_string_if_valid(stream_meta, IJKM_KEY_COLOR_RANGE, av_color_range_name(codecpar->color_range));
    if (codecpar->color_primaries != AVCOL_PRI_UNSPECIFIED)
        set_string_if_valid(stream_meta, IJKM_KEY_COLOR_PRIMARIES, av_color_primaries_name(codecpar->color_primaries));
    if (codecpar->color_trc != AVCOL_TRC_UNSPECIFIED)
        set_string_if_valid(stream_meta, IJKM_KEY_COLOR_TRC, av_color_transfer_name(codecpar->color_trc));
    if (codecpar->color_space != AVCOL_SPC_UNSPECIFIED)
        set_string_if_valid(stream_meta, IJKM_KEY_COLOR_SPACE, av_color_space_name(codecpar->color_space));

    const char *hdr_type = get_hdr_type(codecpar, bit_depth);
    ijkmeta_set_string_l(stream_meta, IJKM_KEY_HDR_TYPE, hdr_type);

    /* 容器层（如 mp4 的 mdcv/clli）携带的 HDR 静态元数据；码流 SEI 里的只能在解码后从帧上拿，这里不处理 */
    int size = 0;
    const AVContentLightMetadata *cll = (const AVContentLightMetadata *)
        av_stream_get_side_data(st, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, &size);
    if (cll && size >= (int)sizeof(*cll)) {
        if (cll->MaxCLL > 0)
            ijkmeta_set_int64_l(stream_meta, IJKM_KEY_MAX_CLL, cll->MaxCLL);
        if (cll->MaxFALL > 0)
            ijkmeta_set_int64_l(stream_meta, IJKM_KEY_MAX_FALL, cll->MaxFALL);
    }

    size = 0;
    const AVMasteringDisplayMetadata *mdm = (const AVMasteringDisplayMetadata *)
        av_stream_get_side_data(st, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, &size);
    if (mdm && size >= (int)sizeof(*mdm) && mdm->has_luminance) {
        snprintf(buf, sizeof(buf), "%.4f", av_q2d(mdm->max_luminance));
        ijkmeta_set_string_l(stream_meta, IJKM_KEY_MASTERING_MAX_LUM, buf);
        snprintf(buf, sizeof(buf), "%.4f", av_q2d(mdm->min_luminance));
        ijkmeta_set_string_l(stream_meta, IJKM_KEY_MASTERING_MIN_LUM, buf);
    }

    av_log(NULL, AV_LOG_INFO, "ijkmeta: video stream #%d hdr_type=%s bit_depth=%d trc=%s primaries=%s space=%s range=%s\n",
           st->index, hdr_type, bit_depth,
           av_color_transfer_name(codecpar->color_trc) ? av_color_transfer_name(codecpar->color_trc) : "?",
           av_color_primaries_name(codecpar->color_primaries) ? av_color_primaries_name(codecpar->color_primaries) : "?",
           av_color_space_name(codecpar->color_space) ? av_color_space_name(codecpar->color_space) : "?",
           av_color_range_name(codecpar->color_range) ? av_color_range_name(codecpar->color_range) : "?");
}

void ijkmeta_set_avformat_context_l(IjkMediaMeta *meta, AVFormatContext *ic)
{
    if (!meta || !ic)
        return;

    if (ic->iformat && ic->iformat->name)
        ijkmeta_set_string_l(meta, IJKM_KEY_FORMAT, ic->iformat->name);

    if (ic->duration != AV_NOPTS_VALUE)
        ijkmeta_set_int64_l(meta, IJKM_KEY_DURATION_US, ic->duration);

    if (ic->start_time != AV_NOPTS_VALUE)
        ijkmeta_set_int64_l(meta, IJKM_KEY_START_US, ic->start_time);

    if (ic->bit_rate)
        ijkmeta_set_int64_l(meta, IJKM_KEY_BITRATE, ic->bit_rate);

    IjkMediaMeta *stream_meta = NULL;
    for (int i = 0; i < ic->nb_streams; i++) {
        if (!stream_meta)
            ijkmeta_destroy_p(&stream_meta);

        AVStream *st = ic->streams[i];
        if (!st || !st->codecpar)
            continue;

        stream_meta = ijkmeta_create();
        if (!stream_meta)
            continue;

        AVCodecParameters *codecpar = st->codecpar;
        const char *codec_name = avcodec_get_name(codecpar->codec_id);
        if (codec_name)
            ijkmeta_set_string_l(stream_meta, IJKM_KEY_CODEC_NAME, codec_name);
        if (codecpar->profile != FF_PROFILE_UNKNOWN) {
            const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
            if (codec) {
                ijkmeta_set_int64_l(stream_meta, IJKM_KEY_CODEC_PROFILE_ID, codecpar->profile);
                const char *profile = av_get_profile_name(codec, codecpar->profile);
                if (profile)
                    ijkmeta_set_string_l(stream_meta, IJKM_KEY_CODEC_PROFILE, profile);
                if (codec->long_name)
                    ijkmeta_set_string_l(stream_meta, IJKM_KEY_CODEC_LONG_NAME, codec->long_name);
                ijkmeta_set_int64_l(stream_meta, IJKM_KEY_CODEC_LEVEL, codecpar->level);
                if (codecpar->format != AV_PIX_FMT_NONE)
                    ijkmeta_set_string_l(stream_meta, IJKM_KEY_CODEC_PIXEL_FORMAT, av_get_pix_fmt_name(codecpar->format));
            }
        }

        int64_t bitrate = get_bit_rate(codecpar);
        if (bitrate > 0) {
            ijkmeta_set_int64_l(stream_meta, IJKM_KEY_BITRATE, bitrate);
        }

        switch (codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO: {
                ijkmeta_set_string_l(stream_meta, IJKM_KEY_TYPE, IJKM_VAL_TYPE__VIDEO);

                if (codecpar->width > 0)
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_WIDTH, codecpar->width);
                if (codecpar->height > 0)
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_HEIGHT, codecpar->height);
                if (st->sample_aspect_ratio.num > 0 && st->sample_aspect_ratio.den > 0) {
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_SAR_NUM, codecpar->sample_aspect_ratio.num);
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_SAR_DEN, codecpar->sample_aspect_ratio.den);
                }

                if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_FPS_NUM, st->avg_frame_rate.num);
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_FPS_DEN, st->avg_frame_rate.den);
                }
                if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0) {
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_TBR_NUM, st->avg_frame_rate.num);
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_TBR_DEN, st->avg_frame_rate.den);
                }

                fill_video_color_meta(stream_meta, st);
                break;
            }
            case AVMEDIA_TYPE_AUDIO: {
                ijkmeta_set_string_l(stream_meta, IJKM_KEY_TYPE, IJKM_VAL_TYPE__AUDIO);

                if (codecpar->sample_rate)
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_SAMPLE_RATE, codecpar->sample_rate);
                if (codecpar->channel_layout)
                    ijkmeta_set_int64_l(stream_meta, IJKM_KEY_CHANNEL_LAYOUT, codecpar->channel_layout);
                break;
            }
            case AVMEDIA_TYPE_SUBTITLE: {
                ijkmeta_set_string_l(stream_meta, IJKM_KEY_TYPE, IJKM_VAL_TYPE__TIMEDTEXT);
                break;
            }
            default: {
                ijkmeta_set_string_l(stream_meta, IJKM_KEY_TYPE, IJKM_VAL_TYPE__UNKNOWN);
                break;
            }
        }

        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        if (lang && lang->value)
            ijkmeta_set_string_l(stream_meta, IJKM_KEY_LANGUAGE, lang->value);

        ijkmeta_append_child_l(meta, stream_meta);
        stream_meta = NULL;
    }

    if (!stream_meta)
        ijkmeta_destroy_p(&stream_meta);
}

const char *ijkmeta_get_string_l(IjkMediaMeta *meta, const char *name)
{
    if (!meta || !meta->dict || !name)
        return NULL;

    AVDictionaryEntry *entry = av_dict_get(meta->dict, name, NULL, 0);
    if (!entry)
        return NULL;

    return entry->value;
}

int64_t ijkmeta_get_int64_l(IjkMediaMeta *meta, const char *name, int64_t defaultValue)
{
    if (!meta || !meta->dict)
        return defaultValue;

    AVDictionaryEntry *entry = av_dict_get(meta->dict, name, NULL, 0);
    if (!entry || !entry->value)
        return defaultValue;

    return atoll(entry->value);
}

size_t ijkmeta_get_children_count_l(IjkMediaMeta *meta)
{
    if (!meta || !meta->children)
        return 0;

    return meta->children_count;
}

IjkMediaMeta *ijkmeta_get_child_l(IjkMediaMeta *meta, size_t index)
{
    if (!meta)
        return NULL;

    if (index >= meta->children_count)
        return NULL;

    return meta->children[index];
}
