#!/usr/bin/env bash
# T6 验收脚本：通信层（CommBase / UdpComm / MqttComm / CommManager）
#
# 用法：./scripts/tests/run_t6_comm.sh
#   环境变量 BUILD 可改构建目录（默认 out，与仓库当前默认一致；
#   注意其他 run_t*.sh 仍写死 build/，见 scripts/tests/README.md 的已知问题）。
# 特点：只跑本机回环 —— 不碰硬件、不起 RTSP、不推流；
#       MQTT 需要 127.0.0.1:1883 有 broker，没有则脚本内 SKIP（不算失败）。
# 记录见 docs/history/TESTLOG.md（L2 · T6）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BUILD="${BUILD:-out}"

echo "== 1/3 generate（构建目录：${BUILD}）=="
cmake -S . -B "${BUILD}" > /dev/null

echo "== 2/3 编译 comm_api + test_comm =="
cmake --build "${BUILD}" --target test_comm -j"$(nproc)"

echo "== 3/3 运行（需要找到 libcomm_api.so / libmosquitto.so）=="
export LD_LIBRARY_PATH="${BUILD}/lib:${ROOT}/install/rk3588/lib:${LD_LIBRARY_PATH:-}"
"./${BUILD}/bin/test_comm"
