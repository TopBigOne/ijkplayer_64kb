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
