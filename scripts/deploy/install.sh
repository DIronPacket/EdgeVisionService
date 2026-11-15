#!/usr/bin/env bash
# ============================================================================
# scripts/deploy/install.sh —— 三进程开机自启的一键安装 / 自检 / 卸载
# ----------------------------------------------------------------------------
# 做的事：把 scripts/deploy/evs-{media,comm,upgrade,rollback}.service 与 evs.target
#         里的 @@占位符@@ 渲染成本机绝对路径，装进 /etc/systemd/system/，
#         再 daemon-reload + enable（默认立刻启动）。
#
# 用法（在仓库根执行）：
#   sudo scripts/deploy/install.sh install                 # 装 5 个 unit + 开机自启 + 立即启动
#   sudo scripts/deploy/install.sh install --no-start      # 只 enable，不启动
#   sudo scripts/deploy/install.sh install --only media,comm
#        scripts/deploy/install.sh install --dry-run       # 只渲染到临时目录 + systemd-analyze verify（不需要 root，不动系统）
#        scripts/deploy/install.sh status                  # 看聚合状态 + 三个进程 + LimitCORE
#   sudo scripts/deploy/install.sh uninstall               # 停服 + 删 unit（数据目录保留）
#
# 可覆盖：
#   --root DIR       部署根（默认 = 本脚本所在仓库根）
#   --user NAME      进程运行用户（默认 root；采集与 systemctl 都需要权限）
#   --data-dir DIR   崩溃报告/运行数据目录（默认 /var/lib/evs）
#   --platform P     库目录平台名（默认 rk3588）
#
# 退出码：0 成功；1 用法/环境错误；2 校验失败
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
USER_NAME="root"
DATA_DIR="/var/lib/evs"
PLATFORM="rk3588"
ONLY="all"
DO_START=1
DRY_RUN=0
ACTION="install"
UNITS="evs-media.service evs-comm.service evs-upgrade.service evs-rollback.service evs.target"
SYSD_DIR="/etc/systemd/system"

log()  { printf '[deploy] %s\n' "$*"; }
warn() { printf '[deploy][WARN] %s\n' "$*" >&2; }
die()  { printf '[deploy][FAIL] %s\n' "$*" >&2; exit 2; }

usage() { sed -n '3,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

# ---------------------------------------------------------------- 参数解析
while [ $# -gt 0 ]; do
  case "$1" in
    install|status|uninstall) ACTION="$1" ;;
    --root)       ROOT="${2:?--root 需要目录}"; shift ;;
    --root=*)     ROOT="${1#*=}" ;;
    --user)       USER_NAME="${2:?--user 需要用户名}"; shift ;;
    --user=*)     USER_NAME="${1#*=}" ;;
    --data-dir)   DATA_DIR="${2:?--data-dir 需要目录}"; shift ;;
    --data-dir=*) DATA_DIR="${1#*=}" ;;
    --platform)   PLATFORM="${2:?--platform 需要值}"; shift ;;
    --platform=*) PLATFORM="${1#*=}" ;;
    --only)       ONLY="${2:?--only 需要 media,comm,...}"; shift ;;
    --only=*)     ONLY="${1#*=}" ;;
    --no-start)   DO_START=0 ;;
    --dry-run)    DRY_RUN=1 ;;
    -h|--help)    usage; exit 0 ;;
    *) die "不认识的参数：$1（--help 看用法）" ;;
  esac
  shift
done

ROOT="$(cd "$ROOT" && pwd)"
LIB_PATH="$ROOT/out/lib:$ROOT/install/$PLATFORM/lib"

# ---------------------------------------------------------------- 前置检查
check_artifacts() {
  local missing=0 b
  for b in evs_media evs_comm evs_upgrade; do
    if [ ! -x "$ROOT/out/bin/$b" ]; then
      warn "缺可执行文件：$ROOT/out/bin/$b"
      missing=1
    fi
  done
  [ -d "$ROOT/install/$PLATFORM/lib" ] || warn "缺库目录：$ROOT/install/$PLATFORM/lib（LD_LIBRARY_PATH 会少一段）"
  if [ "$missing" = 1 ]; then
    die "先构建：make            （或只编某个：make media / make comm / make upgrade）"
  fi
}

want() {   # want <media|comm|upgrade|rollback|target>
  [ "$ONLY" = "all" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; *) return 1 ;; esac
}

# ---------------------------------------------------------------- 渲染
render_one() {  # render_one <unit 文件名> <输出目录>
  local f="$1" out="$2"
  local src="$SCRIPT_DIR/$f"
  [ -f "$src" ] || die "缺模板：$src"
  sed -e "s|@@EVS_ROOT@@|$ROOT|g" \
      -e "s|@@EVS_DATA@@|$DATA_DIR|g" \
      -e "s|@@EVS_USER@@|$USER_NAME|g" \
      -e "s|@@EVS_PLATFORM@@|$PLATFORM|g" \
      "$src" > "$out/$f"
  # ProtectHome=yes 会让 /home 与 /root 下的路径不可读 —— 仓库放在那里时必须关掉
  case "$ROOT" in
    /home/*|/root/*)
      sed -i 's|^ProtectHome=yes|#ProtectHome=yes   # 关掉：部署根在 /home 或 /root 下|' "$out/$f"
      ;;
  esac
}

render_all() {  # render_all <输出目录>
  local out="$1"
  mkdir -p "$out"
  local f
  for f in $UNITS; do
    case "$f" in
      evs-media.service)   want media   || continue ;;
      evs-comm.service)    want comm    || continue ;;
      evs-upgrade.service) want upgrade || continue ;;
      evs-rollback.service) want rollback || continue ;;
      evs.target)          want target  || continue ;;
    esac
    render_one "$f" "$out"
  done
  # 模板里注释掉的 OnFailure/ExecStartPost 由 run 阶段是否含 rollback/target 决定不了，
  # 这里保持模板原样（首期有意关闭），P4 联调时在模板里放开。
}

print_summary() {  # print_summary <unit 目录>
  local d="$1"
  log "部署根      = $ROOT"
  log "运行用户    = $USER_NAME"
  log "数据目录    = $DATA_DIR（崩溃报告 → $DATA_DIR/crash）"
  log "库路径      = $LIB_PATH"
  log "渲染产物    ="
  local f
  for f in $UNITS; do
    if [ -f "$d/$f" ]; then log "              $d/$f"; fi
  done
}

# ---------------------------------------------------------------- 语法校验
verify_units() {  # verify_units <unit 目录>；返回非 0 = 有硬错误
  local d="$1"
  if ! command -v systemd-analyze >/dev/null 2>&1; then
    warn "没有 systemd-analyze，跳过 unit 语法校验"
    return 0
  fi
  local files
  files="$(ls "$d"/*.service "$d"/*.target 2>/dev/null || true)"
  [ -n "$files" ] || return 0
  local out rc=0 noise keep
  # SYSTEMD_UNIT_PATH 必须带结尾冒号：追加而非替换默认路径，否则 sysinit.target 等找不到
  out="$(SYSTEMD_UNIT_PATH="$d:" systemd-analyze verify $files 2>&1)" || rc=$?
  if [ -n "$out" ]; then
    # 非 root 跑 verify 时，它读 /run/systemd/system 下的系统 unit 会 Permission denied，
    # 与本项目无关，过滤掉避免误判（保留计数，便于发现真的权限问题）
    noise="$(printf '%s\n' "$out" | grep -E '/run/systemd/system/|netplan' || true)"
    keep="$(printf '%s\n' "$out" | grep -vE '/run/systemd/system/|netplan' || true)"
    if [ -n "$keep" ]; then printf '%s\n' "$keep" | sed 's/^/[verify] /'; fi
    if [ -n "$noise" ]; then
      log "（已过滤 $(printf '%s\n' "$noise" | wc -l | tr -d ' ') 行与本项目无关的 verify 噪音）"
    fi
  fi
  return $rc
}

# ---------------------------------------------------------------- install
do_install() {
  check_artifacts

  local tmp
  tmp="$(mktemp -d /tmp/evs-units.XXXXXX)"
  render_all "$tmp"
  print_summary "$tmp"

  local vrc=0
  verify_units "$tmp" || vrc=$?
  if [ "$vrc" -ne 0 ]; then
    warn "systemd-analyze verify 返回 $vrc（见上面的 [verify] 行）"
  fi

  if [ "$DRY_RUN" = 1 ]; then
    log "dry-run：以上文件已渲染到 $tmp，未写入 $SYSD_DIR"
    log "dry-run：装到本机用  sudo scripts/deploy/install.sh install"
    [ "$vrc" -eq 0 ] || exit 2
    exit 0
  fi

  [ "$(id -u)" = "0" ] || die "install 需要 root：sudo scripts/deploy/install.sh install"

  # 单进程兼容壳与三进程栈不要同时开（会抢 RTSP 端口 8550）
  if systemctl is-enabled evs-edgevision.service >/dev/null 2>&1; then
    warn "evs-edgevision.service（单进程兼容壳）处于 enable 状态，先执行："
    warn "  sudo systemctl disable --now evs-edgevision"
  fi

  mkdir -p "$DATA_DIR/crash"
  local f
  for f in $UNITS; do
    [ -f "$tmp/$f" ] || continue
    install -m 0644 "$tmp/$f" "$SYSD_DIR/$f"
    log "已安装 $SYSD_DIR/$f"
  done
  systemctl daemon-reload

  if want target; then
    systemctl enable evs.target
    log "已 enable evs.target（开机自启三进程）"
  fi
  for f in evs-media.service evs-comm.service evs-upgrade.service; do
    [ -f "$tmp/$f" ] && { systemctl enable "$f" >/dev/null 2>&1 || true; }
  done

  if [ "$DO_START" = 1 ]; then
    local started=""
    for f in evs-media.service evs-comm.service evs-upgrade.service; do
      [ -f "$tmp/$f" ] && started="$started $f"
    done
    if [ -n "$started" ]; then
      # 不用 `systemctl restart evs.target`：target 的 restart 语义是停/起整个 target，
      # 单独 restart 三个 unit 更可控，也不会误停手工调试中的其他单元。
      systemctl restart $started
      log "已启动：$started"
    fi
  else
    log "--no-start：只 enable，未启动"
  fi

  rm -rf "$tmp"
  log "完成。自检：scripts/deploy/install.sh status"
  log "日志：journalctl -u evs-media -u evs-comm -u evs-upgrade -f"
}

# ---------------------------------------------------------------- status
do_status() {
  local u missing=0
  for u in evs-media.service evs-comm.service evs-upgrade.service evs-rollback.service evs.target; do
    if ! systemctl cat "$u" >/dev/null 2>&1; then
      warn "未安装：$u"
      missing=1
    fi
  done
  if [ "$missing" = 1 ]; then log "安装：sudo scripts/deploy/install.sh install"; fi

  for u in evs.target evs-media.service evs-comm.service evs-upgrade.service; do
    systemctl cat "$u" >/dev/null 2>&1 || continue
    printf '\n==== %s ====\n' "$u"
    systemctl --no-pager -l status "$u" 2>&1 | sed -n '1,12p' || true
  done

  printf '\n==== 关键项（core 与 cwd） ====\n'
  for u in evs-media.service evs-comm.service evs-upgrade.service; do
    systemctl cat "$u" >/dev/null 2>&1 || continue
    printf '%-22s %s | %s\n' "$u" \
      "$(systemctl show -p LimitCORE --value "$u")" \
      "$(systemctl show -p WorkingDirectory --value "$u")"
  done
  printf '\n==== 进程 ====\n'
  pgrep -a -f 'out/bin/evs_(media|comm|upgrade)' || echo "（没有 evs_* 进程在跑）"
}

# ---------------------------------------------------------------- uninstall
do_uninstall() {
  [ "$(id -u)" = "0" ] || die "uninstall 需要 root：sudo scripts/deploy/install.sh uninstall"
  systemctl disable --now evs.target >/dev/null 2>&1 || true
  systemctl stop evs-media.service evs-comm.service evs-upgrade.service >/dev/null 2>&1 || true
  local f
  for f in $UNITS; do
    if [ -f "$SYSD_DIR/$f" ]; then
      rm -f "$SYSD_DIR/$f"
      log "已删除 $SYSD_DIR/$f"
    fi
  done
  systemctl daemon-reload
  log "已卸载。数据目录保留：$DATA_DIR（要清就 rm -rf $DATA_DIR）"
}

case "$ACTION" in
  install)   do_install ;;
  status)    do_status ;;
  uninstall) do_uninstall ;;
  *)         usage; exit 1 ;;
esac

