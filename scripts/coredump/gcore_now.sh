#!/usr/bin/env bash
# ============================================================================
# 进程「没崩但不对」（卡死 / 无输出 / CPU 100%）时，抓一个活进程的现场
# ----------------------------------------------------------------------------
# 用法：
#   scripts/coredump/gcore_now.sh <pid|进程名关键字> [输出目录]
#   scripts/coredump/gcore_now.sh EdgeVisionService crash
#   scripts/coredump/gcore_now.sh 12345
#
# 做三件事：
#   1. /proc 快照：status / 每线程 wchan（区分「卡在锁」还是「在等 IO」）/ 端口
#   2. gcore：ptrace attach，把活进程内存 + 所有线程寄存器写成 core（不杀进程）
#   3. 调 analyze_core.sh 生成全线程栈 + 局部变量
#
# 需要：gdb（自带 gcore）、ptrace 权限（root 或同用户，且 ptrace_scope 允许）
# 注意：attach 期间目标进程会短暂停止（通常 < 1s），生产环境要记住这一点。
# ============================================================================
set -u

TARGET="${1:-}"
OUTDIR="${2:-crash}"
if [ -z "$TARGET" ]; then
  echo "用法: $0 <pid|进程名关键字> [输出目录]"
  exit 1
fi

if [[ "$TARGET" =~ ^[0-9]+$ ]]; then
  PID="$TARGET"
else
  PID="$(pgrep -f "$TARGET" | head -1)"
fi
if [ -z "${PID:-}" ] || [ ! -d "/proc/$PID" ]; then
  echo "找不到进程: $TARGET"
  exit 1
fi

NAME="$(cat "/proc/$PID/comm" 2>/dev/null || echo proc)"
STAMP="$(date +%s)"
mkdir -p "$OUTDIR"
PREFIX="$OUTDIR/live_${NAME}_${PID}_${STAMP}"

echo "== 目标: $NAME (pid=$PID) =="
{
  echo "== date =="; date
  echo; echo "== /proc/$PID/status =="; cat "/proc/$PID/status"
  echo; echo "== /proc/$PID/cmdline =="; tr '\0' ' ' < "/proc/$PID/cmdline"; echo
  echo; echo "== threads (TID / state / cpu / wchan) =="
  ps -L -p "$PID" -o pid,tid,psr,stat,pcpu,wchan:32,comm
  echo; echo "== listening ports =="
  ss -ltnp 2>/dev/null | grep -E "pid=$PID|:8550|:8554" || true
} > "${PREFIX}.txt" 2>&1

echo "-- /proc 快照: ${PREFIX}.txt"

if command -v gcore >/dev/null 2>&1; then
  echo "-- gcore（attach 期间目标进程会短暂暂停）"
  gcore -o "$PREFIX" "$PID" || echo "!! gcore 失败（检查 ptrace_scope / 权限）"
  CORE_FILE="${PREFIX}.${PID}"
  if [ -f "$CORE_FILE" ]; then
    "$(dirname "$0")/analyze_core.sh" "$CORE_FILE" \
      "${EVS_EXE:-out/bin/EdgeVisionService}"
  fi
else
  echo "!! 没有 gcore（gdb 未安装？）"
fi

echo
echo "下一步："
echo "  1) ${PREFIX}.txt 看 wchan：futex/do_wait = 卡在锁；poll/epoll = 在等 IO"
echo "  2) 全线程栈与参数见上面 analyze_core.sh 的输出 txt"
