#!/usr/bin/env bash
# T3 验收脚本：config/node.json（v1）结构与参数自检
#
# 用法：./scripts/tests/run_t3_node_json.sh
# 特点：不碰硬件、不起 RTSP 服务、不推流；只读 config/*.json，并用 ffprobe 探测 ch1~ch3 的素材。
# 记录见 docs/history/TESTLOG.md §5.7（L2 · T3）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

OUT_DIR="${OUT_DIR:-$(mktemp -d)}"
mkdir -p "$OUT_DIR"
FFPROBE="$ROOT/3rdparty/rk3588/ffmpeg/bin/ffprobe"
FFMPEG_LIB="$ROOT/3rdparty/rk3588/ffmpeg/lib"

echo "== 1/4 JSON 语法检查 =="
python3 -m json.tool config/node.json > /dev/null
echo "config/node.json: JSON OK"
python3 -m json.tool config/push.json > /dev/null
echo "config/push.json: JSON OK"

echo "== 2/4 ffprobe 探测 ch1~ch3 素材（缺失则跳过实测比对）=="
EXPECT="$(python3 - "$FFPROBE" "$FFMPEG_LIB" <<'PY'
import json, os, subprocess, sys

ffprobe, fflib = sys.argv[1], sys.argv[2]
cfg = json.load(open('config/node.json'))
srcs = []
for p in cfg['pipelines']:
    for n in p['nodes']:
        if n['type'] == 'vi':
            s = n['config'].get('src', '')
            if s.startswith('file:'):
                srcs.append(s[len('file:'):])
if not srcs:
    sys.exit(0)

ref = None
for s in srcs:
    if not os.path.isfile(s):
        print('SKIP: 素材不存在 %s' % s, file=sys.stderr)
        continue
    out = subprocess.check_output(
        [ffprobe, '-v', 'error', '-select_streams', 'v:0',
         '-show_entries', 'stream=width,height,r_frame_rate', '-of', 'csv=p=0', s],
        env=dict(os.environ, LD_LIBRARY_PATH=fflib), text=True).strip().split(',')
    w, h, fr = int(out[0]), int(out[1]), out[2]
    num, den = fr.split('/')
    fps = int(round(int(num) / int(den)))
    print('probe: %s -> %dx%d @%dfps' % (s, w, h, fps), file=sys.stderr)
    cur = (w, h, fps)
    if ref is None:
        ref = cur
    elif ref != cur:
        print('FAIL: 素材参数不一致，%s=%s 与前面的 %s 不同' % (s, cur, ref), file=sys.stderr)
        sys.exit(2)
if ref:
    print('%d %d %d' % ref)
PY
)"

echo "== 3/4 编译验收程序 =="
g++ -std=c++14 -O2 "scripts/tests/t3_node_json.cpp" \
    -I src/common -I src/common/configmgr \
    -L build/lib -lcommon_api \
    -Wl,-rpath,"$ROOT/build/lib" \
    -lpthread \
    -o "$OUT_DIR/t3_node_json"

echo "== 4/4 运行（工作目录 = 仓库根）=="
if [[ -n "$EXPECT" ]]; then
    echo "素材实测参数（作为期望值传入）: $EXPECT"
    "$OUT_DIR/t3_node_json" "config/node.json" $EXPECT
else
    echo "未探测到素材，仅做结构自检（实测比对将 SKIP）"
    "$OUT_DIR/t3_node_json" "config/node.json"
fi
