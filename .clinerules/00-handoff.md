# 项目接手入口（Cline 自动加载 · 先读这一屏再动手）

> 本文件只是**索引 + 事实卡**，完整内容在 **`docs/HANDOFF.md`**（唯一权威接手文档）。
> **不要用 `search_codebase` 全仓扫描来"重新了解项目"**：文件清单在 HANDOFF §1.4，现状在 §0/§1.2，限制在 §2，任务→文件在 §1.9。

## 现状卡（2026-11-09 基线；若与 git 实际不符，以 git 为准并回写文档）

| 项 | 值 |
| --- | --- |
| 分支 / 基线 | `rk3588`（跟踪 `origin/rk3588`）· 工作树干净 · 本文**代码**校准点 `eee3b01`（P0-14：两个采集后端写块前按本链路分辨率重绑 `FrameBuffer` 视图，修四路并发下 1080p 路下半屏绿；前一校准点 `c557d89` = P0-13 `MppEncoder` 按平面拷贝 NV12；再前 `eac4bb0` = T7 `ViNode` 按 `vi.src` 前缀选采集后端 + 修 P0-2；再前 `29ab58b` = T6 配置 → `PipelineBuilder` → SIGINT 优雅退出） |
| 环境 | aarch64 板端本机编译 · **C++14** · cmake 3.22 · g++ 11.4 · 8 核 |
| 构建（已实测） | `cmake -S . -B build && cmake --build build -j"$(nproc)"` |
| 冒烟运行 | `./build/run.sh EdgeVisionService` → 期望 `EdgeVisionService start` |
| 真实主链路 | `src/main/test/main_node.cpp`（可执行 `testnode`）；`src/main/main.cpp` 是空壳 |
| 技术栈 | V4L2/FFmpeg 采集 → `MemBlock` 内存池 → `PipelineNode` 流水线 → MPP 编码 → ZLMediaKit RTSP |
| P0 问题 | 14 项（P0-2 / 3 / 4 / 6 / 7 / 12 / 13 / 14 已修复，剩 6 项），见 `docs/STATUS.md` §4.1 |

## 跳读表（**不要全文读**：先跑一条命令拿行号，再定向读区间）

```bash
grep -nE '^#{1,2} ' docs/HANDOFF.md     # ~150 token，一次拿全部章节行号
```

| 我要做的事 | 读 `docs/HANDOFF.md` 的哪些章节 |
| --- | --- |
| 只想了解现状 | §0（30 秒事实卡）+ §1.2（关键坐标） |
| 动手改代码前 | §1.4（目录地图）+ §1.7（内存池/节点 API 速查） |
| **规定 / 红线（任何改动都必须读）** | §2 全部（2.1–2.9） |
| 修某个 P0/P1 | §1.5（构建坑）+ §1.9（任务→文件）+ R1 + R4 |
| 加一个新节点 | §2.4（流水线与内存池）+ R2 |
| 排障 | R4（口诀） |
| **记测试 / 查回归结果 / 证明"已修复"** | 不在 HANDOFF：用 **`docs/TESTLOG.md`**（L1 冒烟表 + L2/L3 明细，含模板与必填项） |
| 全量接手 / 换人 | 全文（约 450 行，~10k token，仅必要时代价） |

> 这套跳读法把常任务的上下文成本压到 1–4k token；只有"第一次全量接手"才需要读全文。

## 硬约束（摘自 HANDOFF §2，违反即错误交付）

1. **C++14**：不用 `std::any` / `if constexpr` / 结构化绑定等 C++17+ 特性。
2. 不引入 `install/` 中没有的新第三方库；不重命名目录（`moudle_*` 保持原样）。
3. 不提交 `build/`、`install/`、`3rdparty/`、`.vscode/`（已在 `.gitignore`）。
4. 不 `git push --force`、不 `git reset --hard`、不碰 `main` 分支。
5. 内存池取块（`acquire` / `getBlock`）之后，**所有失败分支都必须 `release`**。
6. 没有实际执行编译/运行，不许声称"已修复 / 已通过"。
7. 不做任务范围之外的"顺手修"（格式化、改名、重构）。
8. **测试必须留痕**：改代码/改配置后按 `docs/TESTLOG.md` 的分级写记录（L1 一行冒烟 / L2 任务小节 / L3 里程碑），无记录的"已通过"视为未验证。


## 会话收尾

代码/构建/依赖发生变化时（新的代码提交 / 构建方式改了 / 修掉某个 P0；**纯 `docs:` 提交不算**），同步更新：
`docs/HANDOFF.md` 的 §0 事实卡、§1.2 关键坐标、附录 B 变更记录，以及本文件的现状卡。

任何构建/运行/测试之后，把结论追加到 `docs/TESTLOG.md`（L1 一行式即可；修 P0/P1 或完成一个任务时写 L2 小节），提交时与代码同一 commit。
