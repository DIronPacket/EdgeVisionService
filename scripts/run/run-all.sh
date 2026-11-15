#!/usr/bin/env bash
# ============================================================================
# 三进程一起跑（开发态）：evs_media + evs_comm + evs_upgrade
# ----------------------------------------------------------------------------
#   make run-all                          # = bash scripts/run/run-all.sh out
#   bash scripts/run/run-all.sh out       # 直接调（构建目录可换 build）
#   make logs                             # 另开终端 tail -f out/logs/*.log
#
# 单独跑某一个（同样可用，各自只起一个进程）：
#   make run-media / make run-comm / make run-upgrade
#
# 与 systemd（scripts/deploy/evs.target）的差别：
#   - 不需要 root、不改系统；日志落 <build>/logs/*.log，不是 journal
#   - 停机顺序：先 media（有序 stop_all → FreeMem，最多等 25 s）→ 再 comm / upgrade
#   - 任一进程提前退出（崩了）→ 打印它的 rc，然后整体收尾并返回非 0
#   - 某个二进制没编（EVS_PROCESS 没选它）→ 跳过并告警，不中断其它两个
# ============================================================================
set -u

BUILD="${1:-out}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "$BUILD" in
  /*) BUILD_DIR="$BUILD" ;;          # 允许绝对路径（临时构建树，例如 /tmp/evs-comm-only）
  *)  BUILD_DIR="$ROOT/$BUILD" ;;
esac
LOG_DIR="$BUILD_DIR/logs"
PLATFORM="${EVS_PLATFORM:-rk3588}"

MEDIA_TIMEOUT=25     # media 有序停止（stop_all → FreeMem）的等待上限
OTHER_TIMEOUT=5      # comm / upgrade 的等待上限

die() { echo "[run-all] $*" >&2; exit 1; }

[ -d "$BUILD_DIR" ] || die "构建目录不存在: $BUILD_DIR（先 make）"
[ -f "$BUILD_DIR/Makefile" ] || die "$BUILD_DIR 没有 CMake 缓存（先 make）"

mkdir -p "$LOG_DIR" || die "无法创建日志目录: $LOG_DIR"

LIBS="$BUILD_DIR/lib"
[ -d "$ROOT/install/$PLATFORM/lib" ] && LIBS="$LIBS:$ROOT/install/$PLATFORM/lib"
export LD_LIBRARY_PATH="$LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export EVS_CRASH_DIR="${EVS_CRASH_DIR:-$ROOT/crash}"
mkdir -p "$EVS_CRASH_DIR" 2>/dev/null || true
ulimit -c unlimited 2>/dev/null

cd "$BUILD_DIR" || die "cd $BUILD_DIR 失败"

# ---------------------------------------------------------------- 进程表
NAMES=(media comm upgrade)
BINS=(bin/evs_media bin/evs_comm bin/evs_upgrade)
CFGS=("$ROOT/config/node.json" "$ROOT/config/comm.json" "$ROOT/config/upgrade.json")  # 绝对路径：cwd 是构建目录，配置不跟着构建目录走
TIMEOUTS=("$MEDIA_TIMEOUT" "$OTHER_TIMEOUT" "$OTHER_TIMEOUT")   # 停止时的等待上限
PIDS=(0 0 0)
RUNNING=(0 0 0)
LOGS=("$LOG_DIR/media.log" "$LOG_DIR/comm.log" "$LOG_DIR/upgrade.log")

echo "[run-all] 构建目录 = $BUILD_DIR"
echo "[run-all] 日志目录 = $LOG_DIR"
echo "[run-all] EVS_CRASH_DIR = $EVS_CRASH_DIR"

SIGNAL=0
EXIT_CODE=0

on_signal() {
  SIGNAL=1
  echo ""
  echo "[run-all] 收到退出信号 → 按 media → comm → upgrade 顺序停止"
}
trap on_signal INT TERM

# ---------------------------------------------------------------- 启动
start_one() {  # start_one <idx>
  local i="$1"
  local exe="$BUILD_DIR/${BINS[$i]}" cfg="${CFGS[$i]}" log="${LOGS[$i]}"
  if [ ! -x "$exe" ]; then
    echo "[run-all] 跳过 ${NAMES[$i]}：$exe 不存在（当前 EVS_PROCESS 没选它？make info 可查）"
    return 1
  fi
  if [ ! -f "$cfg" ]; then
    echo "[run-all] 跳过 ${NAMES[$i]}：配置不存在: $cfg"
    return 1
  fi
  : > "$log"
  "${exe}" "$cfg" >> "$log" 2>&1 &
  PIDS[$i]=$!
  RUNNING[$i]=1
  echo "[run-all] ${NAMES[$i]}  pid=${PIDS[$i]}  日志=${log#$ROOT/}"
}

# media 先起：upgrade 的 health_check 与 systemd 的 After=evs-media 都是这个隐含前提
start_one 0
if [ "${RUNNING[0]}" -eq 1 ]; then
  # 等它把链路拉起来（日志出现「条链路已启动」），最多 8 s；进程先死了就立刻收尾
  for _ in $(seq 1 40); do
    kill -0 "${PIDS[0]}" 2>/dev/null || break
    grep -q "条链路已启动" "${LOGS[0]}" 2>/dev/null && break
    sleep 0.2
  done
fi
start_one 1
start_one 2

STARTED=0
for i in 0 1 2; do [ "${RUNNING[$i]}" -eq 1 ] && STARTED=$((STARTED + 1)); done
[ "$STARTED" -gt 0 ] || die "三个进程一个都没起来（先 make 全编：make）"

echo "[run-all] 已启动 $STARTED 个进程；Ctrl+C 停止全部；实时日志：make logs"

# ---------------------------------------------------------------- 监视
while [ "$SIGNAL" -eq 0 ]; do
  for i in 0 1 2; do
    [ "${RUNNING[$i]}" -eq 1 ] || continue
    if ! kill -0 "${PIDS[$i]}" 2>/dev/null; then
      wait "${PIDS[$i]}" 2>/dev/null
      rc=$?
      RUNNING[$i]=0
      echo "[run-all] ${NAMES[$i]} 已退出（rc=$rc）→ 收尾"
      [ "$rc" -ne 0 ] && EXIT_CODE=1
      SIGNAL=2
      break
    fi
  done
  [ "$SIGNAL" -eq 0 ] && sleep 1
done

# ---------------------------------------------------------------- 收尾
wait_exit() {  # wait_exit <idx>
  local i="$1" pid="${PIDS[$1]}"
  # pid=0 表示这个进程根本没起来（被跳过）：绝不能 kill -0 0 / kill 0 —— 那是当前进程组，会误杀自己
  if [ "${pid:-0}" -le 0 ]; then RUNNING[$i]=0; return 0; fi
  local limit="${TIMEOUTS[$1]}" n=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$n" -ge $((limit * 5)) ]; then
      echo "[run-all] 警告: ${NAMES[$i]} ${limit}s 内未退出 → SIGKILL"
      kill -KILL "$pid" 2>/dev/null
      break
    fi
    sleep 0.2
    n=$((n + 1))
  done
  wait "$pid" 2>/dev/null
  local rc=$?
  RUNNING[$i]=0
  echo "[run-all] ${NAMES[$i]} 已停止（rc=$rc）"
  [ "$rc" -ne 0 ] && [ "$rc" -ne 130 ] && EXIT_CODE=1
  return 0
}

stop_one() {  # stop_one <idx>
  local i="$1"
  [ "${RUNNING[$i]}" -eq 1 ] || return 0
  kill -INT "${PIDS[$i]}" 2>/dev/null || true
}

# 先 media（唯一的「重」进程：stop_all → FreeMem），再 comm / upgrade
stop_one 0
wait_exit 0
stop_one 1; stop_one 2
wait_exit 1; wait_exit 2

if [ "$SIGNAL" -eq 2 ]; then
  echo "[run-all] 因进程提前退出而收尾（见上方 rc）"
fi
echo "[run-all] 全部结束（exit=$EXIT_CODE）；日志在 ${LOG_DIR#$ROOT/}"
exit "$EXIT_CODE"
