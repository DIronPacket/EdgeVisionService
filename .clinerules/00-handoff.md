# 项目接手入口（Cline 自动加载 · 先读这一屏再动手）

> 本文件只是**索引 + 事实卡**，完整内容在 **`docs/HANDOFF.md`**（唯一权威接手文档）。
> **不要用 `search_codebase` 全仓扫描来"重新了解项目"**：文件清单在 HANDOFF §1.4，现状在 §0/§1.2，限制在 §2，任务→文件在 §1.9。

## 现状卡（2026-11-14 基线；若与 git 实际不符，以 git 为准并回写文档）

| 项 | 值 |
| --- | --- |
| 分支 / 基线 | `rk3588`（跟踪 `origin/rk3588`）· **本地 HEAD = 本提交**（2026-11-14 `comm:` 通信层 + 进程拆分 + 根 README 精简，**尚未推送**；`origin/rk3588` 仍为 `c6d040c`）· 工作树只剩 `config/2.mp4`、`config/test.nv12` 两台本机素材未跟踪（有意不入库）· **代码校准点 = 本提交**（前一校准点 `f0b7899` = 崩溃采集；再前 `45a070c` / `12f5480` / `15cf975` / `114c94d` / `c557d89` / `eac4bb0` / `29ab58b`） |
| 环境 | aarch64 板端本机编译 · **C++14** · cmake 3.22 · g++ 11.4 · 8 核 |
| 构建（已实测） | `make` → `cmake -S . -B out && cmake --build out -j"$(nproc)"`（默认构建目录 `out/`；2026-11-12 起，`build/` 为历史目录）。**进程可选编**（2026-11-14）：`-DEVS_PROCESS=media\|comm\|media,comm`（不指定 = 全编）、`make media` / `make comm`（单 target）、`make proc PROC=comm`（改默认集，持久）、`make info` 查当前值 |
| 冒烟运行 | 媒体 **`make run-media`**（`./bin/evs_media ../config/node.json`，4 路出帧 + `FreeMem` 后退出）；通信 **`make run-comm`**（`./bin/evs_comm ../config/comm.json`，无 broker 只告警不退出）；兼容壳 `make run`（`EdgeVisionService`，同 `evs_media`）。三者都等价 `cd out && LD_LIBRARY_PATH="lib:../install/rk3588/lib" ./bin/<目标>` |
| 真实主链路 | **两个进程 + 一个兼容壳**（2026-11-14 拆分）：`src/main/media/`（`evs_media`：`media_service.{h,cpp}` 编排 = 崩溃 handler → `load_with_fallback` → `MemInit` → `build_all` → `start_all` → 信号 → 有序停止 → `FreeMem`；`main_media.cpp` 只做入口）、`src/main/comm/`（`evs_comm`：`config/comm.json` → `CommManager` 建通道 → 收包/状态打印 → 信号 → `StopAll`+`Clear`）、`src/main/main.cpp`（兼容壳 `EdgeVisionService`，调同一个 `evs::media::Run()`）。**`src/comm/` 是库、`src/main/comm/` 是进程**，别混。`src/main/test/main_node.cpp`（`testnode`）**当前不参与构建** |
| 技术栈 | V4L2/FFmpeg 采集 → `MemBlock` 内存池（块内内存 rk3588 = MPP DRM 缓冲，`common/frame_mem.*`）→ `PipelineNode` 流水线 → MPP 编码 → ZLMediaKit RTSP；通信进程 `evs_comm` = `comm_api`（UDP/MQTT）旁路，**媒体进程尚未接总线** |
| 通信层（2026-11-14，已入库） | `src/comm/` → `libcomm_api.so`：`CommBase` 抽象基类（五个钩子 `DoInit/DoStart/DoStop/DoSend/ReceiveLoop` + 内部接收线程 + 状态机 + `last_error()`）、`UdpComm`（POSIX socket/poll，**零第三方依赖**）、`MqttComm`（libmosquitto **C API** + pimpl，`install/rk3588` 2.0.22，重连重订阅）、全局单例 `CommManager`（`Create` 工厂 + 注册表 + 统一回调带通道名）。★ **UDP 无需 zltoolkit**（`install/` 无其 C++ 头，只有 ZLMediaKit 的 C API）；★ MQTT 的 `kServer` = 通配订阅 `evs/v1/#` 的**订阅端角色**，broker 是独立进程。验收 `./scripts/tests/run_t6_comm.sh` → 43 项 PASS。**媒体进程未接总线**（`media_service.cpp` 不创建通道），**通信进程已用它**（`src/main/comm/main_comm.cpp` 读 `config/comm.json`）。说明见 `src/comm/README.md` |
| P0 问题 | 14 项（P0-2 / 3 / 4 / 6 / 7 / 12 / 13 / 14 已修复；P0-5 核实为假阳性；P0-9 / P0-11 于 2026-11-10 修复；P0-10 的「可拷贝 / 无析构」于同日 RAII 收口，**只剩 `capacity` 语义**；剩 3 项：P0-1 / P0-8 / P0-10（部分）），见 `docs/STATUS.md` §4.1 |

## 跳读表（**不要全文读**：先跑一条命令拿行号，再定向读区间）

```bash
grep -nE '^#{1,2} ' docs/HANDOFF.md     # ~150 token，一次拿全部章节行号
```

| 我要做的事 | 读 `docs/HANDOFF.md` 的哪些章节 |
| --- | --- |
| 不知道读哪篇 / 找某个子目录的说明 | 先看 **`docs/README.md`**（文档总索引）+ 各目录 `README.md`：`docs/` `scripts/` `cmake/` `config/` `src/` |
| 只想了解现状 | §0（30 秒事实卡）+ §1.2（关键坐标） |
| 动手改代码前 | §1.4（目录地图）+ §1.7（内存池/节点 API 速查） |
| **规定 / 红线（任何改动都必须读）** | §2 全部（2.1–2.9） |
| 修某个 P0/P1 | §1.5（构建坑）+ §1.9（任务→文件）+ R1 + R4 |
| 加一个新节点 | §2.4（流水线与内存池）+ R2 |
| **加通信通道 / 用 UDP·MQTT 收发 / 加通信后端** | **`src/comm/README.md`**（§3 server-client 语义 · §5 回调三条硬约束 · §9 已知限制）+ §1.4 的 `comm/` 块 + §1.9「用 UDP / MQTT 收发消息」行 |
| **拆分进程 / 只编一个进程（`evs_media` · `evs_comm`）** | §0「两个进程」行 + §1.5「进程可选编」行 + §2.5 第 7 条（`EVS_PROCESS`）+ §1.9「只编某个进程 / 加第三个进程」行 |
| **崩溃了 / 卡死 / 要看崩溃变量** | **`docs/ops/CRASH_DUMP.md`**（全流程 + core 命名 + 抓不到的原因（按链路 7 段 27 条））+ §1.10（四层与四条约束）+ `docs/ops/COMMANDS.md` §11（体检 / 归档改名 / 解析）+ `docs/history/BUG_CASES.md` B12/B13 |
| **程序开机自启** | `docs/ops/COMMANDS.md` §12（systemd unit；`LimitCORE=infinity` 决定 core 能不能落盘） |
| 排障 | R4（口诀） |
| **记测试 / 查回归结果 / 证明"已修复"** | 不在 HANDOFF：用 **`docs/history/TESTLOG.md`**（L1 冒烟表 + L2/L3 明细，含模板与必填项） |
| **复盘某个 bug 为什么发生 / 面试怎么讲** | 不在 HANDOFF：用 **`docs/history/BUG_CASES.md`**（原理剖析 + 代码位置 + 定位手法 + 面试说法；修 bug 后必须追加） |
| **命令怎么敲（构建 / 运行 / 看内存 / 临时探针 / git）** | 不在 HANDOFF：用 **`docs/ops/COMMANDS.md`**（分类命令手册，每条带作用与实测输出，末尾有踩坑表） |
| **准备面试（怎么讲 / 内核问到哪 / 练 C 去哪练）** | 不在 HANDOFF：用 **`docs/interview/INTERVIEW.md`**（高频 · 核心 · 深入问答，附内核边界与 C 语言落点） |
| 全量接手 / 换人 | 全文（约 450 行，~10k token，仅必要时代价） |

> 这套跳读法把常任务的上下文成本压到 1–4k token；只有"第一次全量接手"才需要读全文。

## 硬约束（摘自 HANDOFF §2，违反即错误交付）

1. **C++14**：不用 `std::any` / `if constexpr` / 结构化绑定等 C++17+ 特性。
2. 不引入 `install/` 中没有的新第三方库；不重命名目录（`moudle_*` 保持原样）。
3. 不提交 `out/`、`build/`、`install/`、`3rdparty/`、`.vscode/`（已在 `.gitignore`）。
4. 不 `git push --force`、不 `git reset --hard`、不碰 `main` 分支。
5. 内存池取块（`acquire` / `getBlock`）之后，**所有失败分支都必须 `release`**。
6. 没有实际执行编译/运行，不许声称"已修复 / 已通过"。
7. 不做任务范围之外的"顺手修"（格式化、改名、重构）。
8. **测试必须留痕**：改代码/改配置后按 `docs/history/TESTLOG.md` 的分级写记录（L1 一行冒烟 / L2 任务小节 / L3 里程碑），无记录的"已通过"视为未验证。
9. **修 bug 必须写档案**（用户 2026-11-09 明确要求）：凡是"有原理含量"的 bug（现象与根因跨层 / 只在并发时序内存残留下复现 / 涉及资源所有权与跨组件共享状态 / 曾被误判方向），修完必须在 **`docs/history/BUG_CASES.md`** 追加案例（现象 → 位置到函数级 → 根因原理 → 修复 → 为什么难发现 → 面试怎么讲），并更新其 §1 索引表；与代码同一批提交。纯笔误只在 `STATUS.md` 标注。
10. **`git push` 之前必须先问用户**（用户 2026-11-14 明确要求）：先说明**要推哪几个 commit、推到哪个分支、涉及哪些文件**，拿到明确答复（"推"/"可以"）后才执行 `git push`。**不许**在收尾流程里"顺手推一下"，也不许因为上一次同意过就默认继续推。`git push --force` 永远禁止（见第 4 条）。细则见 `docs/HANDOFF.md` §2.6 第 9 条。


## 会话收尾

**提交 ≠ 推送**：`git commit` 后默认停手；要不要 `git push` 由用户决定（硬约束第 10 条：推送前必须先问，报「推哪几笔 + 哪个分支 + 哪些文件」）。

代码/构建/依赖发生变化时（新的代码提交 / 构建方式改了 / 修掉某个 P0；**纯 `docs:` 提交不算**），同步更新：
`docs/HANDOFF.md` 的 §0 事实卡、§1.2 关键坐标、附录 B 变更记录，以及本文件的现状卡。

任何构建/运行/测试之后，把结论追加到 `docs/history/TESTLOG.md`（L1 一行式即可；修 P0/P1 或完成一个任务时写 L2 小节），提交时与代码同一 commit。

修掉"有原理含量"的 bug（见硬约束第 9 条）时，同时追加 `docs/history/BUG_CASES.md` 案例并更新其 §1 索引表。
