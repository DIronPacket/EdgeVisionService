# docs/ops/ —— 运维与排障

> 这个目录只放**动手时要照做的东西**：敲哪条命令、按什么顺序排查、怎么判断"成功了"。
> 不讲设计取舍（去 [../design/](../design/)），不讲历史（去 [../history/](../history/)）。

## 1. 两篇文档的分工

| 文档 | 回答的问题 | 什么时候翻 |
| --- | --- | --- |
| [COMMANDS.md](COMMANDS.md) | **敲哪条命令**：构建 / 运行 / 看内存 / 写临时探针 / 进程端口线程 / git / 崩溃 / 部署自启 | 只要需要"动手"，先来这里复制命令 |
| [CRASH_DUMP.md](CRASH_DUMP.md) | **崩溃为什么没留下 core、core 名字怎么来的、怎么在 core 里看变量** | 进程异常退出、只拿到一段报告、`(core dumped)` 但找不到文件 |

关系：`CRASH_DUMP.md` 是**原理 + 判据**，`COMMANDS.md §11` 是它的**命令实现**（体检 / 开 core / 符号化 / 解析 / 归档 / gcore）。排障时两篇交替看：原理篇告诉你"下一段该查什么"，命令篇给你那条命令。

## 2. COMMANDS.md 的章节地图

| § | 内容 | 典型用途 |
| --- | --- | --- |
| 0 | 三条通用套路（输出落盘再读 / 超时处理 / 判据优先） | 先读，后面所有命令都靠它读结果 |
| 1 | 构建（`make` / 换目录 / 换 BuildType / 增量） | 改完代码验证编译 |
| 2 | 运行与优雅退出（含 §2.5 两个 `start.sh`） | 冒烟、抓现场 |
| 3 | 看内存（进程级 / 堆级 / 泄漏对比） | 判断是否漏池块 |
| 4 | 临时探针（不污染仓库） | 验证一个猜想，不写进源码 |
| 5 | 进程 / 端口 / 线程 / 设备 | 「进程还在吗、端口被谁占、几路在跑」 |
| 6 | 自检脚本（无硬件依赖） | 没板子时验证逻辑 |
| 7 | Git 提交 / 校验 / 推送 | 交作业 |
| 8 | 文档一致性体检 | 移动/改名文档后必跑 |
| 9 | 踩过的坑速查表 | 命令报错先查表 |
| 10 | 一屏自检 | 交付前 30 秒过一遍 |
| 11 | 崩溃现场：体检 / 开 core / 符号化 / 解析 / 归档 / gcore | 见下 §3 |
| 12 | 部署 / 开机自启（systemd） | 上线 |

## 3. 崩溃排障的正确顺序（不要跳步）

```text
①  scripts/coredump/check_coredump_env.sh      # 13 项体检：先确认"这个环境能不能抓到"
②  scripts/run/out/start.sh                    # 用带 ulimit / env 的方式启动（不要裸跑）
③  复现崩溃 / kill -QUIT <pid>                 # 卡死也能取现场（SIGQUIT 已被 handler 接管）
④  scripts/coredump/symbolize_report.sh <报告> # 先看人类可读报告（有业务 tag、信号、栈）
⑤  scripts/coredump/collect_core.sh <core>     # 归档并自动带上「函数@文件:行」
⑥  scripts/coredump/analyze_core.sh <core> <exe>   # gdb：看局部变量（顺序必须是 file → set solib-search-path → core-file）
```

**只看报告不看 core 的代价**：报告能告诉你「哪个函数崩的」，只有 core 能告诉你「那个变量当时是多少」。

## 4. 三条容易踩的运维约定

1. **不要用 `rc.local` / `crontab @reboot` 开机自启**：拿不到 `LimitCORE`，core 永远抓不到；cwd 不可控。用 systemd（`COMMANDS.md §12`）。
2. **`/etc/security/limits.d` 对 systemd 服务无效**：服务的 hard limit 继承 PID1（通常 0），`setrlimit` 抬不上去 —— 只有 unit 里的 `LimitCORE=infinity` 有效。
3. **抓现场要用 `out/` 版**：`build/` 是 2026-11-12 的旧产物，不含崩溃采集代码（启动日志里没有 `[crash]` 行）。
