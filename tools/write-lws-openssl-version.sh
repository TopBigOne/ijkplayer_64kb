#!/usr/bin/env bash
#
# 把编译 libwebsockets 时所用 OpenSSL 头文件的版本写进 lws 安装目录：
#   <lws-include-dir>/ijk_lws_openssl_version.h
#     IJK_LWS_OPENSSL_VERSION_NUMBER / IJK_LWS_OPENSSL_VERSION_TEXT
#
# ijkws.c 会直接调用 OpenSSL 函数操作 lws 创建的 SSL 对象（见 fix_tls_for_ip_literal）。
# Android 上 libijkwebsockets.so 静态链接并隐藏了自己的一份 OpenSSL，ijkws.c 的调用则解析到
# libijkplayer.so 里的另一份；两份必须是同一个 OpenSSL 构建。ijkws.c 用这个头文件在编译期
# 比较两边的 OPENSSL_VERSION_NUMBER，不一致直接 #error。
#
# 用法：tools/write-lws-openssl-version.sh <openssl-include-dir> <lws-include-dir>
# 由 tools/build-libwebsockets-{android,ios,host}.sh 在安装 lws 之后调用。

set -euo pipefail

ssl_inc="$1"
out_dir="$2"
out="$out_dir/ijk_lws_openssl_version.h"

# 只做预处理，取出宏展开后的值（OpenSSL 3 的 VERSION_NUMBER 是表达式，原样保留即可）
expand() {
    printf '#include <openssl/opensslv.h>\n%s\n' "$1" |
        xcrun clang -E -P -x c -I "$ssl_inc" - | sed '/^[[:space:]]*$/d' | tail -1
}

num=$(expand OPENSSL_VERSION_NUMBER)
text=$(expand OPENSSL_VERSION_TEXT)
if [ -z "$num" ] || [ "$num" = OPENSSL_VERSION_NUMBER ]; then
    echo "无法从 $ssl_inc 取得 OPENSSL_VERSION_NUMBER" >&2
    exit 1
fi

mkdir -p "$out_dir"
cat > "$out" <<EOF
/* 由 tools/write-lws-openssl-version.sh 生成，勿手改 */
#ifndef IJK_LWS_OPENSSL_VERSION_H
#define IJK_LWS_OPENSSL_VERSION_H
/* 编译 libwebsockets 时所用 OpenSSL 头文件（$ssl_inc）的版本 */
#define IJK_LWS_OPENSSL_VERSION_NUMBER ($num)
#define IJK_LWS_OPENSSL_VERSION_TEXT   $text
#endif
EOF
echo "-> $out ($text)"
