# scripts/ —— 可执行脚本总览

> 约定：**所有脚本从仓库根执行**（`cd /mnt/nvme/workspace/EdgeVisionService`），
> 脚本自己用 `ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"` 定位仓库根，不依赖 cwd。
> 命令的详细用法与实测输出不在这里，在 [docs/ops/COMMANDS.md](../docs/ops/COMMANDS.md)；崩溃原理在 [docs/ops/CRASH_DUMP.md](../docs/ops/CRASH_DUMP.md)。

```text
scripts/
├── README.md          ← 本页
├── run/               ← 启动程序（按构建目录分：out/ 与 build/）
│   ├── out/start.sh      当前默认构建目录 out/
│   └── build/start.sh    历史构建目录 build/（旧产物，不含崩溃采集）
├── coredump/          ← 崩溃现场全链路（体检 → 开 core → 归档 → 解析）
├── deploy/            ← 部署产物（systemd unit）
└── tests/             ← 可重跑的回归脚本（T1/T2/T3/T5 + fixtures）
```

## 1. 脚本清单

| 脚本 | 作用 | root | 依赖 | 实测状态 |
| --- | --- | --- | --- | --- |
| `run/out/start.sh` | 启动 `out/bin/EdgeVisionService`：抬 `ulimit -c`、设 `LD_LIBRARY_PATH` / `EVS_CRASH_DIR`、打印环境、前台 `exec` | 否（`LimitCORE` 足时） | 已构建的 `out/` | ✅ 4 路出帧、`Ctrl+C` 后 RC=0 |
| `run/build/start.sh` | 同上，`BUILD_DIR=build` | 否 | 已构建的 `build/` | ✅ 4 路出帧 RC=0（**旧产物，无 `[crash]`**） |
| `coredump/check_coredump_env.sh` | 崩溃体检：13 项 PASS/WARN/FAIL + 修复建议（只读） | 否 | bash | ✅ `PASS=5 WARN=9 FAIL=0` |
| `coredump/enable_coredump.sh` | 写 `core_pattern=…/core.%e.%p.%t.sig%s` + 提 `ulimit` + 建目录 | **是** | bash | 脚本就绪，未在目标机执行 |
| `coredump/collect_core.sh` | gdb 取崩溃帧 → 归档 `core.<app>.<pid>.<epoch>.sig<N>.<函数>@<文件>:<行>/`（core 硬链 + meta + 栈 + 报告） | 否 | gdb | ✅ 探针 + 真实 core 均通过 |
| `coredump/analyze_core.sh` | 标准 gdb 序列解析 core（`file` → `solib-search-path` → `core-file`） | 否 | gdb | ✅ 真实 24 线程 core 全解析 |
| `coredump/symbolize_report.sh` | 把崩溃报告里的地址符号化成「函数@文件:行」 | 否 | addr2line/gdb | ✅ 已用于报告 |
| `coredump/gcore_now.sh` | 对**活进程**抓快照（不杀进程），随后自动调 `analyze_core.sh` | 否（需 ptrace 权限） | gdb | 未实跑（需目标进程） |
| `deploy/evs-edgevision.service` | systemd unit：`LimitCORE=infinity` + `ExecStart=…/scripts/run/out/start.sh` | 是（安装） | systemd | 单元文件就绪 |
| `tests/run_t1_zlm_rtsp_key.sh` | T1：RTSP key 一致性（18 断言） | 否 | ZLM 库 | ✅ |
| `tests/run_t2_node_config.sh` | T2：配置解析 / 校验（53 断言） | 否 | g++ | ✅ |
| `tests/run_t3_node_json.sh` | T3：`node.json` 全量校验（104 断言） | 否 | g++ | ✅ |
| `tests/run_t5_builder.sh` | T5：`NodeFactory` + `PipelineBuilder`（64 断言） | 否 | g++ | ✅ |

## 2. 三条通用约定

1. **改完脚本先 `bash -n` 语法检查**，再实跑；不要靠"看起来对"。
2. **长输出落盘再读**：`脚本 > /tmp/evs_x.txt 2>&1; echo rc=$?`（见 [docs/ops/COMMANDS.md §0](../docs/ops/COMMANDS.md)）—— 直接看终端会被截断，且不知道退出码。
3. **退出码就是判据**：`0` 成功 / `1` 有 FAIL / `2` 参数或前置条件不满足。CI 与人工都只看这个，所以不要"失败也 return 0"。

## 3. 新增脚本该放哪

| 脚本性质 | 放哪 | 例子 |
| --- | --- | --- |
| 启动 / 停止程序 | `run/<构建目录>/` | `run/out/start.sh` |
| 崩溃 / core / 符号化 | `coredump/` | `collect_core.sh` |
| 系统配置 / 服务单元 / 开机自启 | `deploy/` | `evs-edgevision.service` |
| 可重跑的回归 / 验收 | `tests/` | `run_t3_node_json.sh` |

放好后：① 在**本页 §1 表格**登记一行（作用 / root / 依赖 / 实测状态）；② 若是"要敲的运维命令"，同步加到 [docs/ops/COMMANDS.md](../docs/ops/COMMANDS.md) 对应章节；③ 在 [docs/history/TESTLOG.md](../docs/history/TESTLOG.md) 留一条 L1 记录。

## 4. 与文档的对应关系

| 想知道 | 看 |
| --- | --- |
| 每条命令怎么敲、输出长什么样 | [docs/ops/COMMANDS.md](../docs/ops/COMMANDS.md)（§2.5 启动脚本 / §11 崩溃 / §12 自启） |
| 为什么这么抓、为什么抓不到 | [docs/ops/CRASH_DUMP.md](../docs/ops/CRASH_DUMP.md) |
| 某个任务验收了多少断言 | [docs/history/TESTLOG.md](../docs/history/TESTLOG.md) §5.5–§5.8 |
| 构建方式与红线 | [docs/HANDOFF.md](../docs/HANDOFF.md) §1.5 / §2 |
