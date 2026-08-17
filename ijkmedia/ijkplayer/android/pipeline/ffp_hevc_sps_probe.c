/*
 * Copyright (c) 2026
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

#include "ffp_hevc_sps_probe.h"
#include <stddef.h>

#define FFHEVC_NAL_TYPE_SPS 33

typedef struct {
    const uint8_t *data;
    int            size;     /* bytes */
    int            bit_pos;  /* 0-based bit offset from start */
} FFHevcBitReader;

/* 读 n 位无符号数；n 可以大于 32（这种情况下只用于跳过，out 必须传 NULL，
 * 因为 32 位累加器在 n>32 时会静默溢出，调用方不应该读它的值）。
 * 返回 0 成功，-1 表示读越界。 */
static int ffhevc_br_u(FFHevcBitReader *br, int n, uint32_t *out)
{
    uint32_t val = 0;
    int i;
    for (i = 0; i < n; i++) {
        int byte_idx = br->bit_pos >> 3;
        int bit_idx  = 7 - (br->bit_pos & 7);
        if (byte_idx >= br->size)
            return -1;
        val = (val << 1) | ((br->data[byte_idx] >> bit_idx) & 1u);
        br->bit_pos++;
    }
    if (out)
        *out = val;
    return 0;
}

/* Exp-Golomb ue(v) 解码，见 H.265 spec 9.2 节 */
static int ffhevc_br_ue(FFHevcBitReader *br, uint32_t *out)
{
    int      leading_zero_bits = 0;
    uint32_t bit               = 0;
    uint32_t val;

    for (;;) {
        if (ffhevc_br_u(br, 1, &bit) < 0)
            return -1;
        if (bit != 0)
            break;
        leading_zero_bits++;
        if (leading_zero_bits > 31) /* 防御性上限，畸形数据不应该有这么多前导零 */
            return -1;
    }

    if (leading_zero_bits == 0) {
        val = 0;
    } else {
        uint32_t suffix = 0;
        if (ffhevc_br_u(br, leading_zero_bits, &suffix) < 0)
            return -1;
        val = (1u << leading_zero_bits) - 1u + suffix;
    }

    if (out)
        *out = val;
    return 0;
}

/* 去掉 2 字节 HEVC NAL header，并还原 emulation prevention byte（00 00 03 -> 00 00）。
 * out 缓冲区必须至少 in_size 字节。返回写入 out 的字节数，-1 表示 in_size 太小。 */
static int ffhevc_nal_to_rbsp(const uint8_t *in, int in_size, uint8_t *out)
{
    int i, out_size = 0;
    int zero_run = 0;

    if (in_size < 2)
        return -1;

    for (i = 2; i < in_size; i++) {
        if (zero_run >= 2 && in[i] == 0x03) {
            zero_run = 0;
            continue; /* 丢弃 emulation prevention byte 本身 */
        }
        out[out_size++] = in[i];
        zero_run = (in[i] == 0x00) ? (zero_run + 1) : 0;
    }
    return out_size;
}

/* 跳过 profile_tier_level()（H.265 spec 7.3.3），SPS 里调用时 profilePresentFlag 恒为 1。
 * max_sub_layers_minus1 == 0（单层，绝大多数直播/摄像头编码场景）时正好是 96 位（12 字节）。 */
static int ffhevc_skip_profile_tier_level(FFHevcBitReader *br, int max_sub_layers_minus1)
{
    uint8_t sub_layer_profile_present_flag[8] = {0};
    uint8_t sub_layer_level_present_flag[8]   = {0};
    int i;

    /* general_profile_space/tier_flag/profile_idc(8) +
     * general_profile_compatibility_flag[32](32) +
     * general 各类 constraint/reserved 位(48，含 progressive/interlaced/
     * non_packed/frame_only_constraint_flag 及后续 reserved 位) +
     * general_level_idc(8) = 96 位，跟 hvcC 容器里固定头部的 12 字节布局一致 */
    if (ffhevc_br_u(br, 96, NULL) < 0)
        return -1;

    for (i = 0; i < max_sub_layers_minus1; i++) {
        uint32_t profile_present = 0, level_present = 0;
        if (ffhevc_br_u(br, 1, &profile_present) < 0) return -1;
        if (ffhevc_br_u(br, 1, &level_present) < 0) return -1;
        sub_layer_profile_present_flag[i] = (uint8_t)profile_present;
        sub_layer_level_present_flag[i]   = (uint8_t)level_present;
    }
    if (max_sub_layers_minus1 > 0) {
        for (i = max_sub_layers_minus1; i < 8; i++) {
            if (ffhevc_br_u(br, 2, NULL) < 0) /* reserved_zero_2bits */
                return -1;
        }
    }
    for (i = 0; i < max_sub_layers_minus1; i++) {
        if (sub_layer_profile_present_flag[i]) {
            if (ffhevc_br_u(br, 88, NULL) < 0) /* 跟 general 部分一样，只是没有 level_idc */
                return -1;
        }
        if (sub_layer_level_present_flag[i]) {
            if (ffhevc_br_u(br, 8, NULL) < 0) /* sub_layer_level_idc */
                return -1;
        }
    }
    return 0;
}

/* 解析一个完整的 SPS NAL（含 2 字节 header，未去除 emulation prevention byte），
 * 拿到 pic_width_in_luma_samples / pic_height_in_luma_samples。 */
static int ffhevc_parse_sps_size(const uint8_t *nal, int nal_size, int *out_width, int *out_height)
{
    uint8_t         rbsp[512];
    int             rbsp_size;
    FFHevcBitReader br;
    uint32_t        sps_max_sub_layers_minus1 = 0;
    uint32_t        sps_seq_parameter_set_id  = 0;
    uint32_t        chroma_format_idc         = 0;
    uint32_t        pic_width  = 0;
    uint32_t        pic_height = 0;

    if (nal_size < 2 || nal_size > (int)sizeof(rbsp) + 2)
        return -1;

    rbsp_size = ffhevc_nal_to_rbsp(nal, nal_size, rbsp);
    if (rbsp_size < 13) /* 12 字节 profile_tier_level + 至少 1 字节真正语法 */
        return -1;

    br.data    = rbsp;
    br.size    = rbsp_size;
    br.bit_pos = 0;

    if (ffhevc_br_u(&br, 4, NULL) < 0)                             return -1; /* sps_video_parameter_set_id */
    if (ffhevc_br_u(&br, 3, &sps_max_sub_layers_minus1) < 0)       return -1; /* sps_max_sub_layers_minus1 */
    if (ffhevc_br_u(&br, 1, NULL) < 0)                             return -1; /* sps_temporal_id_nesting_flag */
    if (ffhevc_skip_profile_tier_level(&br, (int)sps_max_sub_layers_minus1) < 0)
        return -1;
    if (ffhevc_br_ue(&br, &sps_seq_parameter_set_id) < 0)          return -1;
    if (ffhevc_br_ue(&br, &chroma_format_idc) < 0)                 return -1;
    if (chroma_format_idc == 3) {
        if (ffhevc_br_u(&br, 1, NULL) < 0)                         return -1; /* separate_colour_plane_flag */
    }
    if (ffhevc_br_ue(&br, &pic_width) < 0)                         return -1; /* pic_width_in_luma_samples */
    if (ffhevc_br_ue(&br, &pic_height) < 0)                        return -1; /* pic_height_in_luma_samples */

    /* 合理性兜底：畸形/被截断的数据绝不能被当成真实分辨率往下传 */
    if (pic_width == 0 || pic_height == 0 || pic_width > 8192 || pic_height > 8192)
        return -1;

    (void)sps_seq_parameter_set_id; /* 只需要跳过，不需要用这个值 */

    *out_width  = (int)pic_width;
    *out_height = (int)pic_height;
    return 0;
}

/* 在长度前缀格式的 packet 里查找第一个指定 nal_unit_type 的 NAL，返回指向该 NAL
 * 起始位置（含 2 字节 header）的指针，大小写进 *out_size；找不到/数据异常返回 NULL。
 *
 * nal_size 对外是 size_t（跟 opaque->nal_size 的类型保持一致），这里先做一次范围
 * 校验（合法值只可能是 1~4）再转成局部 int 参与后续跟 pos/size（本来就是 int）的
 * 算术，转换是安全的，同时避免函数体内到处出现 int/size_t 混合运算。 */
static const uint8_t *ffhevc_find_nal(const uint8_t *data, int size, size_t nal_size, int want_type, int *out_size)
{
    int pos = 0;
    int nal_len_bytes;

    if (!data || nal_size < 1 || nal_size > 4)
        return NULL;
    nal_len_bytes = (int)nal_size;

    while (pos + nal_len_bytes <= size) {
        uint32_t len = 0;
        int i, nal_type;

        for (i = 0; i < nal_len_bytes; i++)
            len = (len << 8) | data[pos + i];
        pos += nal_len_bytes;

        /* 长度字段不合理，停止扫描，不越界读。检查时用无符号比较避免 int 溢出：
         * 当 len 超过 INT_MAX 时不能直接转 int（会变负），而应该判断 len 是否超过
         * 剩余缓冲区大小 (size - pos)。同时保证 pos <= size 再做减法防止下溢。 */
        if (len == 0 || pos > size || len > (uint32_t)(size - pos))
            break;

        if ((int)len >= 2) {
            nal_type = (data[pos] >> 1) & 0x3F;
            if (nal_type == want_type) {
                *out_size = (int)len;
                return data + pos;
            }
        }
        pos += (int)len;
    }
    return NULL;
}

int ffhevc_probe_sps_size(const uint8_t *data, int size, size_t nal_size,
                           int *out_width, int *out_height)
{
    int            sps_size = 0;
    const uint8_t *sps_nal;

    if (!data || size <= 0 || !out_width || !out_height)
        return -1;

    sps_nal = ffhevc_find_nal(data, size, nal_size, FFHEVC_NAL_TYPE_SPS, &sps_size);
    if (!sps_nal)
        return -1;

    return ffhevc_parse_sps_size(sps_nal, sps_size, out_width, out_height);
}
