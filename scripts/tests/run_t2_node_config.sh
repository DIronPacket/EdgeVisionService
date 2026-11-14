#!/usr/bin/env bash
# T2 验收脚本：NodeConfigManager（v1 解析 / 校验 / v0 适配 / 引用展开 / 快照语义）
#
# 用法：./scripts/tests/run_t2_node_config.sh
# 特点：不碰硬件、不起 RTSP 服务、不需要板端设备；只读 config/*.json 与 fixtures/。
# 记录见 docs/history/TESTLOG.md §5.6（L2 · T2）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

OUT_DIR="${OUT_DIR:-$(mktemp -d)}"

echo "== 1/3 构建 common_api =="
cmake --build build --target common_api -j"$(nproc)"

echo "== 2/3 编译验收程序 =="
g++ -std=c++14 -O2 "scripts/tests/t2_node_config.cpp" \
    -I src/common -I src/common/configmgr \
    -L build/lib -lcommon_api \
    -Wl,-rpath,"$ROOT/build/lib" \
    -lpthread \
    -o "$OUT_DIR/t2_node_config"

echo "== 3/3 运行（工作目录 = 仓库根）=="
"$OUT_DIR/t2_node_config" "scripts/tests/fixtures"
