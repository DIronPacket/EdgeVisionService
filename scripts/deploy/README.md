# scripts/deploy/ —— 部署产物（三进程 systemd 单元 + 一键安装）

> 目标：**一条命令让 `evs_media` + `evs_comm` + `evs_upgrade` 三个进程开机自启**。
> 完整命令与实测输出在 [../../docs/ops/COMMANDS.md §12](../../docs/ops/COMMANDS.md)；
> 升级相关的 unit 设计依据在 [../../docs/design/DESIGN_UPGRADE.md §7.4 / §11.4](../../docs/design/DESIGN_UPGRADE.md)。

```text
scripts/deploy/
├── README.md                ← 本页
├── install.sh               ← ★ 渲染 + 安装 + 启停 + 自检 + 卸载
├── evs.target               ← 聚合入口（开机只 enable 这一个）
├── evs-media.service        ← 进程 1：evs_media
├── evs-comm.service         ← 进程 2：evs_comm
├── evs-upgrade.service      ← 进程 3：evs_upgrade
├── evs-rollback.service     ← 兜底回滚（oneshot，由 OnFailure 触发）
└── evs-edgevision.service   ← 历史单进程单元（兼容壳，仅调试；勿与 evs.target 同开）
```

## 1. 装 / 看 / 卸

```bash
# 装（需要 root）：渲染 → 拷 5 个 unit → daemon-reload → enable evs.target → 立刻启动
sudo scripts/deploy/install.sh install

# 只渲染 + systemd-analyze verify，不碰系统（不需要 root）
scripts/deploy/install.sh install --dry-run

# 状态：三个进程 + target + LimitCORE + WorkingDirectory + pgrep
scripts/deploy/install.sh status

# 卸（停服 + 删 unit；数据目录 /var/lib/evs 保留）
sudo scripts/deploy/install.sh uninstall
```

常用选项：`--only media,comm`（只装部分）、`--no-start`（只 enable 不启动）、
`--root DIR`（部署根，默认仓库根）、`--user NAME`（默认 root）、`--data-dir DIR`（默认 `/var/lib/evs`）、
`--platform rk3588`。

## 2. install.sh 到底改了什么

| 动作 | 说明 |
| --- | --- |
| 渲染 `@@EVS_ROOT@@` / `@@EVS_DATA@@` / `@@EVS_USER@@` / `@@EVS_PLATFORM@@` | 单元里不写死路径；`LD_LIBRARY_PATH` 直接落成绝对路径（systemd 的 `Environment=` 不做 shell 展开，写 `$VAR` 会静默失效） |
| `ProtectHome=yes` 自动降级 | 部署根在 `/home/*` 或 `/root/*` 下时该行会挡住二进制读取，脚本自动注释掉 |
| `systemd-analyze verify` | `--dry-run` 与 `install` 都会跑；`SYSTEMD_UNIT_PATH=<渲染目录>:` 让 `Wants=/After=` 能解析到同目录 unit |
| 冲突检测 | `evs-edgevision.service`（历史单进程壳）enabled 时告警 —— 两个都起会抢 RTSP 8550 |
| 启动方式 | 不用 `systemctl restart evs.target`，而是逐个 `restart` 三个 service：target 的 restart 语义会连坐，逐个更可控 |

## 3. 单元里每一行为什么这么写

| 配置 | 作用 | 写错的后果 |
| --- | --- | --- |
| `LimitCORE=infinity` | 抬高**服务的 hard limit** | 不写 → hard 通常为 0 → **一个 core 都没有**（`/etc/security/limits.d` 对 systemd 无效） |
| `StartLimitIntervalSec=` / `StartLimitBurst=` **放在 `[Unit]`** | 限制重启风暴 | 放 `[Service]` 会被 systemd 忽略（249 实测：`Unknown key name 'StartLimitIntervalSec' in section 'Service'`） |
| `WorkingDirectory=<root>/out` | 与 `make run-*` 一致 | 配置里的相对路径会按别处解析，启动即失败或读错素材 |
| `TimeoutStopSec=20`（media） | 留时间走 `stop_all → FreeMem` 有序停止 | 太短 → 被 SIGKILL，内存池/编码器未释放（下次启动可能设备忙） |
| `Restart=on-failure`（media/upgrade） vs `always`（comm） | 媒体与升级：自己不退出，崩了才拉起；通信：断线重连是本职 | 全用 `always` → 正常退出也被拉起；全用 `on-failure` → 通信被 kill 后不自愈 |
| `Wants=`（不是 `Requires=`） | 一个进程起不来不阻断另外两个 | 用 `Requires=` → 媒体挂了通信也停，远端直接失联 |
| `ProtectSystem=full`（upgrade） | 首期沙箱：`/usr` 只读 | 直接上 `strict` + `systemctl` 可能在部分 systemd 版本上 D-Bus `Access denied`（见 unit 尾部注释） |

## 4. 三进程拓扑

```text
evs.target (WantedBy=multi-user.target)
 ├─ evs-media.service     evs_media   config/node.json
 ├─ evs-comm.service      evs_comm    config/comm.json      （broker 是独立进程，不在本 unit 里）
 └─ evs-upgrade.service   evs_upgrade config/upgrade.json   （After=evs-media.service）
       ↑ OnFailure（P4 打开）
 evs-rollback.service      evs_upgrade --rollback --reason=start-limit
```

## 5. 为什么不用 `rc.local` / `crontab @reboot`

| 方式 | core | 重启策略 | 日志 | cwd | 依赖顺序 |
| --- | --- | --- | --- | --- | --- |
| systemd unit | ✅ `LimitCORE=infinity` | ✅ `Restart=` | ✅ `journalctl` | ✅ `WorkingDirectory=` | ✅ `After=/Wants=` |
| `rc.local` | ❌ hard limit 默认 0 | ❌ | ❌ 零散 | ❌ 不可控 | ❌ 自己 sleep |
| `crontab @reboot` | ❌ 同上 | ❌ | ❌ | ❌ | ❌ 自己 sleep |

## 6. 目录选择的理由

unit 放 `scripts/deploy/` 而不是仓库根，是为了让「启动（`run/`）/ 采集（`coredump/`）/ 部署（`deploy/`）/ 回归（`tests/`）」职责分开：部署时只需要拷贝/引用 `deploy/`，不影响其他脚本。

## 7. 现状与待办（诚实清单）

| 项 | 状态 |
| --- | --- |
| 5 个 unit + `install.sh` | ✅ 已写；`--dry-run` 本机实测 5/5 渲染、`systemd-analyze verify` 0 条项目相关告警（TESTLOG §5.26） |
| 实装到本机 | ❌ **未执行**（需 sudo 改 `/etc/systemd/system`，属运维动作） |
| `OnFailure=evs-rollback.service` | ⏸ 在 `evs-media.service` 里**注释**：骨架期 `--rollback` 必以 `E_NOTIMPL` 失败，P4 打开 |
| `ExecStartPost=… --on-media-started` | ⏸ 未接线（`evs_upgrade` 子命令已存在），与 `boot_count` 一起做 |
| broker（mosquitto）常驻 | ❌ 没有 unit：目前靠手工起或外部服务；需要时在 `evs.target` 的 `Wants=` 里加 `mosquitto.service`（详见 `docs/design/DESIGN_IPC.md` §4、§10 第 5 条） |
| `ProtectSystem=strict` | ⏸ 待 P3 验证 `systemctl` 通过 D-Bus 能调通再换（现在 `full`） |


