#!/usr/bin/env bash
# ============================================================================
# 启动 out/（默认构建目录）里的 EdgeVisionService
# ----------------------------------------------------------------------------
#   scripts/run/out/start.sh                     # 用 config/node.json
#   scripts/run/out/start.sh config/push.json    # 指定配置（相对仓库根或绝对路径）
#   EVS_CRASH_DIR=/data/crash scripts/run/out/start.sh
#
# 做的事：抬 ulimit -c → 设 LD_LIBRARY_PATH → 设崩溃报告目录 → 打印 core 落点
#         → 前台 exec（Ctrl+C 直接送到进程；卡死时另开终端 kill -QUIT <pid> 取现场）
# ============================================================================
set -u

BUILD_DIR="out"            # ← 与 scripts/run/build/start.sh 的唯一差别
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
EXE="$ROOT/$BUILD_DIR/bin/EdgeVisionService"

if [ ! -x "$EXE" ]; then
  echo "[start] 找不到可执行文件: $EXE" >&2
  echo "[start] 先构建: make   (= cmake -S . -B out && cmake --build out -j\$(nproc))" >&2
  exit 1
fi

CFG="${1:-config/node.json}"
case "$CFG" in /*) : ;; *) CFG="$ROOT/$CFG" ;; esac
if [ ! -f "$CFG" ]; then
  echo "[start] 找不到配置: $CFG" >&2
  echo "[start] 可用配置: $(ls "$ROOT"/config/*.json 2>/dev/null | tr '\n' ' ')" >&2
  exit 2
fi

LIBS="$ROOT/$BUILD_DIR/lib"
[ -d "$ROOT/install/rk3588/lib" ] && LIBS="$LIBS:$ROOT/install/rk3588/lib"
export LD_LIBRARY_PATH="$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

export EVS_CRASH_DIR="${EVS_CRASH_DIR:-$ROOT/crash}"
mkdir -p "$EVS_CRASH_DIR" 2>/dev/null \
  || echo "[start] 警告: 无法创建报告目录 $EVS_CRASH_DIR" >&2

ulimit -c unlimited 2>/dev/null
UL="$(ulimit -c 2>/dev/null || echo '?')"
[ "$UL" = "unlimited" ] \
  || echo "[start] 警告: ulimit -c=$UL（hard limit 为 0 时 core 无法生成，用 root 或 unit 的 LimitCORE 放宽）" >&2

echo "[start] exe          = $EXE"
echo "[start] config       = $CFG"
echo "[start] cwd          = $ROOT/$BUILD_DIR"
echo "[start] EVS_CRASH_DIR= $EVS_CRASH_DIR"
echo "[start] ulimit -c    = $UL"
echo "[start] core_pattern = $(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo '?')"
echo "[start] 停止: Ctrl+C    取现场: kill -QUIT <pid>（报告 + core）"

cd "$ROOT/$BUILD_DIR" || exit 1
exec "$EXE" "$CFG"
