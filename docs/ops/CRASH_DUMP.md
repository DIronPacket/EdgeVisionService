# 崩溃现场采集：从信号到 core 的完整流程

> 一句话：**内核不知道「哪个函数崩的」，用户态不知道「变量现在是多少」**。所以要两条产物：
> 人可读报告（用户态、有符号）＋ 内核 core（内核态、有内存）。
>
> 本文只讲"怎么抓、名字怎么来、为什么抓不到"，命令清单见 [COMMANDS.md §11](COMMANDS.md)，
> 实测证据见 [TESTLOG.md §5.19](../history/TESTLOG.md)，踩坑原理见 [BUG_CASES.md](../history/BUG_CASES.md) B12/B13。

---

## 0. 两条产物

| 产物 | 谁写 | 里面有什么 | 缺什么 |
| --- | --- | --- | --- |
| `crash/crash_<app>_<pid>_<epoch>_sig<N>.log` | 进程自己（信号 handler） | 程序名 / exe 绝对路径 / pid / tid / UTC 时间 / 信号名 + 编号 / `si_code` / 故障地址 / pc·sp·lr·fp / 调用栈 / 业务 tag | 变量值、其它线程 |
| `core.<...>`（或 `core`） | **内核** `do_coredump()` | 全部内存段 + 寄存器 + 每个线程的栈（ELF note `NT_PRSTATUS`） | 符号、函数名、行号、变量名 |

两者合起来才完整：报告告诉你「`MppEncoder::Encode` 里崩的」，gdb 打开 core 才知道「`buf` 是 `0x0`、`size` 是 1920」。

---

## 1. 完整流程（7 步）

### 1.1 时序

```
① 某线程执行一条访存/除零/非法指令
        │  CPU 产生同步异常（不是信号！）
        ▼
② 内核异常处理：do_page_fault / do_undefinstr / ...
   → force_sig_fault(SIGSEGV, si_code, si_addr)  // 只把信号挂到该线程 pending
        │
        ▼
③ 内核返回用户态前的最后一步：get_signal()
   → 查该线程的 sigaction[SIGSEGV]
        ├─ SIG_DFL，且默认动作含 core ──────────────┐
        ├─ SIG_IGN ────────────────────────────────┤→ 不落 core，见 §3
        └─ 有 handler ──→ ④                        │
                                                    │
④ 进程 handler 在备用栈（sigaltstack）上运行        │
   - open() 报告文件 → write() 文本（不用 printf）    │
   - backtrace_symbols_fd() 写栈                     │
   - sigprocmask(SIG_UNBLOCK, {该信号})  ← 关键      │
   - tgkill(getpid(), gettid(), 该信号)  ← 重抛      │
        │                                           │
        ▼                                           │
⑤ 该信号已被 SA_RESETHAND 恢复成 SIG_DFL，          │
   内核第二次投递 → 走默认动作 ────────────────────┘
        │
        ▼
⑥ 内核 do_coredump()（这一段全部在内核里）
   检查链：RLIMIT_CORE > 0 → core_pattern 能解析
        → task 可 dump（dumpable / suid_dumpable）
        → 目标路径可写 / 空间够
        → 按 coredump_filter 选择内存段
   写 ELF core（多线程时每个线程一个 NT_PRSTATUS）
        │
        ▼
⑦ 进程带着「core dumped」状态死掉，父进程看到 WTERMSIG(sig) | WCOREDUMP
   → shell 打印 `Segmentation fault (core dumped)`
```

**记住③→④→⑤这一步是整条链的关键**：内核第一次投递信号时**只调 handler，不落 core**；core 是 handler 自己"重抛"之后由内核按默认动作产生的。所以 handler 里少一步（解除屏蔽）就一个 core 都没有 —— 这就是 BUG_CASES B12。

### 1.2 每一步的代码 / 位置

| 步 | 谁在做 | 本仓库位置 |
| --- | --- | --- |
| ①② | 内核（架构相关：`arch/arm64/mm/fault.c`） | 不可改；现象是 `si_code`（1=SEGV_MAPERR 空指针/未映射，2=SEGV_ACCERR 权限） |
| ③ | 内核 `kernel/signal.c: get_signal()` | 决定因素：`sigaction()` 是否装、`SIG_DFL` 的默认动作表 `sig_kernel_coredump()` |
| ④ | 进程（用户态，async-signal-safe） | `src/common/crash/crash_handler.cpp: OnFatalSignal()`（约 212 行起） |
| ④ 最后一跳 | 进程 → 内核 | 同文件约 310–326 行：`sigprocmask(SIG_UNBLOCK)` → `tgkill` |
| ⑥ | 内核 `fs/coredump.c: do_coredump()` | 受 `RLIMIT_CORE` / `core_pattern` / `core_pipe_limit` / `coredump_filter` / `fs.suid_dumpable` / PR_SET_DUMPABLE 控制 |
| ⑦ | 内核 + shell | `WIFSIGNALED` / `WCOREDUMP`；`bash` 打印 `(core dumped)` |

### 1.3 为什么必须 `tgkill` 到崩溃线程，而不是 `kill(getpid())`

`kill(getpid(), sig)` 会把信号投给**任意一个**线程（通常主线程）；那个线程没有坏栈、没有坏现场，内核记录的 `NT_PRSTATUS`（寄存器 + 栈指针）就是"主线程在 `pause()`"，core 里看不到崩溃点和崩溃线程的局部变量。

`tgkill(pid, tid, sig)`（`syscall(SYS_tgkill, getpid(), gettid(), sig)`）精确投给**执行过 handler 的那个线程**，于是 core 的"当前线程"就是崩溃线程，`bt`、`info locals`、`info args` 一次到位。这是"能在 core 里看变量"的前提。

---

## 2. core 的名字：能带什么、不能带什么

### 2.1 `core_pattern` 的全部格式符（内核 6.1）

`/proc/sys/kernel/core_pattern` 是**内核**在 `do_coredump()` 里展开的模板，可用字段只有内核当时知道的信息：

| 占位符 | 展开成 | 例（本程序） | 备注 |
| --- | --- | --- | --- |
| `%e` | `current->comm`（可执行名） | `EdgeVisionServic` | **硬上限 16 字节（`TASK_COMM_LEN`=16），第 17 个字符起被截断** |
| `%p` | 进程 pid（tgid） | `75879` | |
| `%P` | 初始命名空间里的 pid | 同 `%p`（无容器时） | 容器里才不同 |
| `%i` | 触发崩溃的线程 tid | `75883` | 想知道"哪个线程崩的"就用它 |
| `%s` | 触发信号编号 | `11` / `3` / `6` | 配合报告里的 `signal = SIGSEGV [11]` |
| `%t` | 崩溃时刻 epoch 秒 | `1794659287` | 防覆盖的关键 |
| `%u` / `%g` | uid / gid | `1000` | |
| `%h` | hostname | `orangepi5pro` | |
| `%c` | RLIMIT_CORE 软限制 | `18446744073709551615` | 排查"为什么没 core"时有用 |
| `%d` | dump 模式（PR_SET_DUMPABLE 的值） | `1` | 同上 |
| `%E` | exe 绝对路径，`/` 换成 `!` | `!mnt!nvme!…!out!bin!EdgeVisionService` | 仅较新内核支持，老内核会原样输出 `%E`，**不要依赖** |
| `%%` | 字面 `%` | `%` | |

三条从这张表直接推出的结论：

1. **函数名、源文件名、行号永远不可能出现在 core 文件名里** —— 内核手上只有 `task_struct`，用户态符号在 ELF 的 `.symtab/.dynsym` 里，要 libbfd/addr2line/gdb 才能解析。想要"名字里带函数"，只能在**事后**补（§2.3 方案 B）。
2. **`%e` 会截断**：`EdgeVisionService` 是 17 个字符，实际落盘是 `core.EdgeVisionServic.<pid>.<epoch>`。所以不要把 app 名当作唯一标识，pid + epoch 才可靠。
3. **不写 `%p` 时**，若 `kernel.core_uses_pid=1`（本机就是 1），内核会在末尾自动补 `.<pid>`。本机 `core_pattern=core` → 实际产物 `core.75879`，这就是 `out/core.75879` 的由来。

### 2.2 只用 `core`（本机当前配置）会踩的三个坑

| 坑 | 现象 | 后果 |
| --- | --- | --- |
| 相对路径 | core 落在**进程 cwd**，不是固定目录 | 前台跑在 `out/` 下没问题；systemd 托管时 cwd 可能是 `/`，服务用户无权写 → **静默不落盘** |
| 名字里没有程序/时间 | 只有 `core.<pid>` | pid 复用（重启后）会**覆盖**上一次的证据；多实例互删 |
| 没有目录规划 | 分散在 cwd | 353 MB 的 core 混在构建目录里，`make clean` 一起没了 |

推荐写法（绝对路径 + 程序 + pid + epoch + 信号）：

```
/var/lib/evs/coredump/core.%e.%p.%t.sig%s
# 例：core.EdgeVisionServic.75879.1794659287.sig11
```

`scripts/coredump/enable_coredump.sh` 就按这个写（含 `sysctl.d` 持久化）。

### 2.3 想让名字里带「函数名 / 文件名 / 行号」：三种方案

| 方案 | 做法 | 名字能带什么 | 代价 |
| --- | --- | --- | --- |
| **A 现状**（零成本） | core 用 `%e.%p.%t.sig%s`；报告里另有一个 `crash_…log` | core 名只有程序/pid/时间/信号；函数名在**报告正文**和符号化结果里 | 两份文件要人工对上（靠 pid + epoch 对） |
| **B 事后归档**（本仓库新增，推荐） | `scripts/coredump/collect_core.sh <core> <exe>`：gdb 读出崩溃帧 → 建归档目录 `core.<app>.<pid>.<epoch>.sig<N>.<func>@<file>:<line>/`，里面放 core 硬链 + `meta.txt` + `backtrace.txt` + 配套报告 | **全部**：程序、pid、时间、信号、函数名、源文件、行号 | 事后跑一条命令（~1 s/100 MB） |
| **C 内核管道** | `core_pattern = \|/usr/local/bin/core_pipe %e %p %t %s`，wrapper 从 stdin 收 core，再 gdb 解析改名 | 同 B，但**自动**、即时 | 需 root 改 pattern；collector 运行在崩溃回收路径上（受 `core_pipe_limit` 约束），gdb 崩溃时会丢现场，调试期风险大，**不建议在生产先上** |

方案 B 的归档名长这样（一眼看出崩在哪）：

```
/var/lib/evs/coredump/archive/
  core.EdgeVisionServic.75879.1794659287.sig3.MppEncoder_Encode@mpp_encoder.cpp:412/
      core            ← 硬链接（同盘不占双倍空间）
      meta.txt        ← exe 路径 + build-id + 命令行 + cwd + ulimit + core_pattern
      backtrace.txt   ← 全线程栈（thread apply all bt）
      report.log      ← 同 pid+epoch 的 crash_*.log（自动配对）
```

### 2.4 本仓库的落地组合

1. 落盘位置由内核定：`enable_coredump.sh` → `/var/lib/evs/coredump/core.%e.%p.%t.sig%s`
2. 人可读上下文由进程定：`EVS_CRASH_DIR`（默认 `./crash`）→ `crash_<app>_<pid>_<epoch>_sig<N>.log`
3. 两者用 **pid + epoch** 天然配对（报告名里就是这两个字段），`collect_core.sh` 自动完成配对与改名。

---

## 3. 为什么「抓不到」：按链路逐段排查

整条链是 `信号产生 → handler 装上 → handler 跑通 → 重抛 → RLIMIT_CORE → dumpable → core_pattern 能落盘 → 事后能解析`，
**任何一段断掉都是「没有 core」**，而内核大多**不报错**。下面按段列。

### 3.1 段①：这个信号根本不会产生 core

| 情况 | 默认动作 | 有报告？ | 有 core？ |
| --- | --- | --- | --- |
| `SIGKILL`（含 **OOM killer**、systemd 超时强杀、`kill -9`） | 终止 | ✗（handler 装不上，内核不允许捕获） | **✗ SIGKILL 默认动作不含 core** |
| `SIGTERM` / `SIGINT`（`Ctrl+C`） | 终止 | ✗（默认没装 handler；本仓库只额外抓 SIGQUIT） | ✗ 默认动作不含 core |
| `SIGQUIT`（`Ctrl+\`、`kill -QUIT`） | 终止 + core | ✓（本仓库 `trap_sigquit=true`） | ✓ |
| `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` | 终止 + core | ✓ | ✓ |
| `SIGABRT`（`abort()`、未捕获异常） | 终止 + core | ✓（`std::set_terminate` 先打异常类型） | ✓ |
| `return` / `exit()` / `_exit()` / `std::exit` | 正常退出 | ✗ | ✗（没有信号） |

> 所以「进程突然没了、什么都没留下」先查 `dmesg | grep -i 'killed process'`（OOM）和 `journalctl -u <svc> | tail`（systemd 强杀），这两种**永远不会有 core**，只能加 `MemoryMax` / 调 `TimeoutStopSec` 防。

### 3.2 段②：handler 没装上 / 装上但跑不通

| 原因 | 现象 | 对策 |
| --- | --- | --- |
| 崩溃发生在 `InstallCrashHandler()` 之前（静态初始化、`main` 里的早期代码） | 无报告；core 仍按默认动作产生 | handler 必须放在 `main` 第一行（本仓库已如此） |
| 崩溃在**工作线程**且是**栈溢出** | handler 用坏栈 → 二次崩；报告可能没有 | `sigaltstack` 按线程生效，需在每个线程入口装（本仓库目前只覆盖主线程，见 HANDOFF §1.10 已知限制） |
| handler 内部二次崩（堆已毁、`backtrace` 拿不到内存） | `g_in_handler` 递归保护 → 直接 `tgkill` + `_exit` | core 仍有；报告可能缺半截，属预期 |
| 信号被 `signal(SIGSEGV, SIG_IGN)` 之类覆盖 | 内核按 `SIG_IGN` **静默丢弃** | 全仓搜 `signal(` / `sigaction(`，别覆盖 handler 装的 5 个信号 |

### 3.3 段③：`RLIMIT_CORE` 是 0（**本机默认就是 0**）

```
$ ulimit -c
0            ← 软限制 0：内核一个 core 都不写
$ ulimit -Hc
unlimited    ← 硬限制无限：进程自己可以抬
```

- 本仓库在 `InstallCrashHandler()` 里做 `setrlimit(RLIMIT_CORE, hard)`，所以**前台跑能出 core**（已实测）。
- 但 `setrlimit` 只能抬到 **hard**。若 hard 也是 0（systemd 服务常见，`LimitCORE` 未写时继承 PID1 的 0），进程**无权**抬高 → **一个 core 都不会有**。
- 修复：单元里 `LimitCORE=infinity`，或 root 改 `/etc/security/limits.d/`，或启动脚本里 `ulimit -c unlimited`（`start.sh` 已做）。
- 自检：`cat /proc/<pid>/limits | grep 'Max core file size'`。

### 3.4 段④：`RLIMIT_FSIZE` 把 core 截断

`ulimit -f`（文件大小上限）非 `unlimited` 时，写 core 到一半触发 `SIGXFSZ`/`EFBIG`，**内核会删掉半成品**。

```
$ ulimit -f
unlimited    ← 正常
```

### 3.5 段⑤：进程被标成「不可 dump」

| 原因 | 说明 |
| --- | --- |
| `PR_SET_DUMPABLE=0` | 内核直接跳过 core。**`setuid()`/`setgid()`/执行 suid 程序后内核会自动把它清成 0** |
| `fs.suid_dumpable=0`（本机 0） | suid/sgid 进程一律不写 core（本项目不是 suid，无影响） |
| 父进程/ptrace 残留 | 极少见 |

本仓库在 install 时 `prctl(PR_SET_DUMPABLE, 1)`；但如果**崩溃前**又调了 `setuid`，dumpable 会被内核重置，需要在那之后再调一次。

### 3.6 段⑥：`core_pattern` 解析 / 落盘失败（最常见的「静默丢失」）

| 原因 | 现象 | 检查 |
| --- | --- | --- |
| 相对路径 `core` | 落在**进程 cwd** | `ls -l /proc/<pid>/cwd`；systemd 下常是 `/` |
| 目标目录不存在 / 不可写 / 只读文件系统 | 内核**静默失败**，只有 `dmesg` 可能有一点 | `touch <dir>/.w` |
| 磁盘/配额不足（本项目 core 可到 300 MB+） | 写到一半失败，core 被删 | `df -h <dir>` |
| 落点是 **tmpfs**（本机 `/tmp` 是 7.8 G tmpfs） | core 直接吃**内存**，大 core 可能 OOM | `findmnt -no FSTYPE,TARGET <dir>` |
| `core_pattern` 以 `\|` 开头（apport / systemd-coredump 接管） | 文件不在 pattern 路径，在 `/var/lib/systemd/coredump/` 或 apport 目录 | `cat /proc/sys/kernel/core_pattern`、`command -v coredumpctl` |
| 容器内 | 宿主 `core_pattern` 决定落点，容器内看不到；`/proc/sys` 通常只读 | 在宿主上找 |
| 同名覆盖 | 多个实例/多次崩都叫 `core` | 用 `%e.%p.%t` |

### 3.7 段⑦：core 有了但「看起来什么都没有」（解析问题，不是抓不到）

| 现象 | 真因 | 对策 |
| --- | --- | --- |
| `bt` 全是 `??`、`No such file or directory` | `.so` 没加载（gdb 在 `-ex 'set solib-search-path'` 之前就吃完了 core） | 用 `scripts/coredump/analyze_core.sh`（显式 `file` + `core-file`，实测 missing=0） |
| `No symbol table` / 栈错乱 | core 与二进制**不是同一份**（重新编译过） | 归档时同时存 exe + `.so` 的 build-id（`collect_core.sh` 的 `meta.txt`） |
| `info locals` 一堆 `<optimized out>` | `RelWithDebInfo` 是 `-O2 -g` | 用 `Debug` 复现：`make BUILD=out-dbg CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=Debug"` |
| `disassemble` 看不到代码 | `coredump_filter=0x33` 不含私有文件映射 | 想看代码段：`echo 0x3f > /proc/<pid>/coredump_filter`（需在崩溃前） |
| 只有主线程栈，工作线程全 `pause()` | handler 用了 `kill()` 而不是 `tgkill()` | 本仓库已用 `tgkill(getpid(), gettid(), sig)` |

### 3.8 本机（orangepi5pro / 6.1.99-rk3588）2026-11-14 实测结论

| 检查项 | 实测值 | 判定 | 影响 |
| --- | --- | --- | --- |
| `kernel.core_pattern` | `core` | ⚠️ WARN | 相对路径 → 落进程 cwd；systemd 下可能不可写 |
| `core_uses_pid` | `1` | ✅ PASS | 自动补 `.pid`，所以是 `core.75879` |
| `ulimit -c` 软/硬 | `0` / `unlimited` | ⚠️ WARN | 裸跑默认不落 core；handler 会抬，但**依赖 hard>0** |
| `ulimit -f` | `unlimited` | ✅ PASS | 不截断 |
| `fs.suid_dumpable` | `0` | ✅ PASS | 非 suid 进程无影响 |
| `kernel.core_pipe_limit` | `0` | ✅ PASS | pattern 非管道，不涉及 |
| `coredump_filter` | `0x33` | ⚠️ WARN | 不含代码段（变量仍可看） |
| AppArmor | 关闭（`N`） | ✅ PASS | 不拦 core |
| `coredumpctl` | 未安装 | ✅ PASS | 说明没有 systemd-coredump 接管 |
| `/tmp` 文件系统 | tmpfs 7.8 G | ⚠️ WARN | core 落这里吃内存 |
| 落盘盘剩余 | `/mnt/nvme` 360 G 可用 | ✅ PASS | 353 MB core 无压力 |
| `build/` 目录 | 存在（11-12 旧产物） | ⚠️ WARN | 跑 `build/` 版前先确认里面有新二进制 |

结论：**当前前台运行能抓到 core（已实测）**；风险集中在"换 systemd 托管后 hard limit=0 + cwd 不可写"，所以上线前必须执行 `enable_coredump.sh` 或用带 `LimitCORE=infinity` 的 unit。

---

## 4. 脚本清单与典型流程

| 脚本 | 作用 | 需要 root | 实测状态 |
| --- | --- | --- | --- |
| `scripts/coredump/check_coredump_env.sh` | 体检本机能不能抓到 core（逐项 PASS/WARN/FAIL + 修复命令） | 否（只读） | 已实跑 |
| `scripts/coredump/enable_coredump.sh` | 改 `core_pattern`（绝对路径 + `%e.%p.%t.sig%s`）+ `sysctl.d` / `limits.d` 持久化 | **是** | 未实跑（改 `/proc/sys`） |
| `scripts/coredump/analyze_core.sh` | gdb 批量解析 core：全线程栈 + 寄存器 + 参数/局部 + `.so` 对齐 | 否 | 已实跑 |
| `scripts/coredump/symbolize_report.sh` | 报告里的 `模块(符号+0x偏移)` → `函数 (文件:行号)` | 否 | 已实跑 |
| `scripts/coredump/collect_core.sh` | 把 core 归档重命名成 `core.<app>.<pid>.<epoch>.sig<N>.<函数>@<文件>:<行>` + `meta.txt` | 否 | 已实跑 |
| `scripts/coredump/gcore_now.sh` | 活进程现场（`/proc` 快照 + `gcore`），用于卡死 | 否（需 ptrace 权限） | 未实跑 |
| `scripts/run/out/start.sh` | 启动 `out/bin/EdgeVisionService`（默认构建目录） | 否 | 待实跑 |
| `scripts/run/build/start.sh` | 启动 `build/bin/EdgeVisionService`（历史构建目录） | 否 | 待实跑 |

**典型流程（四条命令）**

```bash
# 0) 先确认能抓（只读，不花时间）
scripts/coredump/check_coredump_env.sh

# 1) 生产机上一次性放行 core（root，落 /var/lib/evs/coredump）
sudo scripts/coredump/enable_coredump.sh

# 2) 启动（脚本已 ulimit -c unlimited + 设 EVS_CRASH_DIR）
scripts/run/out/start.sh                       # 默认 config/node.json
scripts/run/out/start.sh config/other.json     # 指定配置

# 3) 崩了以后：先看报告（有函数名/行号），再进 core（有变量）
scripts/coredump/symbolize_report.sh crash/crash_EdgeVisionService_*.log
scripts/coredump/analyze_core.sh /var/lib/evs/coredump/core.* out/bin/EdgeVisionService
scripts/coredump/collect_core.sh /var/lib/evs/coredump/core.* out/bin/EdgeVisionService  # 归档成带函数名的目录
```

## 5. 开机自启

**要设的地方只有一处：systemd unit**。不要用 `rc.local` / `crontab @reboot`：它们不设 `LimitCORE`（core 直接抓不到）、cwd 不可控（`core` 落到 `/`）、没有重启策略、日志也不进 journal。

关键三行（完整步骤见 [COMMANDS.md §12](COMMANDS.md)）：

```ini
[Service]
LimitCORE=infinity                                        # ← 不写这行，服务模式下 core 永远抓不到
WorkingDirectory=/mnt/nvme/workspace/EdgeVisionService     # ← 直接跑二进制时决定 core 相对名的落点（start.sh 内部会 cd 到构建目录）
ExecStart=/mnt/nvme/workspace/EdgeVisionService/scripts/run/out/start.sh
```

自检：`systemctl show -p LimitCORE evs-edgevision` 必须是 `LimitCORE=infinity`，且 `cat /proc/<主pid>/limits | grep -i core` 的 hard 是 `unlimited`。

---
