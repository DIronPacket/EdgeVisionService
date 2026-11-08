# TESTLOG — 测试与验证记录

> **用途**：每次改代码 / 改配置 / 修 P0 / 加功能之后，把「跑了什么命令、输出什么、结论是什么」留痕。
> 目的只有一个：**任何"已修复 / 已通过"都能被复现**（对应 HANDOFF §2.1 红线 6：没实际执行不许声称通过）。
> 记录规则见下方「分级」；验证顺序见 HANDOFF §2.2。

## 1. 记录粒度（分级，防止记录成本超过测试成本）

| 级别 | 什么时候写 | 篇幅 | 估算成本 | 举例 |
| --- | --- | --- | --- | --- |
| **L1 冒烟** | 每次改完代码，跑了构建 / 跑了一个二进制 | **表格 1 行** | ~30 token | 「`cmake --build` 通过，`run.sh EdgeVisionService` 输出 start」 |
| **L2 任务** | 完成一个任务（T1/T2/…）或修掉一个 P0/P1 | **5–15 行小节** | ~200–400 token | 「T1：P0-6 修复，启停 ×10 后 map 为空」 |
| **L3 版本** | 一个里程碑收尾（M1/M2）或一次长时间压测 | **独立一节**（必要时拆 `docs/report/M1.md`） | ~600–1000 token | 「M1：4 路 30 分钟、4 个 URL 出帧、CPU/RSS/温度实测」 |

**关于 token 成本的结论（评估过，值得写）**：

1. 记录本身很便宜：L1 ≈ 30 token、L2 ≈ 200–400 token。而**一次真实测试**读取构建输出 / 日志 / `git diff` 通常是 2k–10k token — 记录只占 5% 以内。
2. 真正会失控的不是"写记录"，而是**把日志全文贴进文档**（一次贴 300 行日志 ≈ 3k token，且每次后续会话读文档都要再付一次）。
   → 所以规定：**只贴命令 + 结果摘要（≤5 行）+ 结论**，原始日志留在 `/tmp` 或 `build/`，需要时用命令重跑。
3. 因此采用**分级记录**：小改动只写 L1 一行，任务完成写 L2 小节，里程碑写 L3。**不做"每次运行都写长报告"**——那是浪费。
4. 只有一种情况必须立刻写、不能攒：**修复类改动（P0/P1）在提交前**必须有一条 L2 —— 否则事后无法证明"修好了"。

## 2. 记录模板

**L1（直接追加到第 4 节表格）**

```
| 2026-11-09 | T1 | `cmake --build build -j8` 通过；`run.sh testnode` 启动日志正常 | ✅ |
```

**L2 / L3**

```markdown
### L2 · <任务ID> <一句话目标>（YYYY-MM-DD）
- 代码基线：`rk3588 @ <commit>`（未提交写「工作树」）
- 命令：
  ```bash
  ...
  ```
- 结果摘要：（关键输出 1–5 行，长日志不贴）
- 结论：✅ 通过 / ⚠️ 部分通过（遗留 XX，转任务 Tn）/ ❌ 失败（原因 + 下一步）
- 未做 / 受限：例如「板端无 `/dev/video0`，运行验证未做」
```

## 3. 记录里必须出现的信息（缺一即无效）

| 必填 | 说明 |
| --- | --- |
| 日期 + 基线 | commit 或「工作树」；基线不写，结论就不可回溯 |
| 实际执行的命令 | 能复制粘贴直接重跑；不允许写"已测试"了事 |
| 结果摘要 | 关键数字（帧数、耗时、CPU、错误码、文件是否存在） |
| 结论 | 通过 / 部分通过 / 失败，**部分通过必须写清遗留** |
| 未做项 | 因设备、依赖、环境没做的验证要显式声明 |

## 4. 快速索引（L1 冒烟汇总，一行一条）

| 日期 | 任务 | 命令与结果 | 结论 |
| --- | --- | --- | --- |
| 2026-11-05 | 基线 | `cmake -S . -B build && cmake --build build -j8` 全量通过；`./build/run.sh EdgeVisionService` → `EdgeVisionService start` | ✅（来源 HANDOFF §0/§1.5，本次未重跑） |
| 2026-11-08 | VpssNode 新增 | `cmake --build build -j8` 通过；临时测试程序全部用例通过（见 5.2） | ✅ |
| 2026-11-08 | config/node.json 新增 | `python3 -m json.tool config/node.json` → exit=0，236 行 | ✅ |
| 2026-11-08 | 提交前构建（preprocess + VpssNode + 文档） | `cmake -S . -B build && cmake --build build -j8` → `EXIT=0`，10 个 target 全部 built；configure 打印 `preprocess OSD: FreeType disabled`（新开关生效） | ✅ 编译通过（`preprocess` 像素实现仍为 `ERR_NOT_IMPL`，无功能验证） |
| 2026-11-08 | 提交入库 | `e82e39e`（代码 22 文件）+ `d9f5ca2`（文档 6 文件）；提交后 `git status --short` 为空 | ✅ 工作树干净 |

## 5. 详细记录（时间正序，新的追加在末尾）

### 5.1 L3 · 基线冒烟（2026-11-05）

- 代码基线：`rk3588 @ d158eca`
- 命令：
  ```bash
  cmake -S . -B build && cmake --build build -j"$(nproc)"
  ./build/run.sh EdgeVisionService
  ```
- 结果摘要：全量编译通过（含 `rtsp` 目标，因为 `build/CMakeCache.txt` 缓存了 `BUILD_RTSP_MODULE=ON`）；输出 `EdgeVisionService start`。
- 结论：✅ **可构建、可启动**。
- 未做 / 受限：`testnode` 需真实 `/dev/video0`，无设备时按 P0-2 崩溃属已知；x86 构建必然失败（P0-1）。
- 备注：本条为 HANDOFF §0/§1.5 既有事实的归档，**本轮未重跑**。

### 5.2 L2 · VpssNode 样板节点验证（2026-11-08）

- 代码基线：`rk3588 @ 7a2055b + 工作树`（`src/pipeline/moudle_vpss/` 未提交）
- 命令：
  ```bash
  cmake -S . -B build && cmake --build build -j"$(nproc)"
  # 临时测试程序：/tmp/vpss_test.cpp（未入库，随后已清理，命令不可复现）
  /tmp/vpss_test
  ```
- 结果摘要：透传模式 **78/78** 帧通过；转码模式 **78/78** 帧通过（每块都产出独立的输出帧）；非法配置（空指针 / 缺 `vpss` 段 / `dst_fmt` 乱填 / `align<=0`）**均不崩溃**且回退默认值；符号已导出。
- 结论：✅ 样板代码可用；`process_frame()` 仍是 `return true` 占位（等价透传），待 `pre::ImagePreprocessor` 像素实现后填。
- 未做 / 受限：测试程序是临时的 `/tmp/vpss_test.cpp`，**未入库**（结论留档，无法直接重跑）；未接入 `main_node.cpp`，未做多路/长跑验证。

### 5.3 L1 · `config/node.json` 语法与结构校验（2026-11-08）

- 代码基线：工作树（文件未提交）
- 命令：
  ```bash
  python3 -m json.tool config/node.json > /dev/null   # exit=0
  ```
- 结果摘要：语法合法；文件 236 行；4 条链路 `channel` 0/1/2/3，`stream` `ch0..ch3`，`vi` 与 `push` 宽高逐路一致。
- 结论：✅ 配置语法正确。
- 未做 / 受限：**程序尚未读取该文件**（`NodeConfigManager` 属 M1 的 T2，未实现），故"能被正确解析并生效"尚未验证 → 由 T2/T3 验收。

### 5.4 L1 · 建立测试记录机制本身（2026-11-08）

- 代码基线：工作树（**纯文档改动**，未触碰 `src/`、`CMakeLists.txt`）
- 命令：
  ```bash
  # 无构建/运行；仅文档改动
  git --no-pager status --short    # 确认改动范围：docs/ · AGENTS.md · .clinerules/ · config/node.json
  ```
- 结果摘要：新增本文；HANDOFF §1.4/§1.9/§2.2 第 9 条/R5/附录 B 同步；`.clinerules/00-handoff.md` 与 `AGENTS.md` 入口各加一条指向本文。
- 结论：✅ 机制生效（本文即自用示例）。
- 未做 / 受限：**本轮没有代码改动，因此没有构建/运行验证**；首次真正用它记录的是下一个任务（T1）。

---

## 6. 维护约定

1. 提交代码时，同一 commit 里**必须**包含对应的 TESTLOG 记录（L1 或 L2）。
2. 本文只写**测试证据**；方案写 `DESIGN_MULTI_PIPELINE.md`，现状写 `STATUS.md`，规矩写 `HANDOFF.md`（分工见 HANDOFF R5）。
3. 本文超过 ~400 行时，把最早的 L1 行折叠成一行汇总（"2026-11 共 N 次构建通过"），L2/L3 明细保留。
4. 若某条记录后来被推翻（例如复测失败），**不改旧记录**，追加新记录并注明「推翻 5.x」。
