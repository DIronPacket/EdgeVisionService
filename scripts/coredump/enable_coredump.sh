#!/usr/bin/env bash
# ============================================================================
# 让本机「自动落 core dump」，且文件名自带 程序名 + pid + 时间
# ----------------------------------------------------------------------------
# 用法（需要 root）：
#   sudo scripts/coredump/enable_coredump.sh
#   sudo EVS_CORE_DIR=/data/core scripts/coredump/enable_coredump.sh
#
# 做四件事（幂等，可反复执行）：
#   1. 建落盘目录（1777，任何服务用户都能写）
#   2. 写 /proc/sys/kernel/core_pattern（立即生效）
#      %e = 可执行名（内核截断到 16 字节，EdgeVisionService → EdgeVisionServic）
#      %p = pid   %t = epoch 秒   %s = 触发信号编号
#      → core.EdgeVisionServic.12345.1731500000.sig11
#   3. 持久化 /etc/sysctl.d/90-evs-coredump.conf（core_pattern 是内核参数，
#      重启后会回到发行版默认值 "core"，那样只有 core.<pid>，看不出是谁/何时/什么信号）
#   4. 持久化 /etc/security/limits.d/90-evs-coredump.conf（登录 shell 的 ulimit -c）
#      ⚠ systemd 服务不读 limits.conf，托管时要在单元里写 LimitCORE=infinity
#
# 说明：core_pattern 若以 "|" 开头表示交给用户态程序处理（apport/systemd-coredump）。
#       本脚本会把它改成「直接写文件」，因为本机没有装 systemd-coredump。
#       函数名/文件名**不可能**进 core 文件名（内核不认识用户态符号），
#       用 scripts/coredump/collect_core.sh 事后归档改名，见 docs/ops/CRASH_DUMP.md §2.3。
# ============================================================================
set -u

DUMP_DIR="${EVS_CORE_DIR:-/var/lib/evs/coredump}"
PATTERN="${DUMP_DIR}/core.%e.%p.%t.sig%s"

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  sed -n '2,20p' "$0"
  exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "需要 root（要改 /proc/sys 与 /etc）：sudo $0"
  echo "只想当前 shell 生效可以先执行：  ulimit -c unlimited"
  exit 1
fi

echo "== 改前 =="
echo "core_pattern      : $(cat /proc/sys/kernel/core_pattern)"
echo "core_uses_pid     : $(cat /proc/sys/kernel/core_uses_pid 2>/dev/null || echo '?')"
echo "ulimit -c soft/hard: $(ulimit -Sc) / $(ulimit -Hc)"
echo "coredump_filter   : $(cat /proc/self/coredump_filter 2>/dev/null || echo '?')"

mkdir -p "$DUMP_DIR" || exit 1
chmod 1777 "$DUMP_DIR"

# 绝对路径才会被内核按路径解析；%e/%p/%t 保证「哪个程序、哪个进程、什么时候」
echo "$PATTERN" > /proc/sys/kernel/core_pattern || exit 1
echo 1 > /proc/sys/kernel/core_uses_pid 2>/dev/null || true

# 持久化（重启后仍生效）
cat > /etc/sysctl.d/90-evs-coredump.conf <<EOF
# EdgeVisionService: core dump 落盘位置
# %e 可执行名 / %p pid / %t epoch 秒
kernel.core_pattern = ${PATTERN}
kernel.core_uses_pid = 1
EOF

# 登录 shell 的 core 大小限制（systemd 服务另见单元文件 LimitCORE）
cat > /etc/security/limits.d/90-evs-coredump.conf <<'EOF'
# EdgeVisionService: 允许生成 core dump
*  soft  core  unlimited
*  hard  core  unlimited
EOF

sysctl --system >/dev/null 2>&1 || true

echo "== 改后 =="
echo "core_pattern      : $(cat /proc/sys/kernel/core_pattern)"
echo "落盘目录          : ${DUMP_DIR}"
echo
echo "下一步："
echo "  1) 当前 shell 立即生效：  ulimit -c unlimited"
echo "  2) 复现崩溃后找 core ：   ls -lt ${DUMP_DIR} | head"
echo "  3) 解析 core 看变量  ：   scripts/coredump/analyze_core.sh <core> out/bin/EdgeVisionService"
echo "  4) 用 systemd 托管时    参考 scripts/deploy/evs-edgevision.service（LimitCORE=infinity）"
