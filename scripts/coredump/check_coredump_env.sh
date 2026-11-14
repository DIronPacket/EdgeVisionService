#!/usr/bin/env bash
# ============================================================================
# 崩溃现场体检：本机现在能不能抓到 core（**只读**，不改任何系统参数）
# ----------------------------------------------------------------------------
# 用法：
#   scripts/coredump/check_coredump_env.sh          # 默认检查 /var/lib/evs/coredump
#   EVS_CORE_DIR=/data/core scripts/coredump/check_coredump_env.sh
#
# 输出：每项一行 PASS / WARN / FAIL，末尾给出「要做什么」。
# 退出码：0 = 没有 FAIL（能抓到 core，或只差 root 执行 enable_coredump.sh）；
#         1 = 有 FAIL（当前无论如何都抓不到）。
#
# 判定依据见 docs/ops/CRASH_DUMP.md §3：信号产生 → handler → RLIMIT_CORE →
# RLIMIT_FSIZE → dumpable → core_pattern 落盘 → 事后解析。
# ============================================================================
set -u

DUMP_DIR="${EVS_CORE_DIR:-/var/lib/evs/coredump}"
PASS_N=0
WARN_N=0
FAIL_N=0

pass() { PASS_N=$((PASS_N + 1)); printf '  [PASS] %s\n' "$*"; }
warn() { WARN_N=$((WARN_N + 1)); printf '  [WARN] %s\n' "$*"; }
fail() { FAIL_N=$((FAIL_N + 1)); printf '  [FAIL] %s\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  sed -n '2,16p' "$0"
  exit 0
fi

echo "== 崩溃现场体检 (docs/ops/CRASH_DUMP.md §3) =="
echo "kernel : $(uname -r)   host: $(uname -n)   user: $(id -un)"
echo "预期落盘目录: $DUMP_DIR   (只读检查，不会创建)"

# ---------------------------------------------------------------- 1. core_pattern
echo "== 1. core_pattern =="
PATTERN="$(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo '')"
echo "  当前值: '${PATTERN}'"
if [ -z "$PATTERN" ]; then
  fail "core_pattern 读不到或为空 → 内核不会写 core"
elif [ "${PATTERN#|}" != "$PATTERN" ]; then
  warn "core_pattern 是管道（交给 apport / systemd-coredump），core 不在普通路径；用 'coredumpctl list' 找"
else
  case "$PATTERN" in
    /*) pass "core_pattern 是绝对路径（不依赖进程 cwd）" ;;
    *)  warn "core_pattern 是相对路径 '${PATTERN}' → core 落在进程 cwd；systemd 下 cwd 可能是 '/'（不可写就静默丢失）" ;;
  esac
  case "$PATTERN" in
    *%p*) pass "pattern 含 %p（多进程/多次崩溃不会互相覆盖）" ;;
    *)    warn "pattern 不含 %p（core_uses_pid=1 时会自动补 .pid，但重启后 pid 复用仍会覆盖）" ;;
  esac
  case "$PATTERN" in
    *%t*) pass "pattern 含 %t（崩溃时间戳，可排序）" ;;
    *)    warn "pattern 不含 %t → 无法按时间排序/区分多次崩溃" ;;
  esac
  case "$PATTERN" in
    *%e*) : ;;
    *)    warn "pattern 不含 %e → 文件名看不出是哪个程序" ;;
  esac
  PAT_DIR="$(dirname "$PATTERN")"
  if [ "$PAT_DIR" != "." ] && [ -d "$PAT_DIR" ]; then
    if [ -w "$PAT_DIR" ]; then
      pass "pattern 目录可写: $PAT_DIR"
    else
      warn "pattern 目录当前用户不可写: $PAT_DIR（服务用户可能更糟）"
    fi
  elif [ "$PAT_DIR" != "." ]; then
    warn "pattern 目录不存在: $PAT_DIR → 内核静默丢弃 core（mkdir -p 或执行 enable_coredump.sh）"
  fi
fi
echo "  core_uses_pid : $(cat /proc/sys/kernel/core_uses_pid 2>/dev/null || echo '?')"

# ------------------------------------------------------------- 2. 磁盘 / 文件系统
echo "== 2. 落盘空间与文件系统 =="
CHECK_DIR="$DUMP_DIR"
[ -d "$CHECK_DIR" ] || CHECK_DIR="$(dirname "$DUMP_DIR")"
if [ -d "$CHECK_DIR" ]; then
  FS="$(findmnt -no FSTYPE --target "$CHECK_DIR" 2>/dev/null || echo '?')"
  AVAIL="$(df -h --output=avail "$CHECK_DIR" 2>/dev/null | tail -1 | tr -d ' ')"
  echo "  $CHECK_DIR : fstype=$FS  avail=$AVAIL"
  case "$FS" in
    tmpfs|ramfs) warn "$CHECK_DIR 在 $FS 上 → core 直接吃内存，大 core(300MB+) 可能 OOM" ;;
    *)           pass "$CHECK_DIR 在真实存储（$FS）上" ;;
  esac
  AVAIL_K="$(df -k --output=avail "$CHECK_DIR" 2>/dev/null | tail -1 | tr -d ' ')"
  if [ -n "${AVAIL_K:-}" ] && [ "$AVAIL_K" -lt 1048576 ] 2>/dev/null; then
    warn "$CHECK_DIR 剩余不足 1 GB（本项目 core 实测 350 MB）"
  fi
else
  warn "目录不存在: $DUMP_DIR → 需要 mkdir -p 或执行 enable_coredump.sh"
fi

# --------------------------------------------------------------- 3. rlimit（核心）
echo "== 3. rlimit（进程能写多大的 core）=="
UL_SOFT="$(ulimit -Sc 2>/dev/null || echo '?')"
UL_HARD="$(ulimit -Hc 2>/dev/null || echo '?')"
echo "  ulimit -c  soft=$UL_SOFT  hard=$UL_HARD"
if [ "$UL_HARD" = "0" ]; then
  fail "hard limit 是 0 → 进程无权抬高，**一个 core 都不会有**（root 改 limits.d 或 unit 写 LimitCORE=infinity）"
elif [ "$UL_SOFT" = "0" ]; then
  warn "soft=0 但 hard=$UL_HARD → 裸跑默认不落 core；本仓库 handler 会 setrlimit 抬到 hard，所以能抓（已实测）"
else
  pass "soft=$UL_SOFT > 0"
fi
UL_FSIZE="$(ulimit -Sf 2>/dev/null || echo '?')"
echo "  ulimit -f  $UL_FSIZE"
if [ "$UL_FSIZE" = "unlimited" ]; then
  pass "RLIMIT_FSIZE 不限（core 不会被截断删除）"
else
  warn "RLIMIT_FSIZE=$UL_FSIZE → 写 core 超限会被内核删掉半成品"
fi

# ------------------------------------------------------------------ 4. dumpable
echo "== 4. dumpable / 安全模块 =="
SUID_DUMPABLE="$(cat /proc/sys/fs/suid_dumpable 2>/dev/null || echo '?')"
echo "  fs.suid_dumpable = $SUID_DUMPABLE"
if [ "$SUID_DUMPABLE" = "0" ]; then
  pass "0（表示 suid 进程不写 core；本项目非 suid，无影响）"
else
  warn "非 0：suid 进程也会写 core（有信息泄露风险，通常无需改）"
fi
if [ -d /sys/module/apparmor/parameters ]; then
  AA="$(cat /sys/module/apparmor/parameters/enabled 2>/dev/null || echo '?')"
  if [ "$AA" = "Y" ]; then warn "AppArmor 已启用：若 profile 未允许写 $DUMP_DIR，core 会被拦"; else pass "AppArmor 未启用"; fi
else
  pass "无 AppArmor"
fi
if [ -e /sys/fs/selinux/enforce ]; then
  warn "SELinux 存在（enforce=$(cat /sys/fs/selinux/enforce 2>/dev/null)）：可能拦 core 落盘"
else
  pass "无 SELinux"
fi

# --------------------------------------------------------- 5. coredump 过滤器等
echo "== 5. 其它内核开关 =="
echo "  core_pipe_limit   : $(cat /proc/sys/kernel/core_pipe_limit 2>/dev/null || echo '?')  (只在 pattern 为管道时相关)"
FILTER="$(cat /proc/self/coredump_filter 2>/dev/null || echo '?')"
echo "  coredump_filter   : $FILTER  (本进程值)"
if [ "$FILTER" != "?" ]; then
  # bit2 (0x4) = 私有文件映射（代码段）；不带它 disassemble 看不到指令
  if [ $((16#$FILTER & 4)) -eq 0 ] 2>/dev/null; then
    warn "不含 bit2(私有文件映射) → gdb 里看不到代码段（变量/栈仍完整，通常可接受）"
  else
    pass "含代码段（可 disassemble）"
  fi
fi
if have coredumpctl; then
  warn "检测到 coredumpctl（systemd-coredump 可能接管 core）：用 'coredumpctl list' 查，而不是找文件"
else
  pass "无 coredumpctl（core 由内核直接写文件）"
fi
if [ -e /.dockerenv ] || grep -qaE 'docker|lxc|kubepods' /proc/1/cgroup 2>/dev/null; then
  warn "看起来在容器里：core 的落点由**宿主** core_pattern 决定，容器内可能看不到"
fi

# ------------------------------------------------------- 6. 是否已持久化 / 已生效
echo "== 6. 持久化配置 =="
if [ -f /etc/sysctl.d/90-evs-coredump.conf ]; then
  pass "已有 /etc/sysctl.d/90-evs-coredump.conf"
else
  warn "无 /etc/sysctl.d/90-evs-coredump.conf → 重启后 core_pattern 回到默认（执行 enable_coredump.sh）"
fi
if [ -f /etc/security/limits.d/90-evs-coredump.conf ]; then
  pass "已有 /etc/security/limits.d/90-evs-coredump.conf"
else
  warn "无 limits.d 配置（登录 shell 的 ulimit -c 仍可能是 0；systemd 服务请用 LimitCORE=infinity）"
fi
if have systemctl; then
  if systemctl list-unit-files 2>/dev/null | grep -q '^evs-edgevision'; then
    echo "  evs-edgevision 单元 LimitCORE = $(systemctl show -p LimitCORE evs-edgevision 2>/dev/null | cut -d= -f2)"
  else
    echo "  （未安装 evs-edgevision 单元；开机自启见 docs/ops/COMMANDS.md §12）"
  fi
fi

# --------------------------------------------------------------- 7. 结论与建议
echo "== 结论 =="
printf '  PASS=%d  WARN=%d  FAIL=%d\n' "$PASS_N" "$WARN_N" "$FAIL_N"
if [ "$FAIL_N" -gt 0 ]; then
  echo "  → 当前**抓不到 core**。先解决 FAIL 项（多数需要 root）："
  echo "     sudo scripts/coredump/enable_coredump.sh"
  exit 1
fi
if [ "$WARN_N" -gt 0 ]; then
  echo "  → 现在能抓（前台运行已实测），但换 systemd 托管/换机器前建议："
  echo "     sudo scripts/coredump/enable_coredump.sh    # 固定绝对路径 + %e.%p.%t.sig%s"
  echo "     再跑一次本脚本自检"
  exit 0
fi
echo "  → 全部正常。复现崩溃后用 scripts/coredump/collect_core.sh 归档。"
exit 0
