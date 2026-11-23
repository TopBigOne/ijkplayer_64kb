/*
 * ijkmeta.h
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

#ifndef IJKPLAYER__IJKMETA_H
#define IJKPLAYER__IJKMETA_H

#include <stdint.h>
#include <stdlib.h>

// media meta
#define IJKM_KEY_FORMAT         "format"
#define IJKM_KEY_DURATION_US    "duration_us"
#define IJKM_KEY_START_US       "start_us"
#define IJKM_KEY_BITRATE        "bitrate"
#define IJKM_KEY_VIDEO_STREAM   "video"
#define IJKM_KEY_AUDIO_STREAM   "audio"
#define IJKM_KEY_TIMEDTEXT_STREAM "timedtext"

// stream meta
#define IJKM_KEY_TYPE           "type"
#define IJKM_VAL_TYPE__VIDEO    "video"
#define IJKM_VAL_TYPE__AUDIO    "audio"
#define IJKM_VAL_TYPE__TIMEDTEXT "timedtext"
#define IJKM_VAL_TYPE__UNKNOWN  "unknown"
#define IJKM_KEY_LANGUAGE       "language"

#define IJKM_KEY_CODEC_NAME         "codec_name"
#define IJKM_KEY_CODEC_PROFILE      "codec_profile"
#define IJKM_KEY_CODEC_LEVEL        "codec_level"
#define IJKM_KEY_CODEC_LONG_NAME    "codec_long_name"
#define IJKM_KEY_CODEC_PIXEL_FORMAT "codec_pixel_format"
#define IJKM_KEY_CODEC_PROFILE_ID   "codec_profile_id"

// stream: video
#define IJKM_KEY_WIDTH          "width"
#define IJKM_KEY_HEIGHT         "height"
#define IJKM_KEY_FPS_NUM        "fps_num"
#define IJKM_KEY_FPS_DEN        "fps_den"
#define IJKM_KEY_TBR_NUM        "tbr_num"
#define IJKM_KEY_TBR_DEN        "tbr_den"
#define IJKM_KEY_SAR_NUM        "sar_num"
#define IJKM_KEY_SAR_DEN        "sar_den"
// stream: video color / HDR
#define IJKM_KEY_BIT_DEPTH          "bit_depth"
#define IJKM_KEY_COLOR_RANGE        "color_range"
#define IJKM_KEY_COLOR_PRIMARIES    "color_primaries"
#define IJKM_KEY_COLOR_TRC          "color_trc"
#define IJKM_KEY_COLOR_SPACE        "color_space"
#define IJKM_KEY_HDR_TYPE           "hdr_type"
#define IJKM_VAL_HDR_TYPE__SDR      "sdr"
#define IJKM_VAL_HDR_TYPE__HDR10    "hdr10"     /* PQ (SMPTE ST 2084) */
#define IJKM_VAL_HDR_TYPE__HLG      "hlg"       /* ARIB STD-B67 */
#define IJKM_VAL_HDR_TYPE__UNKNOWN  "unknown"   /* 颜色信息缺失，但疑似 HDR（10bit 或 BT.2020） */
#define IJKM_KEY_MAX_CLL            "max_cll"   /* cd/m2，来自容器层 Content Light Level */
#define IJKM_KEY_MAX_FALL           "max_fall"  /* cd/m2 */
#define IJKM_KEY_MASTERING_MAX_LUM  "mastering_max_luminance" /* cd/m2 */
#define IJKM_KEY_MASTERING_MIN_LUM  "mastering_min_luminance" /* cd/m2 */
// stream: audio
#define IJKM_KEY_SAMPLE_RATE    "sample_rate"
#define IJKM_KEY_CHANNEL_LAYOUT "channel_layout"

// reserved for user
#define IJKM_KEY_STREAMS        "streams"

struct AVFormatContext;
typedef struct IjkMediaMeta IjkMediaMeta;

IjkMediaMeta *ijkmeta_create();
void ijkmeta_reset(IjkMediaMeta *meta);
void ijkmeta_destroy(IjkMediaMeta *meta);
void ijkmeta_destroy_p(IjkMediaMeta **meta);

void ijkmeta_lock(IjkMediaMeta *meta);
void ijkmeta_unlock(IjkMediaMeta *meta);

void ijkmeta_append_child_l(IjkMediaMeta *meta, IjkMediaMeta *child);
void ijkmeta_set_int64_l(IjkMediaMeta *meta, const char *name, int64_t value);
void ijkmeta_set_string_l(IjkMediaMeta *meta, const char *name, const char *value);
void ijkmeta_set_avformat_context_l(IjkMediaMeta *meta, struct AVFormatContext *ic);

// must be freed with free();
const char   *ijkmeta_get_string_l(IjkMediaMeta *meta, const char *name);
int64_t       ijkmeta_get_int64_l(IjkMediaMeta *meta, const char *name, int64_t defaultValue);
size_t        ijkmeta_get_children_count_l(IjkMediaMeta *meta);
// do not free
IjkMediaMeta *ijkmeta_get_child_l(IjkMediaMeta *meta, size_t index);

#endif//IJKPLAYER__IJKMETA_H
