#!/usr/bin/env bash
# T5 验收脚本：NodeFactory（type -> 节点对象）+ PipelineBuilder（拓扑校验 / 装配 / 连线）
#
# 用法：./scripts/tests/run_t5_builder.sh [node.json 路径]
# 特点：只 build 不 start —— 不碰硬件、不起线程、不开 RTSP、不推流。
# 记录见 docs/TESTLOG.md §5.8（L2 · T5）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

echo "== 1/3 重新 generate（新增了 test_builder target，缓存里没有就要重跑）=="
cmake -S . -B build > /dev/null

echo "== 2/3 编译 test_builder =="
cmake --build build --target test_builder -j"$(nproc)"

echo "== 3/3 运行（工作目录 = 仓库根）=="
./build/bin/test_builder "${1:-config/node.json}"
