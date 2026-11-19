/*
 * ijkws 选项测试：握手 Header（ws_headers / user_agent / ws_origin / ws_protocol）、
 * 选项消费规则、可选证书校验（ws_tls_verify / ws_ca_file），以及 IP 字面量的 SNI 与主机名校验。
 *
 * 证书（run.sh 生成）：
 *   server.crt    CN/SAN = localhost；客户端带 SNI 时服务器出示它
 *   server-ip.crt SAN 只有 IP:::1；客户端不带 SNI 时服务器出示它
 *   other-ca.crt  无关的 CA
 */
#include <errno.h>

#include "testutil.h"
#include "libavutil/dict.h"

#define WS_BASE  "ws://127.0.0.1:18765"
#define WSS_PORT "18766"

/* 打开 url，读完全部数据并与 test.flv 比较 */
static void read_expect_flv(IjkWsContext **pctx)
{
    char path[1024];
    uint8_t *flv = NULL, *got = NULL;
    size_t flv_len = 0, got_len = 0;
    int ret;
    CHECK_EQ_INT(load_file(test_path(path, sizeof(path), "test.flv"), &flv, &flv_len), 0);
    ret = read_all(*pctx, &got, &got_len);
    ijkws_close(pctx);
    if (got_len != flv_len || memcmp(got, flv, flv_len) != 0) {
        fprintf(stderr, "  got %zu bytes, expect %zu\n", got_len, flv_len);
        free(got);
        free(flv);
        CHECK(!"bytes differ");
    }
    free(got);
    free(flv);
    CHECK_EQ_INT(ret, AVERROR_EOF);
}

/* 找 query 对应的 connect 日志行 */
static int connect_line(const char *query, char *line, size_t size)
{
    int conn = log_conn_for_query(query);
    if (conn <= 0)
        return 0;
    return log_wait_event(conn, "connect", 1000, line, size);
}

static void test_headers_forwarded(void)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    char line[16384];

    av_dict_set(&opts, "ws_headers", "X-Token: t1\r\nX-Dev: 9", 0);
    av_dict_set(&opts, "user_agent", "UA/1", 0);
    av_dict_set(&opts, "ws_origin", "http://o.example", 0);
    av_dict_set(&opts, "ws_protocol", "flv", 0);
    CHECK_EQ_INT(ijkws_open(&ctx, WS_BASE "/flv?token=abc&ch=2", &opts, NULL), 0);
    av_dict_free(&opts);
    /* 服务器回了同一个子协议，握手成功后照常收流 */
    read_expect_flv(&ctx);
    if (g_test_cur_failed)
        return;

    CHECK(connect_line("token=abc&ch=2", line, sizeof(line)));
    CHECK(strstr(line, "\"request_line\": \"GET /flv?token=abc&ch=2 HTTP/1.1\""));
    CHECK(strstr(line, "[\"X-Token\", \"t1\"]"));
    CHECK(strstr(line, "[\"X-Dev\", \"9\"]"));
    CHECK(strstr(line, "[\"User-Agent\", \"UA/1\"]"));
    /* ws_origin 原样发送（lws 默认会改写成 http(s)://<origin>） */
    CHECK(strstr(line, "[\"Origin\", \"http://o.example\"]"));
    CHECK(strstr(line, "[\"Sec-WebSocket-Protocol\", \"flv\"]"));
}

/* 没给 ws_origin / user_agent 时不发 Origin、User-Agent（RFC 6455：非浏览器客户端可以不带 Origin） */
static void test_no_origin_by_default(void)
{
    IjkWsContext *ctx = NULL;
    char url[256], query[64], line[16384];
    snprintf(query, sizeof(query), "noorigin=%lld", (long long)now_ms());
    snprintf(url, sizeof(url), WS_BASE "/flv?%s", query);
    CHECK_EQ_INT(ijkws_open(&ctx, url, NULL, NULL), 0);
    ijkws_close(&ctx);
    CHECK(connect_line(query, line, sizeof(line)));
    CHECK(!strstr(line, "\"Origin\""));
    CHECK(!strstr(line, "\"User-Agent\""));
    CHECK(!strstr(line, "\"Sec-WebSocket-Protocol\""));
}

/* ws_headers 支持结尾多余的 \r\n、空行和单独的 \n 分隔 */
static void test_headers_lenient_separators(void)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    char url[256], query[64], line[16384];
    snprintf(query, sizeof(query), "hdrsep=%lld", (long long)now_ms());
    snprintf(url, sizeof(url), WS_BASE "/flv?%s", query);
    av_dict_set(&opts, "ws_headers", "X-A: 1\r\n\r\nX-B: 2\nX-C: 3\r\n", 0);
    CHECK_EQ_INT(ijkws_open(&ctx, url, &opts, NULL), 0);
    av_dict_free(&opts);
    ijkws_close(&ctx);
    CHECK(connect_line(query, line, sizeof(line)));
    CHECK(strstr(line, "[\"X-A\", \"1\"]"));
    CHECK(strstr(line, "[\"X-B\", \"2\"]"));
    CHECK(strstr(line, "[\"X-C\", \"3\"]"));
}

static void test_opts_consumed(void)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    AVDictionaryEntry *e = NULL;
    av_dict_set(&opts, "ws_headers", "X-Token: t1", 0);
    av_dict_set(&opts, "user_agent", "UA/1", 0);
    av_dict_set(&opts, "ws_origin", "http://o.example", 0);
    av_dict_set(&opts, "ws_protocol", "flv", 0);
    av_dict_set(&opts, "ws_timeout", "5000", 0);
    av_dict_set(&opts, "ws_tls_verify", "0", 0);
    av_dict_set(&opts, "ws_ca_file", "/nonexistent.pem", 0);
    av_dict_set(&opts, "probesize", "4096", 0);
    CHECK_EQ_INT(ijkws_open(&ctx, WS_BASE "/flv", &opts, NULL), 0);
    ijkws_close(&ctx);
    while ((e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strncmp(e->key, "ws_", 3)) {
            fprintf(stderr, "  %s not consumed\n", e->key);
            av_dict_free(&opts);
            CHECK(!"ws_ option left in opts");
        }
    }
    e = av_dict_get(opts, "user_agent", NULL, 0);
    CHECK(e && !strcmp(e->value, "UA/1"));
    CHECK(av_dict_get(opts, "probesize", NULL, 0));
    CHECK_EQ_INT(av_dict_count(opts), 2);
    av_dict_free(&opts);
}

/* 失败时也要消费 ws_* 选项 */
static void test_opts_consumed_on_failure(void)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    av_dict_set(&opts, "ws_tls_verify", "1", 0);
    av_dict_set(&opts, "ws_protocol", "flv", 0);
    av_dict_set(&opts, "user_agent", "UA/1", 0);
    CHECK(ijkws_open(&ctx, "wss://localhost:" WSS_PORT "/flv", &opts, NULL) < 0);
    CHECK(!av_dict_get(opts, "ws_tls_verify", NULL, 0));
    CHECK(!av_dict_get(opts, "ws_protocol", NULL, 0));
    CHECK(av_dict_get(opts, "user_agent", NULL, 0));
    av_dict_free(&opts);
}

static int open_verify(IjkWsContext **ctx, const char *url, const char *ca_name)
{
    AVDictionary *opts = NULL;
    char ca[1024];
    int ret;
    av_dict_set(&opts, "ws_tls_verify", "1", 0);
    if (ca_name)
        av_dict_set(&opts, "ws_ca_file", test_path(ca, sizeof(ca), ca_name), 0);
    av_dict_set(&opts, "ws_timeout", "3000", 0);
    ret = ijkws_open(ctx, url, &opts, NULL);
    av_dict_free(&opts);
    return ret;
}

static void test_tls_verify_ok(void)
{
    IjkWsContext *ctx = NULL;
    CHECK_EQ_INT(open_verify(&ctx, "wss://localhost:" WSS_PORT "/flv", "server.crt"), 0);
    CHECK(ctx != NULL);
    read_expect_flv(&ctx);
}

static void test_tls_verify_wrong_ca(void)
{
    IjkWsContext *ctx = NULL;
    int ret = open_verify(&ctx, "wss://localhost:" WSS_PORT "/flv", "other-ca.crt");
    fprintf(stderr, "  wrong ca: ret=%d\n", ret);
    CHECK(ret < 0);
    CHECK(ctx == NULL);
}

static void test_tls_verify_no_ca(void)
{
    IjkWsContext *ctx = NULL;
    CHECK_EQ_INT(open_verify(&ctx, "wss://localhost:" WSS_PORT "/flv", NULL), AVERROR(EINVAL));
    CHECK(ctx == NULL);
}

static void test_tls_verify_missing_ca_file(void)
{
    IjkWsContext *ctx = NULL;
    int ret = open_verify(&ctx, "wss://localhost:" WSS_PORT "/flv", "no-such-ca.crt");
    fprintf(stderr, "  missing ca file: ret=%d\n", ret);
    CHECK(ret < 0);
    CHECK(ctx == NULL);
}

/* ws_tls_verify 只接受 "0" / "1"：其他值不能悄悄关掉校验 */
static void test_tls_verify_invalid_value(void)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    char ca[1024];
    int ret;
    av_dict_set(&opts, "ws_tls_verify", "yes", 0);
    av_dict_set(&opts, "ws_ca_file", test_path(ca, sizeof(ca), "server.crt"), 0);
    ret = ijkws_open(&ctx, "wss://localhost:" WSS_PORT "/flv", &opts, NULL);
    CHECK(!av_dict_get(opts, "ws_tls_verify", NULL, 0));
    CHECK(!av_dict_get(opts, "ws_ca_file", NULL, 0));
    av_dict_free(&opts);
    CHECK_EQ_INT(ret, AVERROR(EINVAL));
    CHECK(ctx == NULL);
}

/* 续行（obs-fold，RFC 7230 §3.2.4 已废弃）：以 SP / HTAB 开头的行要拒绝 */
static void expect_headers_rejected(const char *headers)
{
    AVDictionary *opts = NULL;
    IjkWsContext *ctx = NULL;
    int ret;
    av_dict_set(&opts, "ws_headers", headers, 0);
    ret = ijkws_open(&ctx, WS_BASE "/flv", &opts, NULL);
    CHECK(!av_dict_get(opts, "ws_headers", NULL, 0));
    av_dict_free(&opts);
    CHECK_EQ_INT(ret, AVERROR(EINVAL));
    CHECK(ctx == NULL);
}

static void test_ws_headers_obs_fold_rejected(void)
{
    expect_headers_rejected("X-A: 1\r\n continued");
    if (g_test_cur_failed) return;
    expect_headers_rejected("X-A: 1\r\n\tcontinued");
    if (g_test_cur_failed) return;
    expect_headers_rejected(" X-A: 1");
}

/* 长 token：query 6000 字节，握手（lws 的 ah 与 serv_buf）都要放得下，服务器看到完整 query */
static void test_long_query_token(void)
{
    IjkWsContext *ctx = NULL;
    const size_t tok_len = 6000;
    char prefix[64], *query, *url, *needle, *line = NULL;
    size_t cap = 0;
    FILE *f;
    int found = 0;

    snprintf(prefix, sizeof(prefix), "n=%lld&token=", (long long)now_ms());
    query = malloc(strlen(prefix) + tok_len + 1);
    strcpy(query, prefix);
    memset(query + strlen(prefix), 'a', tok_len);
    query[strlen(prefix) + tok_len] = '\0';
    url = malloc(strlen(query) + 64);
    sprintf(url, WS_BASE "/flv?%s", query);
    needle = malloc(strlen(query) + 32);
    sprintf(needle, "\"query\": \"%s\"", query);

    {
        int ret = ijkws_open(&ctx, url, NULL, NULL);
        free(url);
        if (ret != 0) {
            free(query);
            free(needle);
            CHECK_EQ_INT(ret, 0);
        }
    }
    read_expect_flv(&ctx);
    if (g_test_cur_failed) {
        free(query);
        free(needle);
        return;
    }
    f = fopen(getenv("IJKWS_TEST_LOG"), "r");
    while (f && getline(&line, &cap, f) > 0) {
        if (strstr(line, "\"event\": \"connect\"") && strstr(line, needle)) {
            found = 1;
            break;
        }
    }
    if (f)
        fclose(f);
    free(line);
    free(query);
    free(needle);
    CHECK(found);
}

/* 期望 query 对应连接的 SNI：expect 为 NULL 表示没有 SNI */
static void expect_sni(const char *query, const char *expect)
{
    char line[16384], key[256];
    CHECK(connect_line(query, line, sizeof(line)));
    if (expect)
        snprintf(key, sizeof(key), "\"sni\": \"%s\"", expect);
    else
        snprintf(key, sizeof(key), "\"sni\": null");
    if (!strstr(line, key)) {
        const char *p = strstr(line, "\"sni\"");
        fprintf(stderr, "  expect %s, got %.40s\n", key, p ? p : "(none)");
        CHECK(!"sni mismatch");
    }
}

static void open_default_wss(const char *host, const char *tag, const char *expect)
{
    IjkWsContext *ctx = NULL;
    char url[256], query[64];
    snprintf(query, sizeof(query), "%s=%lld", tag, (long long)now_ms());
    snprintf(url, sizeof(url), "wss://%s:" WSS_PORT "/flv?%s", host, query);
    CHECK_EQ_INT(ijkws_open(&ctx, url, NULL, NULL), 0);
    read_expect_flv(&ctx);
    if (g_test_cur_failed)
        return;
    expect_sni(query, expect);
}

/* SNI 只用于主机名（RFC 6066 §3），IP 字面量不发 */
static void test_sni_hostname(void)     { open_default_wss("localhost", "snihost", "localhost"); }
static void test_sni_none_for_ipv4(void){ open_default_wss("127.0.0.1", "sniv4", NULL); }
static void test_sni_none_for_ipv6(void){ open_default_wss("[::1]", "sniv6", NULL); }

/* 校验模式下 IP 字面量按 IP 校验证书：server-ip.crt 的 SAN 有 IP:::1，没有 127.0.0.1 */
static void test_tls_verify_ipv6_literal_ok(void)
{
    IjkWsContext *ctx = NULL;
    CHECK_EQ_INT(open_verify(&ctx, "wss://[::1]:" WSS_PORT "/flv", "server-ip.crt"), 0);
    read_expect_flv(&ctx);
}

static void test_tls_verify_ip_mismatch(void)
{
    IjkWsContext *ctx = NULL;
    int ret = open_verify(&ctx, "wss://127.0.0.1:" WSS_PORT "/flv", "server-ip.crt");
    fprintf(stderr, "  ip mismatch: ret=%d\n", ret);
    CHECK(ret < 0);
    CHECK(ctx == NULL);
}

int main(void)
{
    RUN(test_headers_forwarded);
    RUN(test_no_origin_by_default);
    RUN(test_headers_lenient_separators);
    RUN(test_opts_consumed);
    RUN(test_opts_consumed_on_failure);
    RUN(test_tls_verify_ok);
    RUN(test_tls_verify_wrong_ca);
    RUN(test_tls_verify_no_ca);
    RUN(test_tls_verify_missing_ca_file);
    RUN(test_tls_verify_invalid_value);
    RUN(test_ws_headers_obs_fold_rejected);
    RUN(test_long_query_token);
    RUN(test_sni_hostname);
    RUN(test_sni_none_for_ipv4);
    RUN(test_sni_none_for_ipv6);
    RUN(test_tls_verify_ipv6_literal_ok);
    RUN(test_tls_verify_ip_mismatch);
    return test_finish();
}
