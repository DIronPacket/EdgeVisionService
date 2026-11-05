# 项目接手入口（Cline 自动加载 · 先读这一屏再动手）

> 本文件只是**索引 + 事实卡**，完整内容在 **`docs/HANDOFF.md`**（唯一权威接手文档）。
> **不要用 `search_codebase` 全仓扫描来"重新了解项目"**：文件清单在 HANDOFF §1.4，现状在 §0/§1.2，限制在 §2，任务→文件在 §1.9。

## 现状卡（2026-11-05 基线；若与 git 实际不符，以 git 为准并回写文档）

| 项 | 值 |
| --- | --- |
| 分支 / HEAD | `rk3588`（跟踪 `origin/rk3588`）· `e30ccb3` · 工作树干净 |
| 环境 | aarch64 板端本机编译 · **C++14** · cmake 3.22 · g++ 11.4 · 8 核 |
| 构建（已实测） | `cmake -S . -B build && cmake --build build -j"$(nproc)"` |
| 冒烟运行 | `./build/run.sh EdgeVisionService` → 期望 `EdgeVisionService start` |
| 真实主链路 | `src/main/test/main_node.cpp`（可执行 `testnode`）；`src/main/main.cpp` 是空壳 |
| 技术栈 | V4L2/FFmpeg 采集 → `MemBlock` 内存池 → `PipelineNode` 流水线 → MPP 编码 → ZLMediaKit RTSP |
| P0 问题 | 11 项，见 `docs/STATUS.md` §4.1 |

## 跳读表（行号基于 `docs/HANDOFF.md` 当前版本）

| 我要做的事 | 读哪一段 |
| --- | --- |
| 只想了解现状 | 13–26（§0 事实卡）、63–75（§1.2 关键坐标） |
| 动手改代码前 | 89–135（§1.4 目录地图）、186–204（§1.7 API 速查） |
| **规定 / 红线（任何改动都必须读）** | 233–343（§2 全文） |
| 修某个 P0/P1 | 136–160（§1.5 构建坑）、218–232（§1.9 索引）、346–355（R1）、374–385（R4） |
| 加一个新节点 | 280–288（§2.4）、356–365（R2） |
| 排障 | 374–385（R4 口诀） |
| 全量接手 / 换人 | 全文（445 行，~10k token，仅必要时代价） |

> 行号会随文档编辑漂移，一条命令刷新：`grep -nE '^#{1,2} ' docs/HANDOFF.md`

## 硬约束（摘自 HANDOFF §2，违反即错误交付）

1. **C++14**：不用 `std::any` / `if constexpr` / 结构化绑定等 C++17+ 特性。
2. 不引入 `install/` 中没有的新第三方库；不重命名目录（`moudle_*` 保持原样）。
3. 不提交 `build/`、`install/`、`3rdparty/`、`.vscode/`（已在 `.gitignore`）。
4. 不 `git push --force`、不 `git reset --hard`、不碰 `main` 分支。
5. 内存池取块（`acquire` / `getBlock`）之后，**所有失败分支都必须 `release`**。
6. 没有实际执行编译/运行，不许声称"已修复 / 已通过"。
7. 不做任务范围之外的"顺手修"（格式化、改名、重构）。

## 会话收尾

基线变化时（新提交 / 构建方式变了 / 修掉某个 P0），同步更新：
`docs/HANDOFF.md` 的 §0 事实卡、§1.2 关键坐标、附录 B 变更记录，以及本文件的现状卡。
