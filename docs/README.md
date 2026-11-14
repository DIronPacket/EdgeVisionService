# docs/ —— 文档总索引

> **先看这一页，再决定读哪篇。** 文档按「谁在用、什么时候用」分成 4 类，
> 根目录只放**每天都要碰的三篇**（接手 / 架构 / 现状），其余进子目录。
>
> 目录结构变了也不必慌：任何一篇文档开头的引用都指向新路径，链接全部可点。

## 1. 一屏导航（按场景找文档，不用记目录）

| 我想…… | 读这篇 | 在哪 |
| --- | --- | --- |
| **接手项目 / 换人 / 问"现在是什么状态、规矩是什么"** | [HANDOFF.md](HANDOFF.md)（唯一入口：事实卡 + 红线 + 操作规程） | 根 |
| 搞清**分层、数据流、内存/线程模型、怎么加一个节点** | [ARCHITECTURE.md](ARCHITECTURE.md) | 根 |
| 看**模块完成度、P0/P1 清单、哪些功能是假的** | [STATUS.md](STATUS.md) | 根 |
| **敲命令**：构建 / 运行 / 看内存 / 探针 / git / 崩溃 / 自启 | [ops/COMMANDS.md](ops/COMMANDS.md) | `ops/` |
| **程序崩了 / 卡死 / 要抓 core 看变量** | [ops/CRASH_DUMP.md](ops/CRASH_DUMP.md) | `ops/` |
| 记测试结果 / 证明"已修复" / 查回归 | [history/TESTLOG.md](history/TESTLOG.md) | `history/` |
| 复盘某个 bug 为什么发生 / 面试怎么讲 | [history/BUG_CASES.md](history/BUG_CASES.md) | `history/` |
| 看**未来做什么**、三进程拆分、多链路设计 | [design/ROADMAP.md](design/ROADMAP.md) · [design/DESIGN_MULTI_PIPELINE.md](design/DESIGN_MULTI_PIPELINE.md) · [design/DESIGN_PROCESS_SPLIT.md](design/DESIGN_PROCESS_SPLIT.md) | `design/` |
| 准备面试（怎么讲、内核问到哪、C 语言落点） | [interview/INTERVIEW.md](interview/INTERVIEW.md) | `interview/` |
| 想看**代码之外**的目录怎么组织（脚本 / 依赖查找 / 配置 / 源码） | [../scripts/README.md](../scripts/README.md) · [../cmake/README.md](../cmake/README.md) · [../config/README.md](../config/README.md) · [../src/README.md](../src/README.md) | 仓库根各目录 |

## 2. 目录结构与定位

```text
docs/
├── README.md              ← 本页：总索引（新文档先在这里登记）
├── HANDOFF.md             ← 【根】接手唯一入口：现状事实卡 / 红线 §2 / 任务→文件索引 §1.9
├── ARCHITECTURE.md        ← 【根】当前架构：分层、数据流、内存/线程模型、扩展方式
├── STATUS.md              ← 【根】当前现状：模块完成度表 + P0/P1 清单
│
├── ops/                   ← 运维与排障（要动手敲命令时来这）
│   ├── README.md
│   ├── COMMANDS.md        ← 分类命令手册（每条带作用 + 实测输出 + 踩坑表）§11 崩溃 §12 自启
│   └── CRASH_DUMP.md      ← 崩溃全链路：信号→handler→core、命名格式符、抓不到的原因
│
├── design/                ← 设计与规划（决定"以后怎么做"时来这，不描述现状）
│   ├── README.md
│   ├── ROADMAP.md         ← 功能路线图（H.264 / RGA / OSD / 录像 / 交叉编译 / OTA）
│   ├── DESIGN_MULTI_PIPELINE.md  ← 多链路配置 + 进程级资源上提（T5/T6/T7 已按此落地）
│   └── DESIGN_PROCESS_SPLIT.md   ← 单进程 → 媒体/通信/升级 三进程（未落地）
│
├── history/               ← 记录与证据（"发生过什么"时来这）
│   ├── README.md
│   ├── TESTLOG.md         ← 测试留痕：L1 一行 / L2 任务小节 / L3 里程碑
│   └── BUG_CASES.md       ← bug 原理档案（现象→位置→根因→修复→为何难发现→面试说法）
│
└── interview/             ← 输出型文档
    ├── README.md
    └── INTERVIEW.md       ← 面试问答手册（高频 20 · 核心 15 · 深入 12 + 内核边界 + C 落点）
```

**分区判据（新增文档按这个对号入座）**：

| 子目录 | 放什么 | 判据 |
| --- | --- | --- |
| 根 | 入口 / 架构 / 现状 | 「**所有人每天都要看**」或「**只有一个权威版本**」 |
| `ops/` | 命令、排障流程、部署 | 内容主体是「**步骤 / 命令 / 判据**」，读者要立刻执行 |
| `design/` | 设计提案、路线图 | 内容主体是「**取舍 / 方案 / 排期**」，回答"为什么不那样做" |
| `history/` | 测试记录、bug 档案 | 内容**只追加不改写**（每条的日期与结论是证据，事后不能改） |
| `interview/` | 面试问答 | 面向**对外表达**，同一份事实换一种讲法 |

## 3. 建议阅读顺序

| 你的目标 | 顺序 |
| --- | --- |
| 只想跑起来 | [HANDOFF §0](HANDOFF.md)（事实卡）→ [ops/COMMANDS.md §1–§2](ops/COMMANDS.md) → `make && make run` |
| 要改代码 | [HANDOFF §2](HANDOFF.md)（红线，必须全读）→ [ARCHITECTURE](ARCHITECTURE.md) → [STATUS](STATUS.md) → [../src/README.md](../src/README.md)（源码分层与改动落点） |
| 要抓崩溃 | [ops/CRASH_DUMP.md](ops/CRASH_DUMP.md) → [ops/COMMANDS.md §11](ops/COMMANDS.md) → [history/BUG_CASES.md](history/BUG_CASES.md) B12/B13 |
| 要上线部署 | [ops/COMMANDS.md §12](ops/COMMANDS.md)（systemd）→ [ops/CRASH_DUMP.md](ops/CRASH_DUMP.md) §5（`LimitCORE`） |
| 要面试 | [interview/INTERVIEW.md](interview/INTERVIEW.md) §0 三段话 → §9 速查表 → [history/BUG_CASES.md](history/BUG_CASES.md) 挑 3 个案例 |

## 4. 维护规则（改文档前必读）

1. **单一权威**：同一事实只在一处详写，其余地方给一句话 + 链接。
   例：规矩只在 [HANDOFF §2](HANDOFF.md)，命令只在 [ops/COMMANDS.md](ops/COMMANDS.md)。
2. **新增文档**：先按 §2 判据选目录，再在**本页 §1 表格**登记一行，否则等于没写。
3. **移动 / 改名**：必须同时改所有引用（`grep -rn '<旧名>' docs README.md AGENTS.md .clinerules scripts src`），
   并跑一次链接体检：`python3 - <<'PY'` 自检所有相对链接（见 [ops/COMMANDS.md §8](ops/COMMANDS.md)）。
4. **history/ 只追加**：`TESTLOG.md` 与 `BUG_CASES.md` 的既有条目不得回改结论；新结论另起一条并注明日期。
5. **行号会漂移**：读大文件前先 `grep -nE '^#{1,2} ' <文件>` 拿章节行号，再定向读区间。
