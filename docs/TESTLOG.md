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

---

## 6. 维护约定

1. 提交代码时，同一 commit 里**必须**包含对应的 TESTLOG 记录（L1 或 L2）。
2. 本文只写**测试证据**；方案写 `DESIGN_MULTI_PIPELINE.md`，现状写 `STATUS.md`，规矩写 `HANDOFF.md`（分工见 HANDOFF R5）。
3. 本文超过 ~400 行时，把最早的 L1 行折叠成一行汇总（"2026-11 共 N 次构建通过"），L2/L3 明细保留。
4. 若某条记录后来被推翻（例如复测失败），**不改旧记录**，追加新记录并注明「推翻 5.x」。
