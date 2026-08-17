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

/* VPS/PPS fixture：ffhevc_find_nal_public 只按 nal_unit_type 定位 NAL、不解析负载，
 * 所以这两段只需要 2 字节合法的 HEVC NAL header（type 写在第 1 字节的 bit6..1），
 * 后面的字节是任意占位数据。
 *   VPS: type=32 -> (32 << 1) = 0x40
 *   PPS: type=34 -> (34 << 1) = 0x44 */
static const uint8_t VPS_STUB[] = {
    0x40, 0x01, 0x0c, 0x01, 0xff, 0xff
};
static const uint8_t PPS_STUB[] = {
    0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40
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

/* 把一个裸 NAL 以 4 字节长度前缀格式追加到 out + offset 处，返回新的 offset，
 * 用来拼出"一个 packet 里带多个 NAL"的场景 */
static int append_length_prefixed(const uint8_t *nal, int nal_len, uint8_t *out, int offset)
{
    return offset + wrap_length_prefixed(nal, nal_len, out + offset);
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

static void test_malformed_length_field_overflow(void)
{
    /* 回归测试：长度字段声明 0x80000005（大于 INT_MAX），
     * 在直接转 int 时会变成负数 -2147483643，绕过旧的边界检查。
     * 新的边界检查用无符号比较应该安全地拒绝这个。 */
    uint8_t pkt[64];
    int     w = 0, h = 0;
    pkt[0] = 0x80;
    pkt[1] = 0x00;
    pkt[2] = 0x00;
    pkt[3] = 0x05;
    /* 在 4 字节长度后放一些垃圾数据，但不足 0x80000005 字节 */
    memset(pkt + 4, 0xAA, sizeof(pkt) - 4);
    int ret = ffhevc_probe_sps_size(pkt, sizeof(pkt), 4, &w, &h);
    assert(ret == -1);
    printf("test_malformed_length_field_overflow: OK\n");
}

static void test_length_exceeds_remaining_buffer(void)
{
    /* 回归测试：长度字段声明 1000 字节（技术上可以放进 int），
     * 但实际 packet 只有 20 字节。边界检查应该安全地拒绝这个。 */
    uint8_t pkt[20];
    int     w = 0, h = 0;
    /* 1000 = 0x000003e8，放在 4 字节大端长度字段 */
    pkt[0] = 0x00;
    pkt[1] = 0x00;
    pkt[2] = 0x03;
    pkt[3] = 0xe8;
    /* 后面是垃圾数据 */
    memset(pkt + 4, 0xBB, sizeof(pkt) - 4);
    int ret = ffhevc_probe_sps_size(pkt, sizeof(pkt), 4, &w, &h);
    assert(ret == -1);
    printf("test_length_exceeds_remaining_buffer: OK\n");
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

static void test_find_nal_public_finds_all_three(void)
{
    /* 一个 packet 里同时带 VPS + SPS + PPS（编码器重发完整参数集的典型场景），
     * 三种类型都必须能被定位到，返回的指针和长度要跟原始 NAL 逐字节一致 */
    uint8_t pkt[128];
    int     pkt_size = 0;
    const uint8_t *vps, *sps, *pps;
    int     vps_size = 0, sps_size = 0, pps_size = 0;

    pkt_size = append_length_prefixed(VPS_STUB,      sizeof(VPS_STUB),      pkt, pkt_size);
    pkt_size = append_length_prefixed(SPS_1280x720,  sizeof(SPS_1280x720),  pkt, pkt_size);
    pkt_size = append_length_prefixed(PPS_STUB,      sizeof(PPS_STUB),      pkt, pkt_size);

    vps = ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_VPS, &vps_size);
    sps = ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_SPS, &sps_size);
    pps = ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_PPS, &pps_size);

    assert(vps != NULL);
    assert(vps_size == (int)sizeof(VPS_STUB));
    assert(memcmp(vps, VPS_STUB, sizeof(VPS_STUB)) == 0);

    assert(sps != NULL);
    assert(sps_size == (int)sizeof(SPS_1280x720));
    assert(memcmp(sps, SPS_1280x720, sizeof(SPS_1280x720)) == 0);

    assert(pps != NULL);
    assert(pps_size == (int)sizeof(PPS_STUB));
    assert(memcmp(pps, PPS_STUB, sizeof(PPS_STUB)) == 0);

    /* 返回的 NAL 必须落在 packet 缓冲区内部（不越界） */
    assert(vps >= pkt && vps + vps_size <= pkt + pkt_size);
    assert(sps >= pkt && sps + sps_size <= pkt + pkt_size);
    assert(pps >= pkt && pps + pps_size <= pkt + pkt_size);

    printf("test_find_nal_public_finds_all_three: OK (vps=%d sps=%d pps=%d)\n",
           vps_size, sps_size, pps_size);
}

static void test_find_nal_public_missing_type_returns_null(void)
{
    /* 只带 SPS 的 packet（编码器只重发 SPS 的常见场景）：SPS 能找到，
     * VPS/PPS 必须返回 NULL，调用方据此退回"只更新宽高"的安全路径 */
    uint8_t pkt[64];
    int     pkt_size = wrap_length_prefixed(SPS_854x480, sizeof(SPS_854x480), pkt);
    int     out_size = 0;

    assert(ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_SPS, &out_size) != NULL);
    assert(out_size == (int)sizeof(SPS_854x480));
    assert(ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_VPS, &out_size) == NULL);
    assert(ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_PPS, &out_size) == NULL);
    printf("test_find_nal_public_missing_type_returns_null: OK\n");
}

static void test_find_nal_public_rejects_bad_args(void)
{
    uint8_t pkt[64];
    int     pkt_size = wrap_length_prefixed(SPS_1280x720, sizeof(SPS_1280x720), pkt);
    int     out_size = 0;

    assert(ffhevc_find_nal_public(NULL, pkt_size, 4, FFHEVC_NAL_TYPE_SPS, &out_size) == NULL);
    assert(ffhevc_find_nal_public(pkt, 0, 4, FFHEVC_NAL_TYPE_SPS, &out_size) == NULL);
    assert(ffhevc_find_nal_public(pkt, -1, 4, FFHEVC_NAL_TYPE_SPS, &out_size) == NULL);
    assert(ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_SPS, NULL) == NULL);
    /* nal_size 只可能是 1~4，0（Annex-B 流的情况）和 5 都必须被拒绝 */
    assert(ffhevc_find_nal_public(pkt, pkt_size, 0, FFHEVC_NAL_TYPE_SPS, &out_size) == NULL);
    assert(ffhevc_find_nal_public(pkt, pkt_size, 5, FFHEVC_NAL_TYPE_SPS, &out_size) == NULL);
    printf("test_find_nal_public_rejects_bad_args: OK\n");
}

static void test_find_nal_public_truncated_returns_null(void)
{
    /* 声明了 26 字节但只给了 2 字节，绝不能越界读，必须返回 NULL */
    uint8_t pkt[8] = {0x00, 0x00, 0x00, 0x1a, 0x42, 0x01};
    int     out_size = 0;

    assert(ffhevc_find_nal_public(pkt, sizeof(pkt), 4, FFHEVC_NAL_TYPE_SPS, &out_size) == NULL);
    printf("test_find_nal_public_truncated_returns_null: OK\n");
}

static void test_find_nal_public_agrees_with_probe(void)
{
    /* 从 packet 里定位到的 SPS 单独再喂给 ffhevc_probe_sps_size（包一层长度前缀），
     * 解析出来的宽高必须跟直接在整个 packet 上探测的结果一致 */
    uint8_t pkt[128], repacked[64];
    int     pkt_size = 0, repacked_size = 0;
    const uint8_t *sps;
    int     sps_size = 0;
    int     w1 = 0, h1 = 0, w2 = 0, h2 = 0;

    pkt_size = append_length_prefixed(VPS_STUB,       sizeof(VPS_STUB),       pkt, pkt_size);
    pkt_size = append_length_prefixed(SPS_3840x2160,  sizeof(SPS_3840x2160),  pkt, pkt_size);
    pkt_size = append_length_prefixed(PPS_STUB,       sizeof(PPS_STUB),       pkt, pkt_size);

    assert(ffhevc_probe_sps_size(pkt, pkt_size, 4, &w1, &h1) == 0);

    sps = ffhevc_find_nal_public(pkt, pkt_size, 4, FFHEVC_NAL_TYPE_SPS, &sps_size);
    assert(sps != NULL);
    repacked_size = wrap_length_prefixed(sps, sps_size, repacked);
    assert(ffhevc_probe_sps_size(repacked, repacked_size, 4, &w2, &h2) == 0);

    assert(w1 == w2 && h1 == h2);
    assert(w1 == 3840 && h1 == 2160);
    printf("test_find_nal_public_agrees_with_probe: OK (%dx%d)\n", w1, h1);
}

int main(void)
{
    test_finds_720p();
    test_finds_480p();
    test_finds_large_jump_4k();
    test_no_sps_returns_error();
    test_truncated_packet_returns_error();
    test_malformed_length_field_overflow();
    test_length_exceeds_remaining_buffer();
    test_resolution_change_854x480_to_1280x720();
    test_find_nal_public_finds_all_three();
    test_find_nal_public_missing_type_returns_null();
    test_find_nal_public_rejects_bad_args();
    test_find_nal_public_truncated_returns_null();
    test_find_nal_public_agrees_with_probe();
    printf("ALL TESTS PASSED\n");
    return 0;
}
