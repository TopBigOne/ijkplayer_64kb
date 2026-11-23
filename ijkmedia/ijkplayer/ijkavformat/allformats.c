/*
 * Copyright (c) 2003 Bilibili
 * Copyright (c) 2003 Fabrice Bellard
 * Copyright (c) 2015 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 * Based on libavformat/allformats.c
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavformat/avformat.h"
#include "libavformat/demux.h"
#include "libavformat/url.h"
#include "libavformat/version.h"

/*
 * FFmpeg 7：demuxer/protocol 列表在编译期固定，不再支持运行时 av_register_input_format。
 * FFToolChain/patches/ffmpeg-n7.1.1 在 FFmpeg 内部预留了同名占位（ff_ijkxxx_protocol /
 * ff_ijkxxx_demuxer）并导出 ijkav_register_xxx_*()，这里把 ijkmedia 的真实实现 memcpy 覆盖进去。
 * 注意 demuxer 的结构体已改为内部的 FFInputFormat（大小校验也按 FFInputFormat）。
 */
#define IJK_REGISTER_DEMUXER(x)                                         \
    {                                                                   \
        extern FFInputFormat ijkff_##x##_demuxer;                       \
        int ijkav_register_##x##_demuxer(FFInputFormat *demuxer, int demuxer_size);   \
        ijkav_register_##x##_demuxer(&ijkff_##x##_demuxer, sizeof(FFInputFormat));    \
    }

#define IJK_REGISTER_PROTOCOL(x)                                        \
    {                                                                   \
        extern URLProtocol ijkimp_ff_##x##_protocol;                        \
        int ijkav_register_##x##_protocol(URLProtocol *protocol, int protocol_size);\
        ijkav_register_##x##_protocol(&ijkimp_ff_##x##_protocol, sizeof(URLProtocol));  \
    }

void ijkav_register_all(void)
{
    static int initialized;

    if (initialized)
        return;
    initialized = 1;

    /* protocols */
    av_log(NULL, AV_LOG_INFO, "===== custom modules begin =====\n");
#ifdef __ANDROID__
    IJK_REGISTER_PROTOCOL(ijkmediadatasource);
#endif
    IJK_REGISTER_PROTOCOL(ijkio);
    /*
     * async：FFmpeg 7 自带官方 async 协议，补丁未预留 ijk 版的注册入口，直接使用官方实现
     *        （ijk 版 ijkasync.c 额外上报的 async 缓冲统计 FFP_PROP_INT64_ASYNC_STATISTIC_* 将无数据）。
     * ijklongurl：FFmpeg 4.1+ 的 URL 已改为动态长度（AVFormatContext.url），不再需要长 URL 改写。
     */
    IJK_REGISTER_PROTOCOL(ijktcphook);
    IJK_REGISTER_PROTOCOL(ijkhttphook);
    IJK_REGISTER_PROTOCOL(ijksegment);
    /* demuxers */
    IJK_REGISTER_DEMUXER(ijklivehook);
    IJK_REGISTER_DEMUXER(ijklas);
    av_log(NULL, AV_LOG_INFO, "===== custom modules end =====\n");
}
