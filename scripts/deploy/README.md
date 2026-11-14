# scripts/deploy/ —— 部署产物

> 目前只有一个文件：[`evs-edgevision.service`](evs-edgevision.service)（systemd unit）。
> 完整部署步骤与实测输出在 [../../docs/ops/COMMANDS.md §12](../../docs/ops/COMMANDS.md)。

## 1. 安装 / 卸载 / 自检

```bash
# 安装（改路径后先编辑 unit 里的 /opt/EdgeVisionService 与 WorkingDirectory）
sudo cp scripts/deploy/evs-edgevision.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now evs-edgevision     # 开机自启 + 立刻启动

# 自检（三行都必须符合预期）
systemctl status evs-edgevision --no-pager
systemctl show -p LimitCORE evs-edgevision      # 必须 LimitCORE=infinity
cat /proc/$(systemctl show -p MainPID --value evs-edgevision)/limits | grep -i core

# 日志
journalctl -u evs-edgevision -f                 # 启动日志里的 [crash] 行确认采集已装

# 卸载
sudo systemctl disable --now evs-edgevision && sudo rm /etc/systemd/system/evs-edgevision.service && sudo systemctl daemon-reload
```

## 2. unit 里每一行为什么这么写

| 配置 | 作用 | 写错的后果 |
| --- | --- | --- |
| `LimitCORE=infinity` | 抬高**服务的 hard limit** | 不写 → hard 通常为 0，`setrlimit` 抬不上去 → **一个 core 都没有**。这是整个 unit 里最关键的一行 |
| `ExecStart=…/scripts/run/out/start.sh` | 前台执行启动脚本（内部 `exec`） | 直接跑二进制会漏 `LD_LIBRARY_PATH` / `EVS_CRASH_DIR` |
| `WorkingDirectory=<仓库根>` | 直接跑二进制时决定 core 相对名的落点 | 写成 `/` 或不可写目录 → `core_pattern=core` 时 core 静默丢失 |
| `Restart=on-failure` + `RestartSec=3` | 崩溃后自动拉起 | 不写 → 崩溃即永久停机（调试时可以临时改成 `Restart=no` 便于取证） |
| 不写 `Environment=LD_LIBRARY_PATH` | 库路径统一由 `start.sh` 管 | 两处都写 → 改一处忘另一处，出现"命令行能跑、服务起不来" |

## 3. 为什么不用 `rc.local` / `crontab @reboot`

| 方式 | core | 重启策略 | 日志 | cwd |
| --- | --- | --- | --- | --- |
| systemd unit | ✅ `LimitCORE=infinity` | ✅ `Restart=` | ✅ `journalctl` | ✅ `WorkingDirectory=` |
| `rc.local` | ❌ hard limit 默认 0 | ❌ | ❌ 只在 `/var/log` 零散 | ❌ 不可控 |
| `crontab @reboot` | ❌ 同上 | ❌ | ❌ | ❌ |

**`/etc/security/limits.d` 对 systemd 服务同样无效**：它只影响 PAM 登录会话，服务继承的是 PID1 的 limits。

## 4. 目录选择的理由

unit 放 `scripts/deploy/` 而不是仓库根，是为了让「启动（`run/`）/ 采集（`coredump/`）/ 部署（`deploy/`）/ 回归（`tests/`）」职责分开：部署时只需要拷贝/引用 `deploy/` + `run/`，不影响其他脚本。
