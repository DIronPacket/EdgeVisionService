#!/usr/bin/env bash
# ============================================================================
# 崩溃报告符号化：把 "模块(符号+0x偏移) [地址]" 翻译成 "函数名 (文件:行号)"
# ----------------------------------------------------------------------------
# 用法：
#   scripts/coredump/symbolize_report.sh out/crash/crash_EdgeVisionService_1234_1731_sig11.log
#   EVS_MODROOT=/opt/EdgeVisionService scripts/coredump/symbolize_report.sh <报告>
#
# 原理：
#   报告是 src/common/crash/crash_handler.cpp 用 backtrace_symbols_fd() 生成的，
#   格式固定为「模块(符号+0x偏移) [绝对地址]」。
#   addr2line 要的是「模块文件 + 文件内偏移」，正好是括号里的 0x偏移：
#   PIE 可执行文件与 .so 的加载基址 dli_fbase 都对应文件内 vaddr=0，
#   所以 addr - base == 0x偏移，直接喂给 addr2line 是对的。
# ============================================================================
set -u

REPORT="${1:-}"
if [ -z "$REPORT" ] || [ ! -f "$REPORT" ]; then
  echo "用法: $0 <crash 报告 .log>"
  exit 1
fi

ADDR2LINE="${ADDR2LINE:-addr2line}"
MODROOT="${EVS_MODROOT:-}"

echo "== 报告: $REPORT =="
# 只解析 "call stack" 到 "下一步" 之间的帧行
sed -n '/^-- call stack/,/^-- 下一步/p' "$REPORT" \
  | grep -F '+0x' \
  | while IFS= read -r line; do
      # 行格式固定为「模块(符号+0x偏移)[0x地址]」；
      # 用参数展开取值，避免 bash 正则对 \( \+ 的方言差异（实测会漏匹配）
      case "$line" in
        *'('*'+0x'*']'*) ;;
        *) continue ;;
      esac
      mod="${line%%\(*}"
      rest="${line#*\(}"
      inside="${rest%\)*}"
      off="${inside##*+0x}"
      sym="${inside%+0x*}"
      case "$mod" in
        /*) real_mod="$mod" ;;
        *)  real_mod="${MODROOT:+$MODROOT/}${mod#./}" ;;
      esac

      # backtrace_symbols 的两种形态：
      #   模块(符号+0x偏移)  → 偏移是「符号内」偏移，要先查符号 vaddr 再加
      #   模块(+0x偏移)      → 偏移已是模块内偏移（相对加载基址），PIE 下 == 文件 vaddr
      target="0x$off"
      if [ -n "$sym" ]; then
        sym_vaddr="$(nm -D --defined-only "$real_mod" 2>/dev/null \
                     | awk -v s="$sym" '$3 == s {print $1; exit}')"
        if [ -z "$sym_vaddr" ]; then
          sym_vaddr="$(nm --defined-only "$real_mod" 2>/dev/null \
                       | awk -v s="$sym" '$3 == s {print $1; exit}')"
        fi
        if [ -n "$sym_vaddr" ]; then
          target="$(printf '0x%x' "$(( 16#$sym_vaddr + 16#$off ))")"
        fi
      fi

      loc=""
      if [ -f "$real_mod" ] && command -v "$ADDR2LINE" >/dev/null 2>&1; then
        loc="$("$ADDR2LINE" -f -C -i -e "$real_mod" "$target" 2>/dev/null \
               | head -2 | paste -sd' ' -)"
      fi

      printf '%s\n' "$line"
      if [ -n "$loc" ]; then
        printf '    -> %s\n' "$loc"
      else
        printf '    -> (无法解析: 模块=%s 偏移=0x%s；相对路径请用 EVS_MODROOT=<部署根目录>)\n' \
               "$real_mod" "$off"
      fi
    done
