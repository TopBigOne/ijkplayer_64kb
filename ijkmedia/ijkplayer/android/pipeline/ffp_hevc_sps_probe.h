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

#ifndef IJKSDL__FFP_HEVC_SPS_PROBE_H
#define IJKSDL__FFP_HEVC_SPS_PROBE_H

#include <stdint.h>
#include <stddef.h>

/* HEVC nal_unit_type 常量（H.265 Table 7-1） */
#define FFHEVC_NAL_TYPE_VPS 32
#define FFHEVC_NAL_TYPE_SPS 33
#define FFHEVC_NAL_TYPE_PPS 34

/*
 * 在一段"长度前缀"格式（avcC/hvcC sample 格式：nal_size 字节大端长度 + NAL 数据，
 * 循环重复）的 HEVC packet 里查找第一个 SPS NAL（nal_unit_type == 33），并解析出
 * pic_width_in_luma_samples / pic_height_in_luma_samples。
 *
 * 不依赖 FFmpeg 的 AV_PKT_DATA_NEW_EXTRADATA side data（hevc_parser 不产出这个），
 * 直接扫描 packet 自身的 NAL 结构。
 *
 * 返回 0 并写入 out_width 和 out_height（指针参数）表示找到且解析成功；
 * 返回 -1 表示没找到 SPS，或者数据异常/不完整（调用者应当跳过这次更新，不能相信
 * 部分解析出的结果）。
 *
 * nal_size 用 size_t（而不是 int）是为了跟本仓库里同一个概念的既有约定保持一致
 * （ffpipenode_android_mediacodec_vdec.c 里的 opaque->nal_size 字段、以及
 * h264_nal.h 里 convert_h264_to_annexb 的 i_nal_size 参数，都是 size_t）。
 */
int ffhevc_probe_sps_size(const uint8_t *data, int size, size_t nal_size,
                           int *out_width, int *out_height);

/*
 * 内部 NAL 扫描器的公开封装：在同一种"长度前缀"格式的 packet 里查找第一个
 * nal_unit_type == want_type 的 NAL（VPS=32 / SPS=33 / PPS=34 见上面的常量）。
 *
 * 返回指向该 NAL 起始位置的指针（含 2 字节 HEVC NAL header，不含长度前缀），
 * 并把该 NAL 的字节数写进 out_size（指针参数）；
 * 找不到、或者数据异常/不完整时返回 NULL（此时 out_size 的值未定义，调用方
 * 必须先判断返回值非 NULL 再使用 out_size）。
 *
 * 本函数只按类型定位 NAL，不解析 NAL 的负载内容。
 */
const uint8_t *ffhevc_find_nal_public(const uint8_t *data, int size, size_t nal_size,
                                       int want_type, int *out_size);

#endif
