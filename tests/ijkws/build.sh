#!/bin/bash
# 构建主机端工具：
#   tests/ijkws/build.sh cat     -> tests/ijkws/build/ijkws_cat
#   tests/ijkws/build.sh <name>  -> tests/ijkws/build/<name>（编译 tests/ijkws/<name>.c）
set -euo pipefail
. "$(dirname "$0")/common.sh"
ensure_lws
name="${1:-cat}"
[ "$name" = cat ] && name=ijkws_cat
build_c "$name"
echo "built $BUILD/$name"
