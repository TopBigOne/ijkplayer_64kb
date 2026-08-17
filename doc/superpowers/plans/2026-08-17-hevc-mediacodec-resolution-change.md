# HEVC MediaCodec 分辨率切换处理 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Android MediaCodec 硬解 HEVC 直播流时，能正确处理推流端中途切换分辨率（如 480p→720p，或更大跨度如 480p→4K）的场景，不花屏、不崩溃。

**Architecture:** 两条互补路径协同工作：①MediaCodec 自身发出的 `INFO_OUTPUT_FORMAT_CHANGED` 信号（本会话已修复，处理硬件解码器自行完成的小幅自适应缩放）；②在 `feed_input_buffer2`/`feed_input_buffer` 里新增一个纯 C、不依赖 FFmpeg parser side data 的 HEVC SPS NAL 扫描探测器，检测到新 SPS 分辨率跟当前不一致时，复用已有的 `aformat_need_recreate` + `ffpipeline_set_surface_need_reconfigure_l` 机制触发 `recreate_format_l`+`reconfigure_codec_l`——这条路径在当前代码里本来就是"完全重建 MediaCodec 实例"（`quirk_reconfigure_with_new_codec` 恒为 true），天然覆盖大跨度分辨率跳变，不依赖硬件 Adaptive Playback 能力。额外给 MediaFormat 配置加上 `max-width`/`max-height` 提示，提高小跳变走纯硬件自适应（无需重建）的成功率。

**Tech Stack:** C（NDK 交叉编译 + 可独立用 host clang 编译单测的纯 C 模块）、Android MediaCodec JNI 封装（本仓库自带的 `SDL_AMediaCodec*`/`SDL_AMediaFormat*` 包装层）、CMake（`GLOB_RECURSE` 自动收集源文件）。

**Spec:** 本计划文档自带设计说明（见下方"设计背景"），由本次对话中的架构讨论提炼而来，未使用独立的 spec 文件。

## 设计背景（why）

- 现状：`ffpipenode_android_mediacodec_vdec.c` 里已有一套探测机制，靠读取 `AVPacket` 上的 `AV_PKT_DATA_NEW_EXTRADATA` side data 来发现分辨率变化；这个 side data 是 FFmpeg **原生、未打补丁**的 `h264_parser.c` 在检测到新 SPS/PPS 时自己生成的——已核实 `FFToolChain/patches/` 下所有版本都没有补丁碰过 `h264_parser.c`/`hevc_parser.c`。**`hevc_parser.c` 从未实现过同等能力**，所以现有 HEVC 分支（`feed_input_buffer2`/`feed_input_buffer` 里 `codec_id == AV_CODEC_ID_HEVC` 的判断块）里 `av_packet_get_side_data(...)` 永远返回 NULL，是死代码。用户要求保留这段代码（万一以后 FFmpeg 补上这个能力），本计划在其后面新增一个 `else` 分支作为真正生效的探测路径，不删除、不改动原有逻辑。
- 本会话已经把 `drain_output_buffer_l`/`drain_output_buffer2_l` 里 `AMEDIACODEC__INFO_OUTPUT_FORMAT_CHANGED` 的处理逻辑修好（原来被注释掉，理由是"TI decoder could crash after reconfigure"，已确认该 TI 型号是 `OMX.TI.DUCATI1.`，十年前的老硬件，跟这个项目支持的 Android 15 完全不是一个时代；且新逻辑只更新两个 int 字段 + 发通知，完全不触碰 MediaCodec/Surface 重建，跟当年的崩溃场景无关）。这条路径处理"MediaCodec 硬件自己吃下了分辨率变化"的场景，属于轻量、无缝路径。
- 本计划新增的 NAL 扫描探测器，处理"MediaCodec 吃不下（跨度太大，如 480p→4K）"的场景——由于 `reconfigure_codec_l` 里 `quirk_reconfigure_with_new_codec` 恒为 `true`，触发它必然导致 MediaCodec 实例被完整销毁重建（`SDL_AMediaCodec_decreaseReferenceP` + `create_codec_l` 重新创建），这是真正的"硬重启"路径，天然不受 Adaptive Playback 尺寸上限限制。
- 两条路径共享同一份 `opaque->codecpar->width/height` 状态做比较，谁先探测到变化谁就先更新状态，另一条路径的比较条件自然会因为状态已经同步而不再重复触发，不会打架。

## Global Constraints

- 不改动、不删除现有 `AV_PKT_DATA_NEW_EXTRADATA` side data 探测块（H264 与 HEVC 两处都保留原样）。
- 新增的纯解析逻辑（bit reader、SPS 字段解析、NAL 查找）必须是不依赖 Android/JNI/FFmpeg 头文件的纯 C 代码，能在开发机上用 `clang`/`gcc` 直接编译单测，不需要 NDK。
- 任何解析失败/数据异常（长度不够、NAL 找不到、字段值超出合理范围）一律返回"未探测到变化"，绝不能把垃圾数据传给下游触发 MediaCodec 重建。
- 遵循本文件现有代码风格（4 空格缩进、变量对齐、`ALOGW`/`ALOGI` 日志宏）。

---

## File Structure

- Create: `ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.h` — 对外暴露的探测函数声明
- Create: `ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.c` — bit reader、`profile_tier_level` 跳过逻辑、SPS 宽高解析、NAL 查找，全部纯 C 实现
- Create: `ijkmedia/ijkplayer/android/pipeline/test/test_hevc_sps_probe.c` — 独立于 NDK/CMake 构建之外的 host 单测（用真实构造并校验过的 HEVC SPS NAL 十六进制数据做 fixture）
- Modify: `android/ijkplayer/ijkplayer-cmake/src/main/cpp/CMakeLists.txt:80` — 把 `test/` 目录排除出 `SOURCE_IJKPLAYER`，避免单测文件的 `main()` 被打进产物 `.so`
- Modify: `ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c` — 在 `feed_input_buffer2`/`feed_input_buffer` 的 HEVC 分支里接入新探测器；在 `recreate_format_l` 里接入 `max-width`/`max-height` 提示
- Modify: `ijkmedia/ijkplayer/ff_ffplay_options.h` — 新增 `mediacodec-max-width`/`mediacodec-max-height` 两个 option
- Modify: `ijkmedia/ijkplayer/ff_ffplay_def.h` — 新增对应的 `FFPlayer` 字段与默认值

---

### Task 1: HEVC SPS 宽高探测器（纯 C，host 可单测）

**Files:**
- Create: `ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.h`
- Create: `ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.c`
- Test: `ijkmedia/ijkplayer/android/pipeline/test/test_hevc_sps_probe.c`
- Modify: `android/ijkplayer/ijkplayer-cmake/src/main/cpp/CMakeLists.txt:80`

**Interfaces:**
- Produces: `int ffhevc_probe_sps_size(const uint8_t *data, int size, size_t nal_size, int *out_width, int *out_height);`（Task 2 消费这个函数；`nal_size` 用 `size_t` 是为了跟 `ffpipenode_android_mediacodec_vdec.c:88` 的 `opaque->nal_size`（声明为 `size_t`）以及 `h264_nal.h:113` 的 `convert_h264_to_annexb` 保持同一种类型约定，调用方直接传 `opaque->nal_size` 不需要任何隐式转换）

- [ ] **Step 1: 写头文件**

```c
// ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.h
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

/*
 * 在一段"长度前缀"格式（avcC/hvcC sample 格式：nal_size 字节大端长度 + NAL 数据，
 * 循环重复）的 HEVC packet 里查找第一个 SPS NAL（nal_unit_type == 33），并解析出
 * pic_width_in_luma_samples / pic_height_in_luma_samples。
 *
 * 不依赖 FFmpeg 的 AV_PKT_DATA_NEW_EXTRADATA side data（hevc_parser 不产出这个），
 * 直接扫描 packet 自身的 NAL 结构。
 *
 * 返回 0 并写入 *out_width/*out_height 表示找到且解析成功；
 * 返回 -1 表示没找到 SPS，或者数据异常/不完整（调用者应当跳过这次更新，不能相信
 * 部分解析出的结果）。
 *
 * nal_size 用 size_t（而不是 int）是为了跟本仓库里同一个概念的既有约定保持一致
 * （ffpipenode_android_mediacodec_vdec.c 里的 opaque->nal_size 字段、以及
 * h264_nal.h 里 convert_h264_to_annexb 的 i_nal_size 参数，都是 size_t）。
 */
int ffhevc_probe_sps_size(const uint8_t *data, int size, size_t nal_size,
                           int *out_width, int *out_height);

#endif
```

- [ ] **Step 2: 写实现**

```c
// ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.c
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

        if (len == 0 || pos + (int)len > size)
            break; /* 长度字段不合理，停止扫描，不越界读 */

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
```

- [ ] **Step 3: 写 host 单测（先写测试，确认能编译并且会失败/通过对应预期）**

```c
// ijkmedia/ijkplayer/android/pipeline/test/test_hevc_sps_probe.c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ffp_hevc_sps_probe.h"

/* 下面三段十六进制是用独立脚本手工构造并且用一份镜像了本文件解析逻辑的
 * Python 参考实现解码验证过的合法 HEVC SPS NAL（2 字节 header + RBSP，
 * 4 字节长度前缀在调用处单独给出，不包含在这段十六进制里）：
 *   854x480、1280x720（对应用户的 480p -> 720p 场景）、3840x2160（大跨度场景） */
static const uint8_t SPS_1280x720[] = {
    0x42, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0xa0, 0x02, 0x80, 0x80,
    0x2d, 0x10
};
static const uint8_t SPS_854x480[] = {
    0x42, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0xa0, 0x06, 0xae, 0x01,
    0xe1, 0x00
};
static const uint8_t SPS_3840x2160[] = {
    0x42, 0x01, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
    0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x00, 0xa0, 0x01, 0xe0, 0x20,
    0x02, 0x1c, 0x40
};

/* 把一个裸 NAL 包装成 4 字节长度前缀格式，模拟 avcC/hvcC sample 格式的 packet */
static int wrap_length_prefixed(const uint8_t *nal, int nal_len, uint8_t *out)
{
    out[0] = (uint8_t)((nal_len >> 24) & 0xFF);
    out[1] = (uint8_t)((nal_len >> 16) & 0xFF);
    out[2] = (uint8_t)((nal_len >> 8)  & 0xFF);
    out[3] = (uint8_t)(nal_len & 0xFF);
    memcpy(out + 4, nal, nal_len);
    return 4 + nal_len;
}

static void test_finds_720p(void)
{
    uint8_t pkt[64];
    int     pkt_size = wrap_length_prefixed(SPS_1280x720, sizeof(SPS_1280x720), pkt);
    int     w = 0, h = 0;
    int     ret = ffhevc_probe_sps_size(pkt, pkt_size, 4, &w, &h);
    assert(ret == 0);
    assert(w == 1280);
    assert(h == 720);
    printf("test_finds_720p: OK (%dx%d)\n", w, h);
}

static void test_finds_480p(void)
{
    uint8_t pkt[64];
    int     pkt_size = wrap_length_prefixed(SPS_854x480, sizeof(SPS_854x480), pkt);
    int     w = 0, h = 0;
    int     ret = ffhevc_probe_sps_size(pkt, pkt_size, 4, &w, &h);
    assert(ret == 0);
    assert(w == 854);
    assert(h == 480);
    printf("test_finds_480p: OK (%dx%d)\n", w, h);
}

static void test_finds_large_jump_4k(void)
{
    uint8_t pkt[64];
    int     pkt_size = wrap_length_prefixed(SPS_3840x2160, sizeof(SPS_3840x2160), pkt);
    int     w = 0, h = 0;
    int     ret = ffhevc_probe_sps_size(pkt, pkt_size, 4, &w, &h);
    assert(ret == 0);
    assert(w == 3840);
    assert(h == 2160);
    printf("test_finds_large_jump_4k: OK (%dx%d)\n", w, h);
}

static void test_no_sps_returns_error(void)
{
    /* 包一个非 SPS 的假 NAL（type=1，普通切片），必须探测不到任何宽高 */
    uint8_t fake_slice_nal[] = {0x02, 0x01, 0xaa, 0xbb, 0xcc};
    uint8_t pkt[64];
    int     pkt_size = wrap_length_prefixed(fake_slice_nal, sizeof(fake_slice_nal), pkt);
    int     w = 0, h = 0;
    int     ret = ffhevc_probe_sps_size(pkt, pkt_size, 4, &w, &h);
    assert(ret == -1);
    printf("test_no_sps_returns_error: OK\n");
}

static void test_truncated_packet_returns_error(void)
{
    uint8_t pkt[8] = {0x00, 0x00, 0x00, 0x1a, 0x42, 0x01}; /* 声明了 26 字节但只给了 2 字节 */
    int     w = 0, h = 0;
    int     ret = ffhevc_probe_sps_size(pkt, sizeof(pkt), 4, &w, &h);
    assert(ret == -1);
    printf("test_truncated_packet_returns_error: OK\n");
}

static void test_resolution_change_854x480_to_1280x720(void)
{
    /* 直接模拟用户的场景：先探测到 480p 的 SPS，再探测到 720p 的 SPS，
     * 两次结果必须不同，调用方才会判定为"分辨率变了" */
    uint8_t pkt1[64], pkt2[64];
    int     pkt1_size = wrap_length_prefixed(SPS_854x480, sizeof(SPS_854x480), pkt1);
    int     pkt2_size = wrap_length_prefixed(SPS_1280x720, sizeof(SPS_1280x720), pkt2);
    int     w1 = 0, h1 = 0, w2 = 0, h2 = 0;

    assert(ffhevc_probe_sps_size(pkt1, pkt1_size, 4, &w1, &h1) == 0);
    assert(ffhevc_probe_sps_size(pkt2, pkt2_size, 4, &w2, &h2) == 0);
    assert(w1 != w2 || h1 != h2);
    printf("test_resolution_change_854x480_to_1280x720: OK (%dx%d -> %dx%d)\n", w1, h1, w2, h2);
}

int main(void)
{
    test_finds_720p();
    test_finds_480p();
    test_finds_large_jump_4k();
    test_no_sps_returns_error();
    test_truncated_packet_returns_error();
    test_resolution_change_854x480_to_1280x720();
    printf("ALL TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 4: 编译并运行单测，确认全部通过**

Run:
```bash
cd /Users/dev/Documents/AV/ijkplayer_64kb
clang -std=c99 -Wall -Wextra \
  ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.c \
  ijkmedia/ijkplayer/android/pipeline/test/test_hevc_sps_probe.c \
  -o /tmp/test_hevc_sps_probe
/tmp/test_hevc_sps_probe
```

Expected: 编译无警告，运行输出 6 行 `OK`，最后一行 `ALL TESTS PASSED`，进程退出码 0。

- [ ] **Step 5: 把单测目录排除出 Android 正式构建**

Modify `android/ijkplayer/ijkplayer-cmake/src/main/cpp/CMakeLists.txt`，紧跟在第 80 行 `SOURCE_IJKPLAYER` 的 `file(GLOB_RECURSE ...)` 之后插入一行：

```cmake
file(GLOB_RECURSE SOURCE_IJKPLAYER ${ijkmedia}/ijkplayer/*.c ${ijkmedia}/ijkplayer/*.cpp)
list(FILTER SOURCE_IJKPLAYER EXCLUDE REGEX ".*/pipeline/test/.*")
```

- [ ] **Step 6: 用真实 NDK 工具链编译一次 aar，确认新文件被正确收进 native 库、且没有把 test 目录带进去**

Run:
```bash
cd /Users/dev/Documents/AV/ijkplayer_64kb/android && ./build-aar.sh
```
Expected: `BUILD SUCCESSFUL`。

- [ ] **Step 7: 提交**

```bash
cd /Users/dev/Documents/AV/ijkplayer_64kb
git add ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.h \
        ijkmedia/ijkplayer/android/pipeline/ffp_hevc_sps_probe.c \
        ijkmedia/ijkplayer/android/pipeline/test/test_hevc_sps_probe.c \
        android/ijkplayer/ijkplayer-cmake/src/main/cpp/CMakeLists.txt
git commit -m "add HEVC SPS resolution probe with host-testable unit tests"
```

---

### Task 2: 接入 feed_input_buffer2 / feed_input_buffer 的 HEVC 分支

**Files:**
- Modify: `ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c`

**Interfaces:**
- Consumes: `int ffhevc_probe_sps_size(const uint8_t *data, int size, int nal_size, int *out_width, int *out_height);`（Task 1 产出）

- [ ] **Step 1: 加 include**

在文件顶部现有 `#include` 列表末尾（紧跟其他本地头文件之后）加一行：
```c
#include "ffp_hevc_sps_probe.h"
```

- [ ] **Step 2: 在 `feed_input_buffer2` 里，紧跟已保留的 side-data 探测块之后，加一个 `else` 分支**

找到这一段（当前保留、但 `size_data` 永远是 NULL 的代码块）——**这段文字在文件里逐字出现两次**：一次在 `feed_input_buffer2` 内，一次在 `feed_input_buffer` 内，两处完全一模一样（已用 `grep -c` 核实过）：

```c
        if (opaque->ffp->mediacodec_handle_resolution_change &&
            opaque->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            uint8_t  *size_data      = NULL;
            int       size_data_size = 0;
            AVPacket *avpkt          = &d->pkt_temp;
            size_data = av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA, &size_data_size);
            // hvcC 固定头部至少 23 字节（比 avcC 大得多），这里只是避免把明显过短的
            // 垃圾数据传给 avcodec_open2，真正的合法性校验交给 avcodec_open2/avcodec_decode_video2 的返回值判断
            if (size_data && size_data_size >= 23) {
                int             got_picture = 0;
                AVFrame        *frame      = av_frame_alloc();
                AVDictionary   *codec_opts = NULL;
                const AVCodec  *codec      = opaque->decoder->avctx->codec;
                AVCodecContext *new_avctx  = avcodec_alloc_context3(codec);
                int change_ret = 0;

                if (!new_avctx)
                    return AVERROR(ENOMEM);

                avcodec_parameters_to_context(new_avctx, opaque->codecpar);
                av_freep(&new_avctx->extradata);
                new_avctx->extradata = av_mallocz(size_data_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (!new_avctx->extradata) {
                    avcodec_free_context(&new_avctx);
                    return AVERROR(ENOMEM);
                }
                memcpy(new_avctx->extradata, size_data, size_data_size);
                new_avctx->extradata_size = size_data_size;

                av_dict_set(&codec_opts, "threads", "1", 0);
                change_ret = avcodec_open2(new_avctx, codec, &codec_opts);
                av_dict_free(&codec_opts);
                if (change_ret < 0) {
                    avcodec_free_context(&new_avctx);
                    return change_ret;
                }

                change_ret = avcodec_decode_video2(new_avctx, frame, &got_picture, avpkt);
                if (change_ret < 0) {
                    avcodec_free_context(&new_avctx);
                    return change_ret;
                } else {
                    if (opaque->codecpar->width  != new_avctx->width &&
                        opaque->codecpar->height != new_avctx->height) {
                        ALOGW("AV_PKT_DATA_NEW_EXTRADATA(HEVC): %d x %d\n", new_avctx->width, new_avctx->height);
                        avcodec_parameters_from_context(opaque->codecpar, new_avctx);
                        opaque->aformat_need_recreate = true;
                        ffpipeline_set_surface_need_reconfigure_l(pipeline, true);
                    }
                }

                av_frame_unref(frame);
                avcodec_free_context(&new_avctx);
            }
        }
```

改成（`if (size_data ...)` 内部原有代码完全不动，只在其后追加一个 `else` 分支）：

```c
        if (opaque->ffp->mediacodec_handle_resolution_change &&
            opaque->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            uint8_t  *size_data      = NULL;
            int       size_data_size = 0;
            AVPacket *avpkt          = &d->pkt_temp;
            size_data = av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA, &size_data_size);
            // hvcC 固定头部至少 23 字节（比 avcC 大得多），这里只是避免把明显过短的
            // 垃圾数据传给 avcodec_open2，真正的合法性校验交给 avcodec_open2/avcodec_decode_video2 的返回值判断
            if (size_data && size_data_size >= 23) {
                int             got_picture = 0;
                AVFrame        *frame      = av_frame_alloc();
                AVDictionary   *codec_opts = NULL;
                const AVCodec  *codec      = opaque->decoder->avctx->codec;
                AVCodecContext *new_avctx  = avcodec_alloc_context3(codec);
                int change_ret = 0;

                if (!new_avctx)
                    return AVERROR(ENOMEM);

                avcodec_parameters_to_context(new_avctx, opaque->codecpar);
                av_freep(&new_avctx->extradata);
                new_avctx->extradata = av_mallocz(size_data_size + AV_INPUT_BUFFER_PADDING_SIZE);
                if (!new_avctx->extradata) {
                    avcodec_free_context(&new_avctx);
                    return AVERROR(ENOMEM);
                }
                memcpy(new_avctx->extradata, size_data, size_data_size);
                new_avctx->extradata_size = size_data_size;

                av_dict_set(&codec_opts, "threads", "1", 0);
                change_ret = avcodec_open2(new_avctx, codec, &codec_opts);
                av_dict_free(&codec_opts);
                if (change_ret < 0) {
                    avcodec_free_context(&new_avctx);
                    return change_ret;
                }

                change_ret = avcodec_decode_video2(new_avctx, frame, &got_picture, avpkt);
                if (change_ret < 0) {
                    avcodec_free_context(&new_avctx);
                    return change_ret;
                } else {
                    if (opaque->codecpar->width  != new_avctx->width &&
                        opaque->codecpar->height != new_avctx->height) {
                        ALOGW("AV_PKT_DATA_NEW_EXTRADATA(HEVC): %d x %d\n", new_avctx->width, new_avctx->height);
                        avcodec_parameters_from_context(opaque->codecpar, new_avctx);
                        opaque->aformat_need_recreate = true;
                        ffpipeline_set_surface_need_reconfigure_l(pipeline, true);
                    }
                }

                av_frame_unref(frame);
                avcodec_free_context(&new_avctx);
            } else {
                // side data 这条路对 HEVC 目前恒为 NULL（FFmpeg 的 hevc_parser 不产出这个
                // side data），这里用不依赖 FFmpeg parser 的 NAL 扫描直接探测新 SPS 的分辨率
                int new_width = 0, new_height = 0;
                if (ffhevc_probe_sps_size(avpkt->data, avpkt->size, opaque->nal_size,
                                           &new_width, &new_height) == 0 &&
                    (new_width != opaque->codecpar->width || new_height != opaque->codecpar->height)) {
                    ALOGW("HEVC SPS scan: resolution changed %dx%d -> %dx%d\n",
                          opaque->codecpar->width, opaque->codecpar->height, new_width, new_height);
                    opaque->codecpar->width  = new_width;
                    opaque->codecpar->height = new_height;
                    opaque->aformat_need_recreate = true;
                    ffpipeline_set_surface_need_reconfigure_l(pipeline, true);
                }
            }
        }
```

- [ ] **Step 3: 对第二处（`feed_input_buffer` 函数体内）做一模一样的改动**

Step 2 里"改成"的那段文本，在文件里要出现两次替换（`feed_input_buffer2` 一次、`feed_input_buffer` 一次）。因为两处"改前"文本逐字相同，直接用支持"全部替换"的编辑方式（比如 Edit 工具的 `replace_all: true`）一次性替换两处即可，不需要分别手工定位两次。改完用下面命令确认改动确实生效在两个位置：

```bash
grep -n "HEVC SPS scan: resolution changed" ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c
```

Expected: 输出两行，分别在 `feed_input_buffer2` 和 `feed_input_buffer` 各一处。

- [ ] **Step 4: 编译验证**

Run:
```bash
cd /Users/dev/Documents/AV/ijkplayer_64kb/android && ./build-aar.sh
```
Expected: `BUILD SUCCESSFUL`，无新增警告。

- [ ] **Step 5: 提交**

```bash
cd /Users/dev/Documents/AV/ijkplayer_64kb
git add ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c
git commit -m "wire HEVC SPS scan probe into mediacodec feed_input_buffer paths"
```

---

### Task 3: MediaFormat 增加 max-width/max-height 提示

**Files:**
- Modify: `ijkmedia/ijkplayer/ff_ffplay_options.h`
- Modify: `ijkmedia/ijkplayer/ff_ffplay_def.h`
- Modify: `ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c`

**Interfaces:**
- Produces: `ffp->mediacodec_max_width` / `ffp->mediacodec_max_height`（`int`，0 表示不设置，供 App 层通过 `ijkmp_set_option_int` 配置）

- [ ] **Step 1: 加两个新 option**

在 `ijkmedia/ijkplayer/ff_ffplay_options.h` 里，紧跟 `mediacodec-handle-resolution-change` 那一项之后加：

```c
    { "mediacodec-max-width",                   "MediaCodec: hint max width for adaptive playback (0 = unset)",
        OPTION_OFFSET(mediacodec_max_width),     OPTION_INT(0, 0, 7680) },
    { "mediacodec-max-height",                  "MediaCodec: hint max height for adaptive playback (0 = unset)",
        OPTION_OFFSET(mediacodec_max_height),    OPTION_INT(0, 0, 4320) },
```

- [ ] **Step 2: 加对应的 FFPlayer 字段和默认值**

在 `ijkmedia/ijkplayer/ff_ffplay_def.h` 里，紧跟 `int mediacodec_handle_resolution_change;` 那一行之后加：
```c
    int mediacodec_max_width;
    int mediacodec_max_height;
```
再找到 `ffp->mediacodec_handle_resolution_change = 0; // option` 那一行，紧跟其后加：
```c
    ffp->mediacodec_max_width  = 0; // option
    ffp->mediacodec_max_height = 0; // option
```

- [ ] **Step 3: 在 `recreate_format_l` 里把这两个值写进 MediaFormat**

在 `ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c` 的 `recreate_format_l` 函数里，找到这一段（处理旋转角度、紧邻函数末尾 `return 0;` 之前）：
```c
    } else {
        ALOGI("amc: rotate notify: %d\n", rotate_degrees);
        ffp_notify_msg2(ffp, FFP_MSG_VIDEO_ROTATION_CHANGED, rotate_degrees);
    }

    return 0;
fail:
    return -1;
}
```
改成：
```c
    } else {
        ALOGI("amc: rotate notify: %d\n", rotate_degrees);
        ffp_notify_msg2(ffp, FFP_MSG_VIDEO_ROTATION_CHANGED, rotate_degrees);
    }

    if (ffp->mediacodec_max_width > 0 && ffp->mediacodec_max_height > 0) {
        ALOGI("amc: hint max-width/max-height: %d x %d\n",
              ffp->mediacodec_max_width, ffp->mediacodec_max_height);
        SDL_AMediaFormat_setInt32(opaque->input_aformat, "max-width",  ffp->mediacodec_max_width);
        SDL_AMediaFormat_setInt32(opaque->input_aformat, "max-height", ffp->mediacodec_max_height);
    }

    return 0;
fail:
    return -1;
}
```

- [ ] **Step 4: 编译验证**

Run:
```bash
cd /Users/dev/Documents/AV/ijkplayer_64kb/android && ./build-aar.sh
```
Expected: `BUILD SUCCESSFUL`。

- [ ] **Step 5: 提交**

```bash
cd /Users/dev/Documents/AV/ijkplayer_64kb
git add ijkmedia/ijkplayer/ff_ffplay_options.h ijkmedia/ijkplayer/ff_ffplay_def.h \
        ijkmedia/ijkplayer/android/pipeline/ffpipenode_android_mediacodec_vdec.c
git commit -m "add mediacodec-max-width/max-height hints for adaptive playback"
```

---

### Task 4: 真机验证（这段代码强依赖硬件，无法纯自动化，必须手工过一遍）

**Files:** 无代码改动，纯验证步骤。

- [ ] **Step 1: 准备一路会中途切换分辨率的 HEVC 直播源**

如果没有现成的摄像头可测，可以用 ffmpeg 手动拼一路测试流：先推 480p 的 HEVC 切片若干秒，再无缝切到 720p（`ffmpeg -re -i 480p.mp4 ... ; ffmpeg -re -i 720p.mp4 ...` 推到同一个 RTMP/RTSP 地址，中途切换输入文件），或者直接问业务方要一路真实会切换分辨率的监控流地址。

- [ ] **Step 2: 打开 `mediacodec-hevc` 和 `mediacodec-handle-resolution-change` 两个 option**

在 Demo App 或者自己的测试代码里，起播前设置：
```java
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "mediacodec-hevc", 1);
ijkMediaPlayer.setOption(IjkMediaPlayer.OPT_CATEGORY_PLAYER, "mediacodec-handle-resolution-change", 1);
```

- [ ] **Step 3: 起播，用 `adb logcat` 过滤观察日志**

Run:
```bash
adb logcat | grep -E "HEVC SPS scan|AMEDIACODEC__INFO_OUTPUT_FORMAT_CHANGED: resolution changed|amc: recreate aformat|quirk: reconfigure with new codec"
```

Expected：
- 分辨率切换那一刻，应该能看到 `HEVC SPS scan: resolution changed 854x480 -> 1280x720`（或者是 `INFO_OUTPUT_FORMAT_CHANGED` 那条日志，取决于走了哪条路径）
- 紧接着能看到 `amc: recreate aformat` 和 `quirk: reconfigure with new codec`
- 画面在重建瞬间可能有 1~2 帧的短暂黑屏/花屏，但重建完成后画面应该立刻恢复正常、清晰度对应新分辨率，不应该出现持续花屏、卡死或者 App 崩溃

- [ ] **Step 4: 如果测试环境能拿到跨度更大的分辨率跳变（比如 480p -> 4K/3K）源，重复第 3 步验证大跳变场景**

Expected：同上，`HEVC SPS scan` 路径应该在这种大跳变场景下稳定生效（这条路径不依赖硬件 Adaptive Playback 能力，理论上不受跳变幅度影响）。

- [ ] **Step 5: 记录验证结果**

如果一切正常，在 PR 描述或者 commit message 里补一句验证记录（用的什么测试流、什么设备型号、观察到的日志和画面表现），方便以后回溯。如果发现问题，回到 Task 1/2 对应位置调整。
