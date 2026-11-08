#!/usr/bin/env bash
# T1 验收脚本：P0-6（zlm_rtsp_stop 的 key 与注册一致 + ref_cnt 不泄漏）+ P0-7（入参校验顺序）
#
# 用法：./scripts/tests/run_t1_zlm_rtsp_key.sh [端口]
#   默认端口 18550，避开现场正在跑的 8550。
#
# 注意：本脚本会**真的**起停 RTSP 服务（ZLM 会打印日志），并依赖 `ss` 观察端口。
# 记录见 docs/TESTLOG.md §5.5（L2 · T1）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

PORT="${1:-18550}"
PLATFORM="${TARGET_PLATFORM:-rk3588}"
OUT_DIR="${OUT_DIR:-$(mktemp -d)}"

if [ ! -d "install/${PLATFORM}/include" ]; then
    echo "缺少 install/${PLATFORM}/include（ZLMediaKit 依赖），无法编译验收程序" >&2
    exit 1
fi

echo "== 1/3 构建 rtsp 目标 =="
cmake --build build --target rtsp -j"$(nproc)"

echo "== 2/3 编译验收程序 =="
g++ -std=c++14 -O2 "scripts/tests/t1_zlm_rtsp_key.cpp" \
    -I src/platform/rtsp -I "install/${PLATFORM}/include" \
    -L build/lib -lrtsp \
    -L "install/${PLATFORM}/lib" -lmk_api \
    -Wl,-rpath,"$ROOT/build/lib" -Wl,-rpath,"$ROOT/install/${PLATFORM}/lib" \
    -lpthread -ldl \
    -o "$OUT_DIR/t1_zlm_rtsp_key"

echo "== 3/3 运行（端口 ${PORT}）=="
"$OUT_DIR/t1_zlm_rtsp_key" "$PORT"
