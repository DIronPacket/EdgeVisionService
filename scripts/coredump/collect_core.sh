#!/usr/bin/env bash
# ============================================================================
# 把 core 归档成「一眼看出崩在哪」的目录：
#   <archive>/core.<app>.<pid>.<epoch>.sig<N>.<函数>@<文件>:<行>/
#       core                      ← 原 core（同盘硬链接，不占双倍空间）
#       meta.txt                  ← exe/build-id/信号/core_pattern/内核/ulimit
#       crash_thread_bt.txt       ← 崩溃线程栈（tgkill 保证就是崩溃现场）
#       backtrace_all_threads.txt ← 全线程栈
#       sharedlibs.txt            ← .so 加载情况（排查 ?? 帧）
#       report.log                ← 自动配对的 crash_*.log（同 pid）
#
# 原理：core_pattern 由内核展开，只能带 %e/%p/%t/%s 这类进程级信息；
#       函数名/文件名必须由用户态符号解析补上（docs/ops/CRASH_DUMP.md §2.3 方案 B）。
#
# 用法：
#   scripts/coredump/collect_core.sh <core> [exe] [-o 归档根目录] [-s 信号号]
#   scripts/coredump/collect_core.sh out/core.75879 out/bin/EdgeVisionService
#   scripts/coredump/collect_core.sh /var/lib/evs/coredump/core.* -o /data/archive
# ============================================================================
set -u

CORE=""
EXE=""
OUT_ROOT=""
SIG_HINT=""

usage() { sed -n '2,22p' "$0"; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help) usage ;;
    -o) OUT_ROOT="${2:-}"; shift 2 ;;
    -s) SIG_HINT="${2:-}"; shift 2 ;;
    *)  if [ -z "$CORE" ]; then CORE="$1"; elif [ -z "$EXE" ]; then EXE="$1"; else usage; fi; shift ;;
  esac
done

[ -n "$CORE" ] || usage
[ -f "$CORE" ] || { echo "找不到 core 文件: $CORE" >&2; exit 2; }
command -v gdb >/dev/null 2>&1 || { echo "需要 gdb（apt install gdb）" >&2; exit 2; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CORE_ABS="$(cd "$(dirname "$CORE")" && pwd)/$(basename "$CORE")"
CORE_BASE="$(basename "$CORE_ABS")"

# ---------------------------------------------------------------- 文件名拆解
# 支持 core.<app>.<pid>.<epoch>[.sig<N>] / core.<pid> / core
APP="-"; PID="-"; EPOCH="-"; SIG="-"
IFS='.' read -r -a PARTS <<< "$CORE_BASE"
for seg in "${PARTS[@]:-}"; do
  case "$seg" in
    ''|*[!0-9]*) 
      case "$seg" in
        core|"") : ;;
        sig*) SIG="${seg#sig}" ;;
        *) [ "$APP" = "-" ] && APP="$seg" ;;
      esac
      ;;
    *) 
      if [ "$PID" = "-" ]; then PID="$seg"
      elif [ "$EPOCH" = "-" ] && [ "${#seg}" -ge 9 ]; then EPOCH="$seg"
      fi
      ;;
  esac
done
[ -n "$SIG_HINT" ] && SIG="$SIG_HINT"
# epoch 缺省用文件 mtime（内核写完 core 才 mtime 更新，误差在秒级）
[ "$EPOCH" = "-" ] && EPOCH="$(stat -c %Y "$CORE_ABS" 2>/dev/null || echo 0)"

# ---------------------------------------------------------------- 找可执行文件
# %e 会被截断到 16 字节（EdgeVisionService → EdgeVisionServic），所以按前缀匹配
if [ -z "$EXE" ]; then
  for d in "$(dirname "$CORE_ABS")/../bin" "$(dirname "$CORE_ABS")" \
           "$ROOT/out/bin" "$ROOT/build/bin" "$ROOT/bin"; do
    [ -d "$d" ] || continue
    for cand in "$d"/*; do
      [ -f "$cand" ] && [ -x "$cand" ] || continue
      case "$(basename "$cand")" in
        "$APP"*|"$APP") EXE="$cand"; break 2 ;;
      esac
    done
  done
fi
if [ -z "$EXE" ]; then
  # 没有程序名线索（pattern=core → core.75255）：候选目录里只有一个可执行文件时直接用它
  if [ "$APP" = "-" ]; then
    for d in "$ROOT/out/bin" "$ROOT/build/bin" "$(dirname "$CORE_ABS")"; do
      [ -d "$d" ] || continue
      found=""; n=0
      for cand in "$d"/*; do
        [ -f "$cand" ] && [ -x "$cand" ] || continue
        found="$cand"; n=$((n + 1))
      done
      if [ "$n" -eq 1 ]; then EXE="$found"; break; fi
    done
  fi
fi
if [ -z "$EXE" ]; then
  echo "无法自动判断可执行文件（core 里的程序名段 = '$APP'）。请显式给出：" >&2
  echo "  $0 $CORE <exe>" >&2
  exit 2
fi
EXE_ABS="$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")"
[ -x "$EXE_ABS" ] || { echo "可执行文件不可执行: $EXE_ABS" >&2; exit 2; }

# core 名里没有程序段时（例如 pattern=core → core.75255），用二进制名补上
# 注意：名字来自内核 %e 时才可能被截断到 16 字节，来自 exe 时用全名
if [ "$APP" = "-" ]; then
  APP="$(basename "$EXE_ABS")"
fi

# ----------------------------------------------- 用 pid 把同一次崩溃的报告配过来
REPORT="-"
if [ "$PID" != "-" ]; then
  for d in "$(dirname "$CORE_ABS")" "$(dirname "$CORE_ABS")/../crash" \
           "${EVS_CRASH_DIR:-}" "$ROOT/crash" "$ROOT/out/crash"; do
    [ -n "$d" ] && [ -d "$d" ] || continue
    hit="$(ls -t "$d"/crash_*_"${PID}"_*_sig*.log 2>/dev/null | head -1)"
    if [ -n "$hit" ]; then REPORT="$hit"; break; fi
  done
fi
# 信号号：优先 core 名里的 sigN，其次报告名里的 _sigN，最后未知
if [ "$SIG" = "-" ] && [ "$REPORT" != "-" ]; then
  SIG="$(printf '%s' "$REPORT" | sed -n 's/.*_sig\([0-9][0-9]*\)\.log$/\1/p')"
  [ -n "$SIG" ] || SIG="-"
fi

# .so 目录：与 exe 同级的 ../lib（本仓库 out/lib）+ 环境变量可覆盖
SOLIB="${EVS_SOLIB:-$(dirname "$EXE_ABS")/../lib}"
[ -d "$SOLIB" ] || SOLIB=""

# ------------------------------------------------------------------ 跑一次 gdb
TMP="$(mktemp -d "${TMPDIR:-/tmp}/evs_collect.XXXXXX")" || exit 3
GDB_OUT="$TMP/gdb.txt"
# 关键顺序：file → set solib-search-path → core-file（core 一旦加载就去解析 .so，
# 之后再设 solib-search-path 已经晚了 —— 这就是"全是 ?? 帧"的根因）
gdb -q -batch -nx \
    -ex "set pagination off" -ex "set confirm off" \
    -ex "file $EXE_ABS" \
    ${SOLIB:+-ex "set solib-search-path $SOLIB"} \
    -ex "core-file $CORE_ABS" \
    -ex "echo ==BT==\n"  -ex "bt" \
    -ex "echo ==ALL==\n" -ex "thread apply all bt" \
    -ex "echo ==SO==\n"  -ex "info sharedlibrary" \
    > "$GDB_OUT" 2>&1
GDB_RC=$?

if ! grep -q '^==BT==$' "$GDB_OUT"; then
  echo "gdb 无法读取该 core（rc=$GDB_RC），原始输出：" >&2
  sed -n '1,40p' "$GDB_OUT" >&2
  echo "提示：core 与 exe 是否同一份构建？先跑 scripts/coredump/analyze_core.sh 验证。" >&2
  exit 4
fi

# ------------------------------------------------------------------ 切分 gdb 输出
awk -v bt="$TMP/crash_thread_bt.txt" -v all="$TMP/all_threads_bt.txt" \
    -v so="$TMP/sharedlibs.txt" '
  /^==BT==$/  {s=1; next}
  /^==ALL==$/ {s=2; next}
  /^==SO==$/  {s=3; next}
  s==1 {print > bt}
  s==2 {print > all}
  s==3 {print > so}
' "$GDB_OUT"

# --------------------------------------------------- 从崩溃线程栈里挑「真正的崩溃帧」
# 帧 #0 是 handler 自己、#1 是 __kernel_rt_sigreturn（或 raise/abort），都不是现场
pick_crash_frame() {
  local line rest sigp func loc file ln
  while IFS= read -r line; do
    case "$line" in '#'*) ;; *) continue ;; esac
    rest="${line#\#}"
    rest="${rest#"${rest%%[!0-9]*}"}"     # 去掉帧号
    rest="${rest#"${rest%%[! ]*}"}"       # 去掉帧号后的空格
    case "$rest" in *" at "*) ;; *) continue ;; esac   # 无符号帧（??）跳过
    sigp="${rest%% at *}"
    case "$sigp" in *" in "*) sigp="${sigp#* in }" ;; esac   # 去掉 "0x... in " 前缀
    func="${sigp%% (*}"
    loc="${rest##* at }"
    loc="${loc%% (discriminator*}"
    case "$func" in
      *OnFatalSignal*|*OnTerminate*|*crash_handler*|*__kernel_rt_sigreturn*|\
      *raise*|*abort*|*gsignal*|*pthread_kill*|*tgkill*|*__GI_*|\
      *start_thread*|*clone*) continue ;;
    esac
    file="${loc%:*}"; ln="${loc##*:}"
    case "$ln" in ''|*[!0-9]*) continue ;; esac
    printf '%s|%s|%s\n' "$func" "$file" "$ln"
    return 0
  done
  return 1
}

FUNC="-"; CRASH_FILE="-"; LINE="-"
if FRAME="$(pick_crash_frame < "$TMP/crash_thread_bt.txt")" && [ -n "$FRAME" ]; then
  IFS='|' read -r FUNC CRASH_FILE LINE <<< "$FRAME"
fi

SAFE_FUNC="$(printf '%s' "$FUNC" | tr -c 'A-Za-z0-9_:' '_' | cut -c1-60)"
SAFE_FILE="$(printf '%s' "${CRASH_FILE##*/}" | tr -c 'A-Za-z0-9._-' '_')"
if [ "$FUNC" = "-" ]; then
  TAG="noseymbol"
else
  TAG="${SAFE_FUNC}@${SAFE_FILE}:${LINE}"
fi

# -------------------------------------------------------------------- 建归档目录
ARCHIVE_ROOT="${OUT_ROOT:-$(dirname "$CORE_ABS")/archive}"
NAME="core.${APP}.${PID}.${EPOCH}.sig${SIG}.${TAG}"
DEST="$ARCHIVE_ROOT/$NAME"
mkdir -p "$DEST" || { echo "无法创建归档目录: $DEST" >&2; exit 5; }

if [ ! -e "$DEST/core" ]; then
  ln "$CORE_ABS" "$DEST/core" 2>/dev/null || cp -a "$CORE_ABS" "$DEST/core" || {
    echo "归档 core 失败（链接与拷贝都失败）: $DEST/core" >&2; exit 5; }
fi
cp -f "$TMP/crash_thread_bt.txt" "$DEST/" 2>/dev/null
[ -s "$TMP/all_threads_bt.txt" ] && cp -f "$TMP/all_threads_bt.txt" "$DEST/backtrace_all_threads.txt" 2>/dev/null
[ -s "$TMP/sharedlibs.txt" ] && cp -f "$TMP/sharedlibs.txt" "$DEST/" 2>/dev/null

# ----------------------------------------------- 用 pid 把同一次崩溃的报告配过来
if [ "$REPORT" != "-" ]; then
  cp -f "$REPORT" "$DEST/report.log" 2>/dev/null
fi

# ----------------------------------------------------------------------- meta.txt
BUILD_ID="$(readelf -n "$EXE_ABS" 2>/dev/null | awk '/Build ID/ {print $NF; exit}')"
EPOCH_HUMAN="$(date -d "@$EPOCH" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '?')"
{
  echo "collected_at  = $(date '+%Y-%m-%dT%H:%M:%S%z')"
  echo "core_file     = $CORE_ABS"
  echo "archive_dir   = $DEST"
  echo "exe           = $EXE_ABS"
  echo "exe_build_id  = ${BUILD_ID:-unknown}"
  echo "app           = ${APP}   (来自内核 %e 时上限 16 字节，可能被截断)"
  echo "pid           = ${PID}"
  echo "epoch         = ${EPOCH}  (${EPOCH_HUMAN})"
  echo "signal        = ${SIG}"
  echo "crash_func    = ${FUNC}"
  echo "crash_loc     = ${CRASH_FILE}:${LINE}"
  echo "report_log    = ${REPORT}"
  echo "core_pattern  = $(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo '?')"
  echo "kernel        = $(uname -r)"
  echo "host          = $(uname -n)"
  echo "collector_cwd = $PWD"
  echo "ulimit_c      = $(ulimit -Sc 2>/dev/null)/$(ulimit -Hc 2>/dev/null)"
  echo "solib_dir     = ${SOLIB:-(none)}"
  echo
  echo "--- 归档内文件 ---"
  echo "core                     原始 core（硬链接，可直接删除不影响原文件）"
  echo "crash_thread_bt.txt      崩溃线程栈（tgkill 保证就是崩溃现场）"
  echo "backtrace_all_threads.txt 全线程栈"
  echo "sharedlibs.txt           .so 加载情况（排查 ?? 帧）"
  echo "report.log               同 pid 的 crash_*.log（有人可读上下文）"
  echo
  echo "--- 复现 ---"
  echo "gdb -q \"$EXE_ABS\" \"$DEST/core\""
  echo "  (gdb) thread apply all bt full"
  echo "  (gdb) frame <崩溃帧>; info args; info locals; p *this"
} > "$DEST/meta.txt"

rm -rf "$TMP"

# ------------------------------------------------------------------------ 汇报
echo "== collect_core =="
printf '  core        : %s\n' "$CORE_ABS"
printf '  exe         : %s\n' "$EXE_ABS"
printf '  崩溃位置    : %s (%s:%s)   信号=%s\n' "$FUNC" "$CRASH_FILE" "$LINE" "$SIG"
printf '  归档目录    : %s\n' "$DEST"
printf '  配套报告    : %s\n' "$REPORT"
echo "  文件        : $(ls -1 "$DEST" | tr '\n' ' ')"
echo
echo "下一步："
echo "  scripts/coredump/analyze_core.sh \"$DEST/core\" \"$EXE_ABS\""
echo "  # 或在目录里直接： cat \"$DEST/meta.txt\""
exit 0

