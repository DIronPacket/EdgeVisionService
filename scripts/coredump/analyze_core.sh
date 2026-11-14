#!/usr/bin/env bash
# ============================================================================
# 用 gdb 批量解析 core（崩溃 core，或 gcore 抓的活进程快照）
# ----------------------------------------------------------------------------
# 用法：
#   scripts/coredump/analyze_core.sh <core 文件> [可执行文件] [输出 txt]
#   例：
#   scripts/coredump/analyze_core.sh /var/lib/evs/coredump/core.EdgeVisionService.1234.1731
#   scripts/coredump/analyze_core.sh crash/live_EdgeVisionService_1234_1731.1234 \
#                                out/bin/EdgeVisionService
#
# 自动收集（结果写 txt，并打印前 80 行）：
#   info threads                哪些线程、各自停在哪个函数
#   thread apply all bt full    每帧的「参数 + 局部变量」← 这就是「哪个变量」
#   info registers              崩溃线程寄存器（pc/sp/lr/fp）
#   info sharedlibrary          各 .so 实际加载基址（核对是否与当时同版本）
# ============================================================================
set -u

CORE="${1:-}"
EXE="${2:-out/bin/EdgeVisionService}"
OUT="${3:-}"

if [ -z "$CORE" ] || [ ! -f "$CORE" ]; then
  echo "用法: $0 <core> [可执行] [输出txt]"
  exit 1
fi
command -v gdb >/dev/null 2>&1 || { echo "需要 gdb：sudo apt install gdb"; exit 1; }
[ -f "$EXE" ] || { echo "可执行文件不存在: $EXE（用参数 2 指定）"; exit 1; }

OUT="${OUT:-${CORE}.gdb.txt}"
# 让 gdb 找到 out/lib 与 install/ 下的 .so。必须是绝对路径：
# gdb 解析 core 的 NT_FILE 相对路径（lib/libxxx.so）时按 solib-search-path 拼，
# 相对路径再相对 gdb 的 cwd 会双重偏移。部署路径不同时用 EVS_SOLIB_PATH 覆盖。
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SOLIB="${EVS_SOLIB_PATH:-$ROOT/out/lib:$ROOT/install/rk3588/lib}"

# 注意顺序：exe/core 必须用 file/core-file 显式加载，且放在 set solib-search-path
# 之后。若写成 `gdb exe core -ex 'set solib-search-path ...'`，core 会在所有 -ex
# 之前就被读进来，.so 全部找不到（本机实测：14 个库 missing）。
gdb -q -batch \
  -ex "set pagination off" \
  -ex "set width 0" \
  -ex "set print pretty on" \
  -ex "set print elements 64" \
  -ex "set sysroot /" \
  -ex "set solib-search-path ${SOLIB}" \
  -ex "file ${EXE}" \
  -ex "core-file ${CORE}" \
  -ex "echo \n===== threads =====\n" \
  -ex "info threads" \
  -ex "echo \n===== thread apply all bt full (参数 + 局部变量) =====\n" \
  -ex "thread apply all bt full" \
  -ex "echo \n===== registers (current thread) =====\n" \
  -ex "info registers" \
  -ex "echo \n===== shared libraries (版本对齐检查) =====\n" \
  -ex "info sharedlibrary" >"$OUT" 2>&1

rc=$?
echo "== gdb 退出码: $rc =="
echo "== 输出: $OUT =="
head -80 "$OUT"
echo "..."
echo
echo "提示："
echo "  - 看指定帧的变量："
echo "      gdb -q $EXE $CORE -ex 'thread apply all bt' -ex 'frame N' \\"
echo "          -ex 'info args' -ex 'info locals' -ex 'p *this'"
echo "  - 变量显示 <optimized out> 说明是 -O2 构建：用 Debug 构建复现"
echo "  - 地址对不上行号：二进制/.so 已被重新编译，core 必须配当时的产物"
