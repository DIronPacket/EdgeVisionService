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
| 2026-11-08 | 内存池归属方案（纯文档 + `node.json` 增补字段） | `python3 -m json.tool config/node.json` → `JSON_OK`（239 行）；`git diff --stat` 仅 `config/node.json`(+5/-1) 与 `docs/DESIGN_MULTI_PIPELINE.md`(+102)；**未改 C++ 源码，未重新编译** | ✅ 文档/配置一致（`quota_per_channel` 尚未被二进制读取，随 T10a 生效） |
| 2026-11-08 | 池方案决策 **D9 = A**（纯文档） | `python3 -m json.tool config/node.json` → `JSON_OK`；`D9` 已出现在 DESIGN §1.4 决策表 / §9 第 6 项 / §11.6.2 / §11.6.4 / §11.6.6 / §11.6.7 / §12 变更记录（口径一致：共享池 + 每路配额，A+ 不实施）；**未改 C++，未重新编译** | ✅ 决策沉淀完成（T10a 范围与人日同步为 1.5；`node.json` 注释标注 D9） |
| 2026-11-08 | **T1** 修 P0-6 / P0-7 | `cmake --build build --target rtsp -j8` → `EXIT=0`；`cmake --build build -j8` 全量 → `EXIT=0`（含 `testnode`）；`./build/run.sh EdgeVisionService` → `EdgeVisionService start`；`./scripts/tests/run_t1_zlm_rtsp_key.sh` → `ALL PASS (fails=0)`，18 项断言（见 5.5） | ✅ 代码 + 冒烟 + 验收断言全通过 |
| 2026-11-08 | **T2** `NodeConfigManager`（v1 解析/校验/v0 适配） | `cmake -S . -B build && cmake --build build -j8` → `EXIT=0`；`./build/run.sh EdgeVisionService` → `EdgeVisionService start`；`./scripts/tests/run_t2_node_config.sh` → `ALL PASS (fails=0)`，53 项断言（见 5.6） | ✅ 代码 + 冒烟 + 验收断言全通过 |
| 2026-11-08 | **T3** `config/node.json` 定稿（4 路） | `./scripts/tests/run_t3_node_json.sh` → `passes=104 fails=0 skips=0`，`EXIT=0`；素材缺失分支 `passes=95 skips=1`；`./build/run.sh EdgeVisionService` → `EdgeVisionService start`（见 5.7） | ✅ 配置层通过（端到端待 T6/T7b） |
| 2026-11-08 | **T5** `NodeFactory` + `PipelineBuilder` | `./scripts/tests/run_t5_builder.sh` → `passes=64 fails=0 skips=2`，`EXIT=0`；真实 `node.json` 建成 4 条链；T3 `passes=104`、T2 `ALL PASS` 回归通过；全量 `cmake --build` 通过 + `run.sh EdgeVisionService` → start（见 5.8） | ✅ 装配层通过（`start_all` 顺序待 T6/T7 板端验） |
| 2026-11-08 | **T6** `main_node.cpp` 改为 builder 驱动 + SIGINT 优雅退出 | `cmake --build build -j8` → `EXIT=0`；`timeout --preserve-status -s INT 12 ./build/run.sh testnode config/push.json` → `EXIT=0`（`vi0 → rtsp0 → print0` 正序停止、`FreeMem 完成`、无残留进程）；同命令跑 `config/node.json` → 4 条链建成/启动/正序停止、`EXIT=0`；T5 `passes=64`、T3 `passes=104`、T2 `ALL PASS`；冒烟 `run.sh EdgeVisionService` → start（见 5.9） | ✅ 进程编排层通过（4 路**出帧**待 T7/T7b：`ViNode` 仍硬编码同一 `/dev/video0`） |
| 2026-11-09 | README 重写（文档 + 命令实测） | `./build/run.sh testnode`（无参）→ `[builder] 汇总: 建成 4 条, 失败 0 条, 跳过 0 条`，`V1_EXIT=0`（证实无参时命中 `../config/node.json`）；`./build/run.sh testnode ../config/push.json` → `建成 1 条`，`V0_EXIT=0`；README 代码围栏 16 个（成对闭合）、8 个相对链接目标全部存在；改 `config/node.json` 注释后 `python3 -m json.tool` → `JSON_OK`、`run_t3_node_json.sh` → `passes=104 fails=0 skips=0` | ✅ 文档里写出的运行命令与输出均实测；无残留进程 |
| 2026-11-09 | **T7** `ViNode` 解析 `vi.src` 前缀选采集后端 + 修 P0-2 | `cmake --build build -j8` → `BUILD_EXIT=0`；`testnode config/push.json` → 源仍是 `/dev/video0:640:480:NV12:30`（v0 不变）、`EXIT=0`；`testnode config/node.json` → `dev:`→V4L2、3 路 `file:`→FFmpeg 全部 open 成功，四路 29.96 / 23.96 / 23.93 / 23.94fps，`EXIT=0`；设备/文件不存在的失败场景 → `[vi0] open failed` + 有序停止 + `EXIT=0`（无段错误）；T5/T3/T2 回归全通过 + 冒烟 start（见 5.10） | ✅ 通过（**四路同时出帧成立**；mp4 不 loop 待 T7b） |



| 2026-11-09 | **P0-13** NV12 UV 平面错位（mp4 路 Y 正常、UV 绿斑/花屏） | `cmake --build build -j8` → `BUILD_EXIT=0`；四路 `testnode ../config/node.json` → `EXIT=0`、`hal_h265e_v580_ret_task` 报错 **0** 次、29.82 / 23.94 / 23.93 / 23.96fps；拉流解码统计 `U=128.1±10.9 / V=128.0±1.6`（修复前 `105.4±41.1 / 105.5±41.2`，源文件 `126.6±11.0 / 128.2±1.7`）；ch0（1920×1080）`U=70.5±66.1 / V=68.7±64.3` 与修复前一致（无回归）；T5 `passes=64` / T3 `passes=104` / T2 `ALL PASS`（见 5.11） | ✅ 通过（UV 与源吻合，1080p 路零回归） |

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

### 5.5 L2 · T1 修 P0-6 / P0-7（RTSP 服务 key、ref_cnt 与入参校验）（2026-11-08）

- 代码基线：工作树（`rk3588`，基于 `d9f5ca2`）；改动 `src/platform/rtsp/zlm_push_server.{cpp,h}` + 新增 `scripts/tests/`
- 命令：
  ```bash
  cmake --build build --target rtsp -j"$(nproc)"   # EXIT=0
  cmake --build build -j"$(nproc)"                 # 全量 EXIT=0（含 testnode）
  ./build/run.sh EdgeVisionService                 # → EdgeVisionService start
  ./scripts/tests/run_t1_zlm_rtsp_key.sh           # 18 项断言，默认端口 18550
  ```
- 结果摘要：
  - `ALL PASS (fails=0)`；`out_code=nullptr` 不崩溃；空 `stream` / 0×0 / `fps=0` 均返回 `ZLM_ERR_INVALID_PARAM`
  - stop 之后同名流可重新注册（**旧实现此处永为 `ZLM_ERR_STREAM_EXIST`**）
  - 全部 stop 后 `streams=0 servers=0`，端口监听=0；重复 stop 幂等；已停句柄送帧回 `ZLM_ERR_NOT_FOUND`（不崩溃）
  - start/stop ×10：每轮 `streams/servers` 起 1 落 0
- 结论：✅ 通过。同一文件三处缺陷（同源）：
  1. **P0-6-a**：`zlm_rtsp_stop()` 用 `bind_ip + port` 拼流 key，而 `bind_ip` 从未被赋值 → key 为 `":8550"`，与注册时的 `"8550"` 不等，停止路径静默失效。改为 `RtspServerCtx`/`OneKeyStreamCtx` 自带 `key`，`ServerKey(port)` / `StreamKey(port,app,stream)` 成为唯一生成入口。
  2. **P0-6-b（本次新发现）**：重复注册分支在 `ref_cnt++` **之后**直接 `return`，每重复注册一次就漏一个服务引用 → 最后一个流 stop 时 `ref_cnt` 归不到 0、`mk_stop_all_server()` 永不执行、server 永久滞留（实测先出现 `streams=0 / servers=1`）。抽出 `ReleaseServerRef()` 作为唯一释放点（正常 stop / 重复注册回滚 / `mk_media_create` 失败三路共用）。
  3. **P0-7**：`!out_code` 判断移到最前并直接返回，不再"先判空、后解引用"。
  - 同因附带修复：`zlm_rtsp_stop()` 与 `zlm_rtsp_input_encodeData()` 原先直接 `static_cast` 解引用句柄；stop 真正生效后，对已停句柄再 stop / 再送帧即 UAF。改为先按指针在表内定位，命中才访问成员。
  - 新增只读自检接口 `zlm_rtsp_stream_count()` / `zlm_rtsp_server_count()`（`zlm_push_server.h`）：两张表是文件内 `static`，不暴露就无法断言 T1 验收条件"stop 后两表为空"。
- 未做 / 受限：
  - 只验证同进程内单/双路起停，**未做 4 路并发与长稳**（属 M1 T8）；
  - `mk_stop_all_server()` 是进程级"停所有服务"，多端口会误伤 → **T4 `MediaServer` 必须收敛**（已在代码注释标注）；
  - 端口释放是**异步**的（监听 socket 由 ZLM 的 `TcpServer` 析构在 poller 线程关闭）：同一端口 stop 后立刻 restart 会 `bind: address already in use`，脚本做了 ≤3s 退避重试（10 轮共重试 8 次）。4 路共用 8550 时 server 只起一次，不受影响；但 T4 若按端口重建服务需考虑此约束。
  - `scripts/tests/` 依赖 `ss`（观察端口）与 `install/rk3588`（ZLMediaKit 头/库），x86 未验证。

### 5.6 L2 · T2 新增 `NodeConfigManager`（v1 解析 / 校验 / v0 适配 / 引用展开）（2026-11-08）

- 代码基线：工作树（`rk3588`，基于 `e9a620c`）；新增 `src/common/configmgr/{node_config.h,node_config_manager.h,node_config_manager.cpp}`，`src/common/CMakeLists.txt` 注册源文件与头路径；新增 `scripts/tests/{run_t2_node_config.sh,t2_node_config.cpp,fixtures/*.json}`（7 份样例配置）
- 命令：
  ```bash
  cmake -S . -B build && cmake --build build -j"$(nproc)"   # 全量 EXIT=0（含 testnode）
  ./build/run.sh EdgeVisionService                          # → EdgeVisionService start
  ./scripts/tests/run_t2_node_config.sh                     # 53 项断言
  ```
- 结果摘要：`ALL PASS (fails=0)`，`EXIT=0`：
  - **v1 正常样例**：2 条链路，`name` 缺省 = `type`、节点继承链路 `channel`、`enable:false` 生效、`NodeDesc.path` 记录字段路径（供 T5/T6 报错定位）
  - **引用展开**：整值 `${global.media.rtsp.port}` 展开为 **数字** `18500`（不是字符串 `"18500"`）；`global.media.rtsp.{port,app}` 在节点未写时自动注入
  - **拒绝路径 7 条，错误信息均带字段路径**：缺 `type`（`pipelines[0].nodes[0].type`）、`channel` 重复（`pipelines[1].channel`）、节点 `port` 与 global 冲突（`pipelines[0].nodes[1].config.port`）、`fps=0`（`...config.fps`）、同链路内节点重名导致队列名撞车（`pipelines[0].nodes[1].name`，"队列名 = name + channel"）、JSON 语法错误（带行列）、文件不存在
  - **快照语义**：连续 7 次加载失败后仍保留上一次成功的快照（2 条链路 / 端口 18500），失败不污染已生效配置
  - **v0 适配**：`config/push.json` → 1 条链路 3 节点（`vi` → `push(name=rtsp)` → `print`，与今天 `main_node.cpp` 装配一致）；池参数 50 / 20（与现状代码默认值一致）
  - **`load_with_fallback()`**：`node.json` 与 `push.json` 同时存在时优先 `node.json` 并打印"已忽略 push.json"；显式传路径时只认它、不回退
  - **真实 `config/node.json`**：4 条链路，`blocks=64`、`queue_depth=8`、`quota=10`、端口 8550、images 1 项
- 结论：✅ 通过。T2 验收条件（"v1 正常 / 缺 type / 重名 / 端口冲突 4 份样例正确接受与拒绝、错误信息带字段路径"）已全部覆盖并扩大为 7 条拒绝路径 + 3 组正向用例。
- 未做 / 受限：
  - **尚未接入 `main_node.cpp`**（按计划 T6 才替换装配），故"配置真的驱动起链路"未经运行验证 → 由 T5/T6/T7 验收；
  - 未知 `type` 本层**只记录不报错**（`vi/vpss/push/print` 之外一律放行），合法性由 T5 `NodeFactory` 负责（职责分离）；
  - 端口只校验"节点 `port` 必须等于 `global.media.rtsp.port`"；`app/stream` 重复、两路抢同一设备属 T15；
  - 数值上限（宽高 ≤8192 且偶数、`bitrate ≤200000`、`gop ≤10000`、`interval ≤100000`）经代码走查确认，未逐项构造用例（`fps` 上下界已有用例）；
  - `v0` 适配只覆盖当前 `push.json` 的字段集合；若 `push.json` 增加新字段，需同步 `ParseV0`。

### 5.7 L2 · T3 `config/node.json` 定稿（4 路：1 摄像头 + 3 路 mp4）（2026-11-08）

- 代码基线：工作树（`rk3588`，基于 `e0ce88f`，T2 的 `NodeConfigManager` 可用）；本任务**只改配置与测试脚本，未改 C++ 源码**
- 命令：
  ```bash
  python3 -m json.tool config/node.json > /dev/null   # JSON 语法
  ./scripts/tests/run_t3_node_json.sh                 # 104 项断言（含 ffprobe 交叉校验）
  ./build/run.sh EdgeVisionService                    # 冒烟
  ```
- 结果摘要：
  - **素材实测**（`3rdparty/rk3588/ffmpeg/bin/ffprobe`）：`build/file/test1~3.mp4` = hevc **1024×576 @23.976fps**，8888s / 213098 帧；脚本四舍五入取 24 作为交叉校验期望值
  - **配置按实测改定**：ch1~ch3 的 `vi`/`push` 宽高 `1280×720 → 1024×576`（`FfmpegCapture::transfer_frame` 不缩放，尺寸不符必花屏）、`fps 30 → 24`（`PushNode` 用 `1000000/fps` 推算流时间戳，配 30 会使时间戳比真实采集节奏快 20%，播放端表现加速/抖动）；`vi.src` 补 `file:` 前缀（原为本地改的无前缀绝对路径，不符 DESIGN §5.4 前缀规则）；ch0 保持 `dev:/dev/video0` + 1920×1080@30 与码率 4096 不变
  - **脚本结果**：`==== T3 node.json 自检结果: passes=104 fails=0 skips=0 ====`，`SCRIPT_EXIT=0`。六组断言：A 加载/global 段（4 链、port 8550、app live、blocks 64、queue_depth 8、quota 10、images 1×nv12 1920×1080）；B 链路结构（ch0 4 节点含 vpss 且 `enable=false`、ch1~3 各 3 节点、name/channel/enable）；C `vi.src` 前缀与 `loop`（ch0 `dev:` 不写 loop、ch1~3 `file:` 且 `loop=true`）；D vi↔push 参数一致 + push 段 `port`/`app` 等于 global（原文里 push 不写 port/app）+ 队列名全局唯一（13 个）；E 与 ffprobe 实测比对；F 与 v0 `config/push.json` 逐字段等价
  - **素材缺失分支**：把 3 个 `src` 指向不存在文件后再跑 → `passes=95 fails=0 skips=1`，`EXIT=0`（E 段打印 SKIP，其余断言照常）
  - **与 v0 等价**：ch0 的 `push` / `print` 段 14 个字段与 `config/push.json` 完全一致（`port` / `app` 由 `global` 注入后也相等），即「v1 的 ch0 = 今天的单链路」
  - 冒烟：`./build/run.sh EdgeVisionService` → `EdgeVisionService start`
- 结论：✅ 通过（**配置层**）。T3 验收条件「4 路结构正确 + 用 v1 跑起来与 v0 单路现象一致」中，结构部分已全量断言，等价性以字段级断言替代，运行部分受限见下。
- 未做 / 受限：
  - **"真的跑起来"未验证**：`main_node.cpp` 仍只装配 v0 单链路（T6 才换 `PipelineBuilder`），`ViNode::run()` 仍硬编码 `CAPTURE_V4L2` + `/dev/video0:640:480:NV12:30`（T7 才解析 `vi.src`），`FfmpegCapture` 仍不 loop（T7b）→ 「4 路同时出流 + `rtsp://<ip>:8550/live/ch0..ch3`」的端到端验证必须等 T6/T7b 之后
  - `global.pool.{blocks,queue_depth,quota_per_channel}` 仍是"配置里写了但无人读"（T10a）
  - `vi.loop: true` 当前无实现（T7b）；ch1~ch3 要播到 8888s 末尾才会暴露问题
  - 素材是**本机绝对路径**（`/mnt/nvme/workspace/EdgeVisionService/build/file/testN.mp4`），`build/` 不入库 → 换机器 / 重 clone 需自备同名文件或改路径（已写入配置 `_comment`）
  - 三路素材是同一文件的副本，画面无差异，肉眼无法区分通道 → 排查串流问题只能靠 `stream` 名与日志

### 5.8 L2 · T5 `NodeFactory` + `PipelineBuilder`（配置驱动的装配与拓扑校验）（2026-11-08）

- 代码基线：工作树（`rk3588`，基于 `0635ad1`；本次提交为 `b461ece`，其后的纯 `docs:` 提交不改变本记录）
- 改动：新增 `src/pipeline/node_factory.{h,cpp}`（type→节点单例注册表，内置 vi/vpss/push/print）与 `src/pipeline/pipeline_builder.{h,cpp}`（拓扑校验 + 装配 + 连线 + 批量启停 + 计数/错误），`src/pipeline/CMakeLists.txt` 加 2 个源文件并 `PUBLIC` 链 `common_api`（builder 读 `NodeConfigManager`，需符号 + `configmgr` 头路径）；`src/main/test/CMakeLists.txt` 新增 `test_builder` target；新增 `scripts/tests/t5_builder.cpp`（64 断言）与 `scripts/tests/run_t5_builder.sh`
- 命令：
  ```bash
  ./scripts/tests/run_t5_builder.sh      # generate + 编译 test_builder + 运行
  ./scripts/tests/run_t3_node_json.sh    # 回归
  ./scripts/tests/run_t2_node_config.sh  # 回归
  cmake --build build -j"$(nproc)"       # 全量
  ./build/run.sh EdgeVisionService       # 冒烟
  ```
- 结果摘要：
  - `==== T5 builder 自检结果: passes=64 fails=0 skips=2 ====`，`EXIT=0`。八组：A `NodeFactory` 14 项（未知 type 返回 `nullptr` 并报出已注册类型、重复注册被拒且不覆盖内置、空 type / 空 creator 被拒）；B `check_order` 拓扑规则 10 项（R1 空链 / 单节点、R2 首节点非 vi、R3 末节点非 push|print、R4 vi 非首位、R5 print 非末位，错误均带 `pipelines[i].nodes[j]` 字段路径）；C `build_one` 装配 13 项（节点数 / 全名 / channel 透传 / 正反向连线 / 失败不留半条链）；D 调用顺序与配置透传 6 项（`create` → `set_data` 各一次，收到 T2 包装后的配置原文）；E 真实 `config/node.json` `build_all` 10 项；F 单链失败隔离（N3）8 项；G 启停边界 3 项 + 2 SKIP；H 同配置重复 build 2 项
  - **真实配置装配**：`ch0[0] vi0 -> vpss0 -> rtsp0 -> print0`、`ch1[1] vi1 -> rtsp1 -> print1`、`ch2` / `ch3` 各 3 节点，`[builder] 汇总: 建成 4 条, 失败 0 条, 跳过 0 条`；连线数 = Σ(节点数−1)
  - **N3 部分失败**：1 条合法 + 1 条未知 type（`vpsss`）+ 1 条无末端节点 → `建成 1 条, 失败 2 条, 跳过 0 条`，两条失败各留一条带字段路径的错误，合法链路照常建成
  - 回归：T3 `passes=104 fails=0 skips=0`、T2 `ALL PASS (fails=0)`，两者 `EXIT=0`；全量 `cmake --build` 通过，`run.sh EdgeVisionService` → `EdgeVisionService start`
- 结论：✅ 通过（**装配层**）。`config/node.json` 现在能直接驱动出 4 条已连线的链路对象；拓扑错误在 build 阶段拦截，单链失败不牵连其它链。
- 未做 / 受限：
  - **只 build，不 start**：`start_all` / `stop_all` 的顺序（终点先起、源头先停）会真拉 `/dev/video0` 并占 RTSP 端口，属端到端，留 T6/T7 板端验（G2 / G3 两个 SKIP 即此项）
  - `main_node.cpp` 仍是 v0 手写单链路，**未接入 builder**（T6）
  - `ViNode` 仍硬编码 `CAPTURE_V4L2` + `/dev/video0:640:480:NV12:30`，不解析 `vi.src`（T7）；`FfmpegCapture` 不 loop（T7b）
  - `global.pool.{blocks,queue_depth,quota_per_channel}` 仍无人读（T10a）

### 5.9 L2 · T6 `main_node.cpp` 改为「配置 → 进程资源 → builder → SIGINT 优雅退出」（2026-11-08）

- 代码基线：工作树（`rk3588`，基于 `b461ece`）
- 改动：`src/main/test/main_node.cpp` 重写（116 行）。删除 v0 手写装配（3 个 `make_shared` + 2 次 `link_to` + `load_push_config()` 候选路径逻辑）、`while(1)` 死循环与**永不执行**的 `stop()`；改为 `NodeConfigManager::load_with_fallback(argv[1])` → `MemManager::MemInit()` → `PipelineBuilder::build_all()`（单链失败只告警）→ `std::signal(SIGINT/SIGTERM)` → `start_all()` → `while (g_run) sleep 200ms` → `stop_all()` → `sets.clear()` → `FreeMem()` → `return 0`。信号处理只写 `volatile sig_atomic_t g_run`（异步信号安全）。入口不再出现任何节点名/设备路径（M1 验收第 7 条）
- 命令：
  ```bash
  cmake --build build -j"$(nproc)"                                   # 全量编译
  timeout --preserve-status -s INT 12 ./build/run.sh testnode /abs/config/push.json   # v0 单链路 + 优雅退出
  timeout --preserve-status -s INT 14 ./build/run.sh testnode /abs/config/node.json   # v1 四路 + 优雅退出
  ./scripts/tests/run_t5_builder.sh ; ./scripts/tests/run_t3_node_json.sh ; ./scripts/tests/run_t2_node_config.sh
  ./build/run.sh EdgeVisionService                                   # 冒烟
  pgrep -af testnode                                                 # 残留检查
  ```
- 结果摘要：
  - **v0 单链路（真拉 `/dev/video0` 12 秒）**：`[builder] 汇总: 建成 1 条, 失败 0 条, 跳过 0 条` → `successfully open stream:/dev/video0:640:480:NV12:30, output=1920x1080` → `[print0] frame#330 … 实测=29.85fps`（全程稳定 29.8–29.95fps）→ SIGINT → `[main] 收到退出信号，开始有序退出` → `[vi0] VI Node Stopped.` → ZLM 媒体注销（`fmp4/rtmp/rtsp/ts` 四条 schema）+ `Close tcp server [::]: 8550` → `[rtsp0] Push Node Stopped.` → `[print0] Print Node Stopped.` → `[main] FreeMem 完成，进程正常退出` → **`EXIT=0`**，`pgrep` 无残留
  - **停止顺序证据**：`vi0 → rtsp0 → print0`（正序，源头先停），与启动日志的逆序 `print0 → rtsp0 → vi0` 相反 —— T5 的 `start_all`/`stop_all` 顺序在真实节点上成立（**关闭了 T5 的 G2/G3 两个 SKIP 的一半：顺序已实测，`MediaServer::shutdown` 那一跳等 T4**）
  - **v1 四路**：`建成 4 条, 失败 0 条, 跳过 0 条` → 4 条链全部 `start_all` 成功（`[main] 4 条链路已启动`）→ SIGINT → 逐链正序停止（ch0：`vi0 → vpss0 → rtsp0 → print0`，ch1/2/3：`viN → rtspN → printN`）→ `FreeMem 完成` → `EXIT=0`，无残留
  - **已知限制按预期暴露**：ch1~ch3 三路 `open file failed:/dev/video0:640:480:NV12:30`（4 条 `vi` 都硬编码同一设备）→ 属 P0-2 / T7 范围，**不是 T6 引入的回归**（旧 main 同样硬编码，只是旧 main 只有一路）
  - 回归：T5 `passes=64 fails=0 skips=2`、T3 `passes=104 fails=0 skips=0`、T2 `ALL PASS (fails=0)`，三者 `EXIT=0`；全量 `cmake --build` 通过；`run.sh EdgeVisionService` → `EdgeVisionService start`；`git status --short` 仅 `M src/main/test/main_node.cpp`
- 结论：✅ 通过（**进程编排层**）。T6 验收三条全部实测：Ctrl+C（等效 `SIGINT`）后 ① `stop_all` 执行（有逐节点 `Stopped` 日志）② `FreeMem` 执行（有 `[main] FreeMem 完成`）③ 进程 `EXIT=0`。`config/node.json` 现在**单独**决定跑几条链路，加一路只改配置。
- 未做 / 受限：
  - **4 路同时出帧未验证**：`ViNode::run()` 仍硬编码 `CAPTURE_V4L2` + `/dev/video0:640:480:NV12:30`（P0-2 / T7），ch1~ch3 必然打不开设备；同时 `ViNode` 只读 v0 的 `push` 段，`vi.width/height` 未生效（日志恒为 `1920x1080 fps=30`）
  - **`MediaServer::shutdown()` 那一跳不存在**：T4（媒体服务上提）未做，ZLM 仍由 `PushNode::stop()` 内部 `zlm_rtsp_stop()` 关闭，效果等价但归属未变；T4 完成后退出序列要补 `MediaServer::shutdown()`（DESIGN §5.5 第 5 步）
  - `MemInit()` 仍用默认 50 块（`global.pool.*` 未接线，T10a）；本次日志里 `[main] 注意: pool.blocks … 尚未接线（T10a）` 即为此提示
  - 真终端 Ctrl+C 与 `timeout -s INT` 等效（`run.sh` 末行是 `exec`，SIGINT 直达二进制），未另做终端手工验证

### 5.10 L2 · T7 `ViNode` 按 `vi.src` 前缀选采集实现 + 修 P0-2（2026-11-09）

- 代码基线：工作树（`rk3588`，基于 `33d6690`；T6 代码校准点 `29ab58b`）
- 改动：`src/pipeline/moudle_vi/moudle_vi.{h,cpp}`
  - 新增匿名命名空间 `ResolveSrc()`：按**前缀**判采集实现（`dev:`→V4L2；`file:` / `rtsp://` / `rtmp://` / `http://` / `udp://`→FFmpeg；无前缀按老规则兜底：`/dev/video*` 当 V4L2，其余当本地文件）。`dev:` 只写设备路径时补成 V4L2 契约的 5 段 `设备:宽:高:格式:fps`（默认 `:640:480:NV12:<vi.fps>`）；写了一半（如 `dev:/dev/video0:640`）直接报错，不猜
  - `set_data()`：优先读 `vi` 段、回落到 `push` 段，支持对象或单元素数组；`src` 解析失败只告警，**不静默降级**到默认源
  - `run()`：改用 `CreateCapture(m_capture_type)`；`m_capture == nullptr` 时报错并退出本节点线程（**P0-2**：不再空指针解引用）；`open()` 失败分支补 `m_queue.release(block)`（§2.4 取块必有归还）
  - 兼容性：`vi.src` 缺省（v0 `push.json` 合成的 vi 段没有 src）→ 仍走 `/dev/video0:640:480:NV12:30`，行为与 T7 之前一致
- 命令：
  ```bash
  cmake --build build -j"$(nproc)"
  timeout --preserve-status -s INT 12 ./build/run.sh testnode /abs/config/push.json           # v0 不变性
  timeout --preserve-status -s INT 14 ./build/run.sh testnode /abs/config/node.json            # v1 四路（1 V4L2 + 3 FFmpeg）
  timeout -k 5 --preserve-status -s INT 8 stdbuf -oL ./build/run.sh testnode /tmp/t7_fail_dev.json   # 设备不存在
  timeout -k 5 --preserve-status -s INT 8 stdbuf -oL ./build/run.sh testnode /tmp/t7_fail_file.json  # 文件不存在
  ./scripts/tests/run_t5_builder.sh ; ./scripts/tests/run_t3_node_json.sh ; ./scripts/tests/run_t2_node_config.sh
  ./build/run.sh EdgeVisionService ; pgrep -af testnode ; ss -lntp | grep 8550
  ```
  （失败场景配置由 `config/node.json` 的 ch0 派生：只保留 `pipelines[0]`，把 `nodes[0].config.src` 分别换成 `dev:/dev/no_such_video:640:480:NV12:30` 与 `file:/tmp/no_such_clip.mp4`，生成到 `/tmp/t7_fail_{dev,file}.json`）
- 结果摘要：
  - **v0 不变性**：`successfully open stream:/dev/video0:640:480:NV12:30, output=1920x1080`，`实测=29.63~29.98fps`，SIGINT → `vi0 → rtsp0 → print0` 正序停止 → `FreeMem 完成` → `EXIT=0`
  - **v1 四路出帧（T7 主验收）**：`[ViNode] 采集源: dev:/dev/video0 -> CAPTURE_V4L2 (/dev/video0:640:480:NV12:30)`；ch1~ch3 `file:.../test{1,2,3}.mp4 -> CAPTURE_FFMPEG`；`[builder] 汇总: 建成 4 条, 失败 0 条, 跳过 0 条`；四路 `successfully open stream` 全成功（`vi1/vi2/vi3` 文件源 1024x576、`vi0` 设备源 1920x1080）；报表 `print0=29.96fps`、`print1=23.96fps`、`print2=23.93fps`、`print3=23.94fps`（与 ch1~ch3 配置 24fps 一致）；SIGINT → 四链正序停止 → `FreeMem 完成` → `EXIT=0`，`pgrep` 无残留、8550 已释放
  - **P0-2 同类路径（设备 / 文件打不开）**：`open v4l2 failed: No such file or directory` → `[vi0] open failed: /dev/no_such_video:640:480:NV12:30` → 节点线程退出（**无段错误**）→ SIGINT → `vi0 → vpss0 → rtsp0 → print0` 全部 `Stopped` → `FreeMem 完成` → `A_EXIT=0`；文件源同形（`[FfmpegCapture] Opening Local File: /tmp/no_such_clip.mp4` → `[vi0] open failed` → `B_EXIT=0`）
  - 回归：T5 `passes=64 fails=0 skips=2`、T3 `passes=104 fails=0 skips=0`、T2 `ALL PASS (fails=0)`，三者 `EXIT=0`；全量 `cmake --build` → `BUILD_EXIT=0`；`run.sh EdgeVisionService` → `EdgeVisionService start`；`git status --short` 仅 `M src/pipeline/moudle_vi/moudle_vi.cpp` + `M ...h`
- 结论：✅ 通过。T7 验收（`vi.src` 前缀 → 采集实现选择）与 **P0-2**（`nullptr` 采集的必现崩溃）均实测关闭；**四路同时出帧首次成立**（M1 验收第 1 条）。
- 未做 / 受限：
  - `FfmpegCapture` **不 loop**（T7b）：mp4 播完即断流，四路「30 分钟不 EOF 重播」未验证；文件源 `vi.width/height` 必须与文件实际分辨率一致（采集层不缩放）
  - `!m_capture` 分支（工厂返回 `nullptr`）在 RK 平台**不可达**（V4L2 后端已编入），本次以代码审查 + 上述 `open()` 失败路径动态覆盖；x86 侧复测需等 P0-1
  - `[vi0] open failed` 后该链路静默空转（下游无帧、RTSP 无输出），是否要「失败重试 / 拉黑该路」未定 → 归入 T10a 之后的稳定性任务
- 复测（2026-11-09，T7 提交 `eac4bb0` + 两次纯文档提交之后的 HEAD 复验）：
  ```bash
  cmake --build build -j"$(nproc)"                                                    # 增量：100% Built target（无编译错误/警告）
  timeout --preserve-status -s INT 14 ./build/run.sh testnode ../config/node.json      # 必须 ../config：run.sh 会切到 build/，显式路径不回退
  ./build/run.sh EdgeVisionService
  ```
  - `RUN_EXIT=0`；四路 `successfully open stream` 全成功（`vi0` 设备 1920x1080、`vi1~vi3` 文件 1024x576）；`[ViNode] 采集源:` 四行与配置一致
  - 报表：`print0=29.65~29.97fps`、`print1=23.90~23.96fps`、`print2=23.93~23.95fps`、`print3=23.90~23.97fps`
  - SIGINT → 逐链正序停止（`vi0 → vpss0 → rtsp0 → print0` → `vi1 → rtsp1 → print1` → …）→ `[main] FreeMem 完成，进程正常退出`
  - `pgrep -af testnode` → 无残留；`ss -lntp | grep 8550` → 空（端口已释放）；冒烟 `EdgeVisionService start`
  - 备注（用法坑，非缺陷）：`./build/run.sh testnode config/node.json` 少了 `../` 会得到 `[configmgr] 配置被拒绝 -> 找不到配置：config/node.json 不存在或内容为空（显式路径不回退）` 且 `EXIT=1` —— `run.sh` 切到 `build/` 后显式路径不回退是 T6 的有意设计

---

### 5.11 L2 · 修 P0-13：`MppEncoder` 按平面拷贝 NV12（mp4 路 UV 绿斑/花屏）（2026-11-09）

- 代码基线：工作树（`rk3588`，基于 `75eea5b`；代码校准点 `eac4bb0`）
- 现象：T7 打通四路（ch0 = `dev:` 1920×1080，ch1~ch3 = `file:` 1024×576）后，**每路推流画面的 Y 正常、UV 出现大块绿斑 / 花屏**
- 定位（实测数据，非推断）：临时在 `MppEncoder::encode()` 打印每帧尺寸，得到
  - ch1（1024×576）：`w=1024 h=576 st0=1024 st1=1024 dsz=884736 cap=6220800 d1off=2073600`，即 **`data[1]` 相对 `data[0]` 偏移 = 2073600 = 1920×1080（池块分辨率）**，而不是本帧的 1024×576 = 589824
  - ch0（1920×1080）：`d1off=2073600`，恰好等于 `1920×1080`，所以连续拷贝正确 —— 这解释了「为什么只有 mp4 路花屏」
- 根因（**P0-13**，既有缺陷，被 T7 暴露）：`MppEncoder::encode()` 用
  `memcpy(dst, fb->data[0], fb->data_size)` 一次性把 `data_size` 字节拷进 MPP buffer，
  而 `FrameBuffer::BindDataPointers()` 里 `data[1] = base + stride[0] * 池块高`，
  **UV 平面并不紧跟在「本帧 Y 平面」之后**。当本帧比池块小时，被当作 UV 拷进去的是
  本帧 Y 之后的内存残影（上一帧 / 其它链路留下的亮度数据）→ 播放端 Y 正常、UV 花屏
- 改动（`src/platform/encoder/rk_mpp_encoder.cpp`，+26/−1）：
  - Y、UV **分别拷贝**：`const size_t y_size = frame_size / 3 * 2;`
    `memcpy(dst_ptr, fb->data[0], y_size);`
    `memcpy((uint8_t*)dst_ptr + y_size, fb->data[1], frame_size - y_size);`
    （NV12 中 Y 占 2/3、UV 占 1/3，UV 在 MPP buffer 里紧跟 Y，偏移 = `hor_stride * ver_stride`）
  - `frame_size == 0` 时按 `stride * height * 3 / 2` 兜底；`data[0] / data[1]` 空指针时归还 buffer 并返回 false
  - 其余保持原写法（`mpp_frame_set_hor_stride(frame, fb->stride[0])`、`mpp_buffer_get(size = fb->data_size)`）
- 命令：
  ```bash
  cmake --build build -j"$(nproc)"                                     # BUILD_EXIT=0
  timeout --preserve-status -s INT 45 ./build/run.sh testnode ../config/node.json
  ./3rdparty/rk3588/ffmpeg/bin/ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:8550/live/ch1 \
      -frames:v 5 -f rawvideo -pix_fmt yuv420p /tmp/fin_ch1.yuv -y      # ch0/ch2/ch3 同
  ./scripts/tests/run_t5_builder.sh ; ./scripts/tests/run_t3_node_json.sh ; ./scripts/tests/run_t2_node_config.sh
  ```
  UV 判据：拉流解码成 yuv420p 后统计 U/V 均值与标准差，与**源 mp4 同分辨率解码帧**对比（UV 正确时两者应接近）
- 结果摘要：
  - **修复前**（`U/V` 均值±标准差，1024×576）：RTSP ch1 `U=105.4±41.1 / V=105.5±41.2`；源 `test1.mp4` `U=126.6±11.0 / V=128.2±1.7` → 均值偏离 + 方差 4 倍，典型 UV 花屏
  - **修复后**：ch1/ch2/ch3 `U=128.1±10.9 / V=128.0±1.6` —— 与源吻合；ch0（1920×1080）`U=70.5±66.1 / V=68.7±64.3`，与修复前（`69.7±65.3 / 69.0±64.8`）一致，**无回归**
  - 四路 fps：`print0=29.82`、`print1=23.94`、`print2=23.93`、`print3=23.96`（与配置 30/24 一致）；`hal_h265e_v580_ret_task` 编码错误 **0** 次；SIGINT → 正序停止 → `FreeMem 完成` → `EXIT=0`
  - 隔离对照（用的配置副本放 `/tmp`，不动仓库）：
    - 单路 ch1（1024×576）→ 0 错误、23.93fps；单路 ch0（1920×1080）→ 0 错误、29.84fps
    - 基线代码（未修 P0-13）四路 → 0 错误、ch0 29.87fps（说明本次改动**不增加** 1080p 路负载）
  - 回归：T5 `passes=64 fails=0 skips=2`、T3 `passes=104 fails=0 skips=0`、T2 `ALL PASS (fails=0)`
- 结论：✅ 通过。P0-13（UV 平面错位）实测关闭，四路画面 UV 与源一致，1080p 路零回归
- 排障坑（与缺陷无关，但会误导判断，记下来省下次的时间）：
  1. **不要用 `pkill -9` 强杀 `testnode`**：rkvenc 硬件来不及释放，`dmesg` 会出现
     `mpp_rkvenc2 ... found error status 00000100` / `soft reset timeout, use cru reset`，
     之后的运行可能持续报 `hal_h265e_v580_ret_task enc not done`
  2. **不要同时跑两个 `testnode`**：8550 端口与 MPP 硬件都会冲突，第二个实例可能直接 `EXIT=135`（SIGBUS）
  3. 判断「是否回归」必须先跑一次基线对照（同一环境、同一轮次），否则容易把环境污染当成代码缺陷
- 未做 / 受限：
  - 未在播放器里逐帧目视（本环境无 GUI），以 yuv420p 的 U/V 统计作为客观判据；建议后续在带显示的机器上抽检一次
  - `FfmpegCapture` 仍不缩放、不 loop（T7b）；`vi.width/height` 必须与素材实际分辨率一致，否则画面会被拉伸/裁切（与本次 UV 问题无关）
  - 池块按最大分辨率（1920×1080）统一分配，720p 以下链路仍浪费约 56% 内存（T10a）

---

## 6. 维护约定

1. 提交代码时，同一 commit 里**必须**包含对应的 TESTLOG 记录（L1 或 L2）。
2. 本文只写**测试证据**；方案写 `DESIGN_MULTI_PIPELINE.md`，现状写 `STATUS.md`，规矩写 `HANDOFF.md`（分工见 HANDOFF R5）。
3. 本文超过 ~400 行时，把最早的 L1 行折叠成一行汇总（"2026-11 共 N 次构建通过"），L2/L3 明细保留。
4. 若某条记录后来被推翻（例如复测失败），**不改旧记录**，追加新记录并注明「推翻 5.x」。
