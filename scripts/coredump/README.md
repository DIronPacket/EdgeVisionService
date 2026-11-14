# scripts/coredump/ —— 崩溃现场采集与解析

> 目标：进程死了之后，既能回答「**哪个程序、哪个函数**」（报告），也能回答「**当时变量是多少**」（core + gdb）。
> 原理与判据在 [../../docs/ops/CRASH_DUMP.md](../../docs/ops/CRASH_DUMP.md)，命令清单在 [../../docs/ops/COMMANDS.md §11](../../docs/ops/COMMANDS.md)。

## 1. 六个脚本的关系（按执行顺序）

```text
              ┌──────────────────────────── 事前 ────────────────────────────┐
              │ enable_coredump.sh   (root，一次性：core_pattern + ulimit + 目录) │
              │ check_coredump_env.sh(只读体检，13 项 PASS/WARN/FAIL)             │
              └───────────────────────────────────────────────────────────────┘
                                        ↓ 崩溃发生（或 kill -QUIT 取卡死现场）
              ┌──────────────────────────── 事后 ────────────────────────────┐
              │ symbolize_report.sh  → 报告里的地址变成「函数@文件:行」          │
              │ collect_core.sh      → 归档：core 硬链 + meta + 崩溃栈 + 全线程栈 │
              │                        + .so 列表 + 同 pid 的报告；目录名带函数    │
              │ analyze_core.sh      → gdb 看局部变量（file → solib → core-file）  │
              │ gcore_now.sh         → 不杀进程，对活进程抓 snap 并自动调 analyze  │
              └───────────────────────────────────────────────────────────────┘
```

`gcore_now.sh` 内部调用同目录的 `analyze_core.sh`（`$(dirname "$0")/analyze_core.sh`），因此**两个脚本必须留在同一目录**。

## 2. 每个脚本的输入 / 输出

| 脚本 | 输入 | 输出 | 关键参数 |
| --- | --- | --- | --- |
| `check_coredump_env.sh` | 无 | 终端 13 项报告；rc=1 表示有 FAIL | `EVS_CORE_DIR`（默认 `/var/lib/evs/coredump`） |
| `enable_coredump.sh` | 无 | 改 `/proc/sys/kernel/core_pattern`、写 `limits.d`、建目录 | `EVS_CORE_DIR` |
| `collect_core.sh` | `<core> [exe] [-o 归档根] [-s 信号]` | 一个归档目录（见 §3） | exe 省略时自动推断 |
| `analyze_core.sh` | `<core> [exe] [输出 txt]` | 终端 / txt：崩溃线程栈 + 全线程栈 | 顺序固定，乱序会全是 `??` |
| `symbolize_report.sh` | `<报告 .log>` | 同内容 + 符号化后的栈 | `EVS_MODROOT`（部署路径） |
| `gcore_now.sh` | `<pid 或进程名关键字> [输出目录]` | `crash/live_*.core` + 解析结果 | 需要 ptrace 权限 |

## 3. `collect_core.sh` 的归档结构

```text
core.<app>.<pid>.<epoch>.sig<N>.<函数>@<文件>:<行>/
├── core                        硬链接（link count=2，不占双倍空间）
├── meta.txt                    exe 路径 + build-id + pid/epoch/信号 + core_pattern + 内核 + ulimit
├── crash_thread_bt.txt         崩溃线程栈（tgkill 保证是真实现场）
├── backtrace_all_threads.txt   全线程栈
├── sharedlibs.txt              .so 加载情况（排查 ?? 帧）
└── report.log                  同 pid 的 crash_*.log（存在时自动配对）
```

**为什么函数名/文件名在目录名里而不是 core 名里**：`core_pattern` 由内核展开，内核手里只有 `task_struct`（程序名 / pid / 信号），拿不到用户态符号 —— 所以只能事后用 gdb 解析后归档改名。这条结论与本目录的设计理由见 [CRASH_DUMP.md §2](../../docs/ops/CRASH_DUMP.md)。

## 4. 环境变量

| 变量 | 谁用 | 含义 |
| --- | --- | --- |
| `EVS_CORE_DIR` | `check` / `enable` | core 落盘目录（默认 `/var/lib/evs/coredump`） |
| `EVS_CRASH_DIR` | 程序（`crash_handler`）、`collect_core.sh` 找报告 | 崩溃报告目录；`start.sh` 默认设为 `<仓库根>/crash` |
| `EVS_SOLIB` | `collect_core.sh` | 覆盖 `.so` 搜索路径（默认 `<exe>/../lib`） |
| `EVS_MODROOT` | `symbolize_report.sh` | 部署根目录（二进制与 `.so` 的实际位置） |

## 5. 依赖与自检

- 依赖：`bash`、`gdb`、`binutils`（`addr2line` / `readelf`）、`coreutils`。缺 `gdb` 时 `collect_core.sh` 会在第一步明确报错退出，不会产出半成品目录。
- 自检：`for s in scripts/coredump/*.sh; do bash -n "$s" || echo "FAIL $s"; done`（应无输出）。
- 实跑记录：[../../docs/history/TESTLOG.md §5.19–§5.20](../../docs/history/TESTLOG.md)。
