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
| 2026-11-08 | 内存池归属方案（纯文档 + `node.json` 增补字段） | `python3 -m json.tool config/node.json` → `JSON_OK`（239 行）；`git diff --stat` 仅 `config/node.json`(+5/-1) 与 `docs/design/DESIGN_MULTI_PIPELINE.md`(+102)；**未改 C++ 源码，未重新编译** | ✅ 文档/配置一致（`quota_per_channel` 尚未被二进制读取，随 T10a 生效） |
| 2026-11-08 | 池方案决策 **D9 = A**（纯文档） | `python3 -m json.tool config/node.json` → `JSON_OK`；`D9` 已出现在 DESIGN §1.4 决策表 / §9 第 6 项 / §11.6.2 / §11.6.4 / §11.6.6 / §11.6.7 / §12 变更记录（口径一致：共享池 + 每路配额，A+ 不实施）；**未改 C++，未重新编译** | ✅ 决策沉淀完成（T10a 范围与人日同步为 1.5；`node.json` 注释标注 D9） |
| 2026-11-08 | **T1** 修 P0-6 / P0-7 | `cmake --build build --target rtsp -j8` → `EXIT=0`；`cmake --build build -j8` 全量 → `EXIT=0`（含 `testnode`）；`./build/run.sh EdgeVisionService` → `EdgeVisionService start`；`./scripts/tests/run_t1_zlm_rtsp_key.sh` → `ALL PASS (fails=0)`，18 项断言（见 5.5） | ✅ 代码 + 冒烟 + 验收断言全通过 |
| 2026-11-08 | **T2** `NodeConfigManager`（v1 解析/校验/v0 适配） | `cmake -S . -B build && cmake --build build -j8` → `EXIT=0`；`./build/run.sh EdgeVisionService` → `EdgeVisionService start`；`./scripts/tests/run_t2_node_config.sh` → `ALL PASS (fails=0)`，53 项断言（见 5.6） | ✅ 代码 + 冒烟 + 验收断言全通过 |
| 2026-11-08 | **T3** `config/node.json` 定稿（4 路） | `./scripts/tests/run_t3_node_json.sh` → `passes=104 fails=0 skips=0`，`EXIT=0`；素材缺失分支 `passes=95 skips=1`；`./build/run.sh EdgeVisionService` → `EdgeVisionService start`（见 5.7） | ✅ 配置层通过（端到端待 T6/T7b） |
| 2026-11-08 | **T5** `NodeFactory` + `PipelineBuilder` | `./scripts/tests/run_t5_builder.sh` → `passes=64 fails=0 skips=2`，`EXIT=0`；真实 `node.json` 建成 4 条链；T3 `passes=104`、T2 `ALL PASS` 回归通过；全量 `cmake --build` 通过 + `run.sh EdgeVisionService` → start（见 5.8） | ✅ 装配层通过（`start_all` 顺序待 T6/T7 板端验） |
| 2026-11-08 | **T6** `main_node.cpp` 改为 builder 驱动 + SIGINT 优雅退出 | `cmake --build build -j8` → `EXIT=0`；`timeout --preserve-status -s INT 12 ./build/run.sh testnode config/push.json` → `EXIT=0`（`vi0 → rtsp0 → print0` 正序停止、`FreeMem 完成`、无残留进程）；同命令跑 `config/node.json` → 4 条链建成/启动/正序停止、`EXIT=0`；T5 `passes=64`、T3 `passes=104`、T2 `ALL PASS`；冒烟 `run.sh EdgeVisionService` → start（见 5.9） | ✅ 进程编排层通过（4 路**出帧**待 T7/T7b：`ViNode` 仍硬编码同一 `/dev/video0`） |
| 2026-11-09 | README 重写（文档 + 命令实测） | `./build/run.sh testnode`（无参）→ `[builder] 汇总: 建成 4 条, 失败 0 条, 跳过 0 条`，`V1_EXIT=0`（证实无参时命中 `../config/node.json`）；`./build/run.sh testnode ../config/push.json` → `建成 1 条`，`V0_EXIT=0`；README 代码围栏 16 个（成对闭合）、8 个相对链接目标全部存在；改 `config/node.json` 注释后 `python3 -m json.tool` → `JSON_OK`、`run_t3_node_json.sh` → `passes=104 fails=0 skips=0` | ✅ 文档里写出的运行命令与输出均实测；无残留进程 |
| 2026-11-09 | **T7** `ViNode` 解析 `vi.src` 前缀选采集后端 + 修 P0-2 | `cmake --build build -j8` → `BUILD_EXIT=0`；`testnode config/push.json` → 源仍是 `/dev/video0:640:480:NV12:30`（v0 不变）、`EXIT=0`；`testnode config/node.json` → `dev:`→V4L2、3 路 `file:`→FFmpeg 全部 open 成功，四路 29.96 / 23.96 / 23.93 / 23.94fps，`EXIT=0`；设备/文件不存在的失败场景 → `[vi0] open failed` + 有序停止 + `EXIT=0`（无段错误）；T5/T3/T2 回归全通过 + 冒烟 start（见 5.10） | ✅ 通过（**四路同时出帧成立**；mp4 不 loop 待 T7b） |



| 2026-11-09 | **P0-13** NV12 UV 平面错位（mp4 路 Y 正常、UV 绿斑/花屏） | `cmake --build build -j8` → `BUILD_EXIT=0`；四路 `testnode ../config/node.json` → `EXIT=0`、`hal_h265e_v580_ret_task` 报错 **0** 次、29.82 / 23.94 / 23.93 / 23.96fps；拉流解码统计 `U=128.1±10.9 / V=128.0±1.6`（修复前 `105.4±41.1 / 105.5±41.2`，源文件 `126.6±11.0 / 128.2±1.7`）；ch0（1920×1080）`U=70.5±66.1 / V=68.7±64.3` 与修复前一致（无回归）；T5 `passes=64` / T3 `passes=104` / T2 `ALL PASS`（见 5.11） | ✅ 通过（UV 与源吻合，1080p 路零回归） |
| 2026-11-09 | **P0-14** 池块 `FrameBuffer` 视图被跨链路改写（四路下 1080p 路下半屏全绿） | `cmake --build build -j8` → `BUILD_EXIT=0`；四路 `testnode ../config/node.json` → 四路照常出帧；拉流解码统计 ch0（1920×1080）**坏帧 0/166**（修复前四路 51/64、两路 63/129）、ch1（1024×576）**坏帧 0/73**；T5 `passes=64` / T3 `passes=104` / T2 `ALL PASS`；冒烟 `run.sh EdgeVisionService` → `EdgeVisionService start`（见 5.12） | ✅ 通过（下半屏绿关闭，1080p/576p 两路均无坏帧） |
| 2026-11-10 | **P0-9** `MemInit()`/`FreeMem()` 泄漏（`FrameBuffer` 堆内存从不释放） | `cmake --build build -j"$(nproc)"` → `BUILD_RC=0`；仓库外探针 `/tmp/mem_probe` 连跑 6 轮 `MemInit(2)+FreeMem()`：修复前每轮净增 12,443,648 B 且永不归还，修复后 init 12.52 MB → free 回落 82 kB、6 轮零累积（当时残余 128 B/轮 `MemSlot`，已由 P0-11 收口）；四路 `testnode` SIGINT → `FreeMem 完成` + `exit_code=0`（见 5.13） | ✅ 通过（帧内存逐轮完整回收） |
| 2026-11-10 | **P0-11** `MemMeta` 槽位从不销毁 + `vpss_out` 槽位漏收 | `cmake --build build -j8` → `BUILD_RC=0`；仓库外探针 `/tmp/meta_probe` → 2 槽位 12.52 MB、覆盖同名不多留帧、`clear() released=2` 回落 78 kB、同一指针重复登记不双删；`/tmp/mem_probe 2 6` → `after free` 6 轮**恒定 82,080 B**（修复前每轮 +128 B）；四路 `testnode` SIGINT → `FreeMem: 释放 50 块 / 100 个槽位` + `exit_code=0`（见 5.14） | ✅ 通过（槽位结构与节点自建帧均随 `FreeMem()` 回收） |
| 2026-11-10 | 新增 `docs/ops/COMMANDS.md`（命令手册）+ 5 处入口同步 | 文档里每条命令逐条复跑：`g++ -fsanitize=address -I src/common /tmp/fb_real_probe.cpp` → `CC_RC=0`、`RUN_RC=0`（ASAN 无报错）；`-L build/lib -lcommon_api` 版 `mem_probe 2 3` → 每轮 `after free` 恒定 82,080 B；`meta_probe`（`-O1`）→ `clear() released=2` 回落 78,112 B；禁拷贝反证 `CC_RC=1`（2 处 `use of deleted function`）；反例探针（只加析构不禁拷贝，ASAN）→ `RUN_RC=1` + `attempting double-free`；`timeout --preserve-status -s INT 10 ./build/run.sh testnode $PWD/config/push.json` → `EXIT=0` + `ss -ltnp` 见 `*:8550` + `VmRSS=172708 kB`/`Threads=11` + 无残留；`git check-ignore -v build/bin/testnode` → `.gitignore:2:build/`；`3rdparty/rk3588/ffmpeg/bin/ffprobe -version` → 5.0.3；**未改 C++ / CMake / config，未重新编译**（构建结论仍对 `15cf975` 有效） | ✅ 文档内命令均可复现；另记环境事实：`ffplay`、`valgrind` 未安装 |
| 2026-11-10 | **P0-10 收口**：`FrameBuffer` 改 RAII（`~FrameBuffer(){Release();}` + 删拷贝/赋值）+ 删 `slot_free<FrameBuffer*>` 特化 | `cmake --build build -j8` → `BUILD_RC=0`；`/tmp/fb_real_probe`（真实头文件 + ASAN）→ `delete` 即归还、零错误零泄漏 `REAL_RC=0`，栈视图 no-op；`/tmp/fb_copy_forbidden.cpp` → 2 处 `use of deleted function`（`CC_RC=1`，符合预期）；`/tmp/fb_raii_copy_probe`（只加析构不禁拷贝的反例，ASAN）→ `attempting double-free`；`/tmp/meta_probe` → `clear() released=2` 回落 78,112 B；`/tmp/mem_probe 2 6` → 6 轮恒定 82,080 B；四路 `testnode` SIGINT → `释放 50 块 / 100 个槽位`、无残留进程、`SMOKE3_RC=0`（见 5.15） | ✅ 通过（P0-10 的 ①② 关闭，仅剩 `capacity` 语义） |
| 2026-11-11 | 新增 `docs/interview/INTERVIEW.md`（面试问答手册）+ 5 处入口同步 | 文档自检脚本：`lines=581`、代码围栏 `4`（成对闭合）、TOC `17` 条锚点全部可解析（`bad=[]`）、`58` 处 `文件:行` 引用全部落在真实文件行号范围内（`bad=[]`）、相对链接 `BUG_CASES.md` / `TESTLOG.md` 均存在；**未改 C++ / CMake / config，未重新编译**（构建结论仍对 `15cf975` 有效） | ✅ 纯文档（无代码变更） |
| 2026-11-11 | 帧内存分配切到 MPP DRM 缓冲（`FrameBuffer::Allocate()` 不再 `new[]`） | `cmake -S . -B build && cmake --build build -j"$(nproc)"` → `CONFIG_RC=0` / `BUILD_RC=0`、warning 0、configure 打印 `frame mem: MPP DRM buffer (...)`；仓库外探针 `/tmp/mpp_pool_probe 2 2` → `[FrameMem] backend: MPP DRM buffer group`、组内记账 `12441600 B`（=2×6220800）、`data[1]-data[0]=2073600`（=`stride*h`）、写满 3,110,400 B 首/末字节回读正确、`FreeMem` 后记账归零、`fds 11→11`（两轮 delta 0）；`/tmp/mpp_rss_probe 50` → 记账 296 MB → `FreeMem` 后归零、`mallinfo2.uordblks=241 kB`；四路 `testnode` 拉流解码统计 ch1 `U=128.1±10.9 / V=128.0±1.6`（源 `126.6±10.9 / 128.2±1.7`，与 §5.11 基线一致）、ch0 `U=127.3±7.1 / V=129.8±6.7`、`hal_h265e` 报错 **0** 次、29.18 / 23.94 / 23.94 / 23.78fps、`FreeMem: 释放 50 块 / 100 个槽位`、`EXIT=0`；T5 `passes=64` / T3 `passes=104` / T2 `ALL PASS`（见 5.16） | ✅ 通过（帧内存改走 DMA 缓冲，画面与 fps 零回归；编码前 `memcpy` 未动） |
| 2026-11-11 | 探针实测：池块 MPP DRM 缓冲能否直传 RGA（缩放 / 格式转换 / 裁剪 / 粘贴 / 叠字） | 5 个仓库外探针（`/tmp/rga_probe{,2,3,4,5}.cpp`）`g++ -std=c++14 -O2 -I 3rdparty/rk3588/librga-1.9.1/include ... -lrockchip_mpp -lrga` → 全部 `EXIT=0`；`mpp_buffer_get_fd` + `importbuffer_fd` 通；`imcopy` 两帧 3,110,400 B **不一致数 0**；`imcvtcolor` NV12→RGB888 首/末像素 `(55,55,56)`（BT.601 limited）；`imresize` 1080p→720p 后 Y `min=max=64`；`imcrop`(100,50,640,480) 首/末点 `150/150`、`244/244`；crop+scale 640×360→320×180 通过；paste 64×32 **被拒**，阈值扫描：宽 `64`/`66` 失败、`68` 起成功（高度无下限，16 高可过）；`imblend`/`improcess` 混合可提交但像素语义不符（`Y(0,0)=205` 期望 ~70、`UV=(110,25)` 期望 ~(109,184)）；每轮 `group usage after put` 记账与申请字节一致 + `group cleared` + 无 fd 泄漏（见 5.17） | ⚠️ 部分通过：cvt / resize / crop / crop+scale / paste（宽 ≥ 68）可用；blend 需标定，叠字暂走 CPU |
| 2026-11-12 | 顶层 `Makefile` + 默认构建目录 `build/` → `out/`（并修 P1-2） | `make` → `[100%] Built target EdgeVisionService`、`grep -cE 'error:|Error [0-9]'` = **0**；`out/lib` 6 个 `.so`（含 `librtsp.so`）+ `out/bin/EdgeVisionService`；`make help` / `make info` `RC=0`；`make distclean`（无 `FORCE=1`）`RC=1` 拒绝执行；`make run` → 4 条链路启动、3 路 mp4 出帧 23.7–24.0fps、`*:8550` 监听（见 5.18） | ✅ 构建 + 运行通过（`testnode` 未参与构建，属用户有意关闭） |

| 2026-11-14 | 崩溃现场采集（报告 + core + 诊断脚本） | `make reconfigure && make` → 0 error；探针 `/tmp/evs_crash_probe` → `probe_rc=139` + 报告 + `core.<pid>`（589,824 B），gdb 读到 `chain_id=7` / `p=0x0` / `f={1920,1080,"vi0"}`；`timeout --preserve-status -s QUIT 6 ./bin/EdgeVisionService ../config/node.json` → `RUN_RC=131` + `out/crash/…_sig3.log` + `out/core.75879`（353,808,384 B）、`analyze_core.sh` 24 线程全解析、`.so` missing=0（见 5.19） | ✅ 通过（`enable_coredump.sh` / `gcore_now.sh` 未实跑） |
| 2026-11-14 | **docs/ 与 scripts/ 目录分类 + README**（纯文档/脚本搬迁，无 C++ 改动） | `docs/` 分子目录：`design/`（ROADMAP·DESIGN_MULTI_PIPELINE·DESIGN_PROCESS_SPLIT）、`ops/`（COMMANDS·CRASH_DUMP）、`history/`（TESTLOG·BUG_CASES）、`interview/`（INTERVIEW），根保留 `HANDOFF`/`ARCHITECTURE`/`STATUS`；`scripts/`：`diag/`→`coredump/`、`evs-edgevision.service`→`deploy/`。新增 `docs/README.md`、`scripts/README.md`（+4 个子目录 README）、`scripts/tests/README.md`、`cmake/README.md`、`config/README.md`、`src/README.md`。验证：`bash -n` 12 个脚本全过（无输出）；markdown 相对链接体检 **missing=0**；`grep -rn 'docs/<旧名>\\.md'`、`grep -rn 'scripts/diag'` 均无残留；38 个文件（md/sh/cpp/h/cmake/service/Makefile）引用路径批量修正 | ✅ 通过（路径与文档变更，代码语义未变） |
| 2026-11-14 | 目录分类后构建复验 | `make` → `[100%] Built target EdgeVisionService`；`grep -cE 'error:\|Error [0-9]'` = 0（`out/` 增量构建，无源码改动） | ✅ 通过（分类未影响构建） |
| 2026-11-14 | 目录分类后冒烟运行（4 路真实出帧） | `timeout --preserve-status -s INT 8 scripts/run/out/start.sh` → `rc=0`；日志：`[main] 配置来源: …/config/node.json（v1 node.json） / 链路 4 条`、`[main] global: rtsp 端口 8550 / app "live" / pool.blocks 64 / queue_depth 8`、4 条链 `vi0→vpss0→rtsp0→print0` / `vi1→rtsp1→print1` …；实测 **30.44/23.98/23.96/23.96fps**、`[MemManager] FreeMem: 释放 50 块 / 100 个槽位`、`[main] FreeMem 完成，进程正常退出` | ✅ 通过（与 `config/README.md` §8 记录的日志逐行一致） |
| 2026-11-14 | 入库并推送（`f0b7899` 崩溃采集 · `c6d040c` 目录分类+README） | `git push origin rk3588` → `547ab3a..c6d040c rk3588 -> rk3588`、`rc=0`；两个 commit 分别 18 文件 / 40 文件；`git status` 提交后仅剩 `?? config/2.mp4`、`?? config/test.nv12`（本机素材，有意不入库） | ✅ 通过 |
| 2026-11-14 | **T6 通信层**（`src/comm/` → `libcomm_api.so`） | `make` → 0 error，新增 `out/lib/libcomm_api.so` + `out/bin/test_comm`；`run_t6_comm.sh` → **43 项 PASS / fail=0**（MQTT 分支临时起 broker 真跑 pub/sub）；`timeout 12 make run` → 4 路 23.9~29.9fps、`FreeMem: 释放 50 块`、正常退出（见 5.21） | ✅ 通过（未接主链路） |
| 2026-11-14 | **两进程拆分 + 进程可选编译**（`evs_media` / `evs_comm` + 兼容壳 `EdgeVisionService`） | `make` → 0 error（`out/bin/{evs_media,evs_comm,EdgeVisionService}`）；`-DEVS_PROCESS=media` / `comm` 两棵独立构建树产物正确，未选中的 target 仍可 `--target` 构建；`make media` / `comm` / `proc PROC=comm` / `proc PROC=all` 全 rc=0；`make run-media` → 4 路 `实测=23.82fps` + `FreeMem: 释放 50 块` + 正常退出；`make run-comm` → 有 broker `cmd → running` 且收到 `evs/v1/media/evt/heartbeat size=17`，无 broker `Connection refused` 后正常退出（见 5.22） | ✅ 通过 |


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

### 5.12 L2 · 修 P0-14：池块 `FrameBuffer` 视图被跨链路改写（四路下 1080p 路下半屏全绿）（2026-11-09）

- 代码基线：工作树（`rk3588`，基于 `c557d89` = P0-13 校准点）
- 现象：T7 打通四路后，**ch0（`dev:` 1920×1080）画面下半屏约 47% 以下全绿、上半屏正常**；ch1~ch3（`file:` 1024×576）正常。该现象**间歇出现**（同一路时好时坏），单路跑 ch0 → 176 帧全好
- 定位（全部为实测数据，逐层排除，不是推断）：
  1. **不是 USB 丢帧**：V4L2 `DQBUF` 后探针统计 `bytesused` 与期望（640×480×2 = 614400）是否一致 → 450 帧 `short=0`；摄像头原始 YUYV（`/tmp/cam45.yuyv`）零行分析全干净
  2. **不是采集侧 sws 源数据**：在 `sws_scale` 之后同时打印输出 Y 十段均值与**UVC 源**十段均值 → 坏帧时源 `srcY` 十段全部正常（169.9 152.7 … 109.1），输出 `Ybands` 却是 `190.6 147.3 105.6 105.0 111.5 | 37.2 | 0 0 0 0`（band5 部分、band6~9 全 0）
  3. **不是 MPP 输入拷贝**：`MppEncoder::encode()` 里对刚 `memcpy` 完的 MPP buffer 做同样的十段统计 → 与采集侧**完全一致**（坏帧后半同样为 0），说明 `memcpy` 忠实地把坏数据搬了过去
  4. **不是编码/传输/解码**：MPP 码流 dump 后本地解码同样下半屏 0；720p（1280×720）也坏（排除垂直 stride 对齐）
  5. **关键证据**：同一探针打印 `frame->stride[0] / width / height` →
     - 第 0 帧：`st0=1920 st1=1920 w=1920 h=1080`（正常）
     - 第 30 帧起：`st0=1024 st1=1024 w=1024 h=576`，甚至出现 `w=1920 h=1080` 但 `st0=1024` 的混合态
     → 数字自洽：若 `stride=1024` 而写 1080 行，则只写入 `1024×1080 = 1105920` 字节，按 1920 宽折算恰为 **576 行**（band0~4 = 540 行全有数据、band5 前 36 行有数据 → 均值 36/108×112 ≈ 37.2，与观测的 37.1 吻合）
- 根因（**P0-14**）：`MemMeta` 里的 `FrameBuffer` 是**块级共享视图**（`data[0]/data[1]/width/height/stride` 都存在块上），4 条链路从同一个全进程池取块。`FfmpegCapture::transfer_frame()` 写完自己那帧会把视图改成自己的分辨率（1024×576、`hw_stride=1024`）再随块释放回池子；`V4l2Capture::read_frame()` 取到这种块后**直接沿用** `frame->stride[0]` 让 `sws_scale` 输出 → 视图过期时行宽错、`data[1]`（UV）偏移也错，Y 平面只覆盖 576 行，`1080×1920` 缓冲的剩余部分保持旧数据/0 → 编码后下半屏绿。单路时没有别的链路改写视图，所以全好
- 改动（2 个文件，+32/−1）：
  - `src/platform/capture/v4l2_source/v4l2_capture.cpp::read_frame`（+24/−1）：`DQBUF` 之后、`sws_scale` 之前
    `frame->BindMemory(frame->data[0], m_dst_width, m_dst_height, PixelFormat::FMT_NV12)`
    （`data[0]` 恒为块首地址，可安全复用；布局固定 NV12 —— `sws_getContext` 输出就是 `AV_PIX_FMT_NV12`，`src` 里的 fmt 段不参与输出）
  - 同函数补 `sws_scale` 返回行数校验：`ret_h != m_dst_height` → 归还 V4L2 buffer（`VIDIOC_QBUF`）并返回 false，不再静默产出半帧
  - `src/platform/capture/ffmpeg_source/ffmpeg_capture.cpp::transfer_frame`（+9）：`sws_scale` 之前同样
    `out_frame->BindMemory(out_frame->data[0], w, h, PixelFormat::FMT_NV12, 128)`
    （`align=128` 与原 `hw_stride = ((w+127)/128)*128` 完全一致，`y_ptr/uv_ptr` 都改从重绑后的 `data[0]/data[1]` 取）
- 命令：
  ```bash
  cmake --build build -j"$(nproc)"                                            # BUILD_EXIT=0
  timeout --preserve-status -s INT 24 ./build/run.sh testnode ../config/node.json   # 四路
  ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1:8550/live/ch0 \
      -t 8 -pix_fmt yuv420p -f rawvideo -y /tmp/dec_fix_ch0.yuv             # 再做 ch1（1024×576）
  python3 /tmp/badframe_fast.py /tmp/dec_fix_ch0.yuv 1920 1080 10            # Y/UV 各切 10 段，任一段均值 < 8 记坏帧
  ./scripts/tests/run_t5_builder.sh ; ./scripts/tests/run_t3_node_json.sh ; ./scripts/tests/run_t2_node_config.sh
  ./build/run.sh EdgeVisionService                                          # 冒烟
  ```
- 结果摘要：
  - **ch0（1920×1080，四路并发）坏帧 0/166**（修复前：四路 51/64、两路 63/129）
  - **ch1（1024×576，四路并发）坏帧 0/73**（ffmpeg 侧改动无回归）
  - 四路帧率照常（`print0≈29.9`、`print1~3≈23.9`），SIGINT 后正序停止、无残留进程
  - 回归：T5 `passes=64 fails=0 skips=2`、T3 `passes=104 fails=0 skips=0`、T2 `ALL PASS (fails=0)`；冒烟 `EdgeVisionService start`
- 结论：✅ 通过。P0-14 实测关闭；四路混合分辨率下 1080p 路画面完整
- 排障要点（省下次时间）：
  1. 「帧的一半坏」先看**写入者的行宽**，不要先怀疑 USB/驱动：本例 `stride=1024` 与 `1920` 的差异仅靠画面无法分辨，必须打印 `stride/width/height`
  2. 探针要**成对打印「源」和「目标」**（同一个循环里 srcY 与 Ybands），才能一刀切开「源坏」还是「转换/拷贝坏」
  3. 内存池是**块级共享**的：任何"视图"字段（宽高/stride/平面指针）都必须在**使用前重绑**，不能假设初始化时的值还在
- 未做 / 受限：
  - `FrameBuffer::BindMemory()` 会把 `capacity` 重算为 `w*h*3`，掩盖了 `Allocate()` 时的真实分配大小；当前池块按 1920×1080 分配、所有链路 ≤1080p，故无越界，但把链路分辨率配到 1080p 以上时会失去保护（属 P0-10 那类 `FrameBuffer` 生命周期问题，未在本次范围）
  - `vi.src` 里写 `YUV420P` 时 `frame->fmt` 仍写 `m_fmt`，而 `sws` 恒输出 NV12 → `MppEncoder` 会以「fmt 非 NV12」拒帧（既有缺陷，与本次无关；配置里实际都写 NV12）

---

### 5.13 L2 · 修 P0-9：`MemInit()`/`FreeMem()` 泄漏（`FrameBuffer` 堆内存从不释放）（2026-11-10）

- 代码基线：工作树（`rk3588`，基于 `114c94d` = P0-14 校准点）
- 现象：`MemInit()` 里每个池块 `new FrameBuffer` + `Allocate()` 会 `new uint8_t[capacity]`（1920×1080×3 = 6.22 MB/块），但 `FreeMem()` 只 `delete _memData`（`MemMeta`）——**既不删 `FrameBuffer`、也不释放它的数组**。进程"正常退出、退出码 0、日志全绿"，内存却整块沉淀
- 定位（先造一个**能失败**的量化口径，再动代码）：
  1. `VmRSS` 看不出：`new[]` 的页没被写过就不计入 RSS → 首版探针 6 轮全平 2364 kB，像是"没漏"
  2. 换 `mallinfo2().hblkhd`（mmap 在用字节）后立刻看到每轮 +12,443,648 B —— 但**第二轮又"归零"**：glibc 在 free 掉大 mmap 块后把 `mmap_threshold` 抬到该块尺寸，后续同尺寸分配改走 brk 主堆
  3. 正确口径 = `hblkhd + uordblks`（在用堆总量）。未修时每轮 `MemInit(2)` 净增 12.44 MB、`FreeMem()` 后一分不还（5 轮累计 60,760 kB）
- 根因（**P0-9**）：`FrameBuffer` 顶部注释写着「仅作为 MediaBlock 的视图，**不拥有内存，不负责释放**」，而 `Allocate()` 的实现是 `new (std::nothrow) uint8_t[capacity]` —— 所有权声明与实现相反，于是 `FreeMem()` 按注释办事、谁都不敢删除那块 6.22 MB。同函数里还有 `new MemBlock` 后未使用（`MemPool::addBuffer()` 内部自己 new）
- 改动（3 个文件）：
  - `src/common/frame_buffer.h`：新增 `uint8_t *owned_mem`（`Allocate()` 记基址、`BindMemory()` 不写）、`void *native`（平台句柄占位，仅视图）、`Release()`（`delete[] owned_mem` + 清 `data[0..2]`/`native`）；文件头注释改写成"所有权归谁由 `owned_mem` 决定"。`capacity` 仍为 `w*h*3`（按你的要求**本次不改**）
  - `src/common/MemManager/MemManager.cpp::MemInit`：只调 `Allocate()`，**删掉**紧跟其后的 `BindMemory(frame->data[0], ...)`（用"绑外部内存"的接口绑自己的堆块 = 所有权说两遍，将来谁给 `BindMemory` 加个清 `owned_mem`，`Release()` 立刻变空操作）；补 `Allocate()` 失败分支（`delete frame` + `continue` + 一行错误日志）
  - `MemManager.cpp::FreeMem`：按 `imgMap` 逐槽 `frame->Release()` → `delete frame`，再删 `NodeProfiler` / `MemMeta` / `MemBlock`
  - `src/common/type_registry.h`：**删除**（已无人 `#include`；唯一引用方 `main_testpool.cpp` 是使用旧 `GlobalMemManager`/`TypeRegistry` 的死代码，见 STATUS §4.2 第 17 条）
- 命令：
  ```bash
  cmake --build build -j"$(nproc)"                                   # BUILD_RC=0
  g++ -O2 -std=c++14 /tmp/mem_probe.cpp -I src/common -I src/common/MemManager \
      -I src/common/profiler -L build/lib -lcommon_api -Wl,-rpath,$PWD/build/lib -o /tmp/mem_probe
  /tmp/mem_probe 2 6                                                 # 2 块/轮 × 6 轮
  ./build/run.sh testnode > /tmp/evs_tn2.log 2>&1 & sleep 12; kill -INT $!; wait $!
  ```
- 结果摘要：
  - 修复前：`in_use` 每轮 +12,443,648 B，free 后不归还（`hblkhd` 12152 → 60760 kB）
  - 修复后：init 12,524,000 B → free 回落 82,080 B，**6 轮零累积**（82208/82336/82464/82592/82720 → 每轮 +128 B，即 2 块 × 2 个 `MemSlot<T>` ≈ 32 B/个，属 P0-11）
  - 四路 `testnode`：`[builder] 汇总: 建成 4 条, 失败 0 条`、`print1 frame#480 … 23.93fps`、SIGINT → `[main] FreeMem 完成，进程正常退出`、`exit_code=0`、无残留进程
- 结论：✅ 通过。P0-9 的帧内存泄漏实测关闭；`MemBlock` / `MemMeta` / `NodeProfiler` 一并释放
- 排障要点（省下次时间）：
  1. **RSS 不是泄漏度量**：`malloc` 的大块要写过才进 RSS；测泄漏用 `mallinfo2()` 的 `uordblks + hblkhd`
  2. 只看 `hblkhd` 会被 glibc **动态 mmap 阈值**骗（第二轮假性归零），两个指标必须相加
  3. **先让探针在旧代码上稳定复现增长**，再改代码看曲线归零；否则"改完是平的"证明不了任何事
- 未做 / 受限：
  - `capacity` 仍是 `w*h*3`（1080p NV12 实需 2.97 MB、实分 6.22 MB，2× 浪费），`BindMemory()` 还会把它重算成 `w*h*3` 掩盖真实分配大小 → 归 STATUS §4.1 第 10 条残余
  - `VpssNode::ensure_dst_frame()`（`moudle_vpss.cpp:338-382`）会**按块懒创建**自己的输出帧挂在 `out_slot_`（默认 `vpss_out`）上，而 `FreeMem()` 只遍历 `imgMap` 登记的槽位（`nv12`）→ 该帧不随 `FreeMem()` 释放。当时 `config/node.json` 的 vpss 是 `enable: false`，实测路径不触发 —— **已由 P0-11 收口（§5.14：`FreeMem()` 统一 `MemMeta::clear()`）**
  - `FrameBuffer` 仍可拷贝、无析构（靠调用方记得 `Release()`）；`MppDecoder::decode()` 往栈上 `FrameBuffer` 里 `memcpy`（P0-8）与 MPP DMA-HEAP 零拷贝（`owned_mem`→`native`）按你的要求**本次不动**（其中「可拷贝 / 无析构」两条已于同日晚收口：`~FrameBuffer(){Release();}` + 删拷贝，见 §5.15）

---

### 5.14 L2 · 修 P0-11：`MemMeta` 槽位从不销毁 + 节点自建槽位漏收（2026-11-10）

- 目标：反复 `MemInit`/`FreeMem` 以及节点自己往块上挂的槽位（`VpssNode` 的 `vpss_out`）都能零残留
- 现象：`MemInit(2)+FreeMem()` 每轮残留 128 B（2 块 × 2 个 `MemSlot`）；`FreeMem()` 只认 `imgMap` 里的 `nv12`，`VpssNode::ensure_dst_frame()` 懒创建后挂在 `vpss_out` 上的帧（6.22 MB/块）永远不会被回收
- 根因（原理层面）：
  1. `MemMeta::data_map_` 存 `void*`。`void*` 既不能安全 `delete`（UB，且**不调用析构函数**），也调不到 `MemSlot<T>::freeObj()` —— 类型擦除把「访问」擦通了，顺手把「销毁」也擦掉了
  2. 于是 `set_ptr()` 只能纯覆盖（旧槽位连同里面的对象一起丢），`MemMeta` 没有析构也没有 `clear()`，`FreeMem()` 只好按名字点名遍历 `imgMap`
  3. 点名遍历 ⇒ 节点后来挂上去的槽位天然不在名单里（`vpss_out` 整帧漏收）；容器不持有所有权，却拿走了唯一能回收它的指针
- 改动：
  - `common/MemManager/MemMeta.h`
    - `MemSlot<T>` 继承 `MemSlotBase`（虚析构）+ `~MemSlot(){ freeObj(); }`：**槽位死 = 对象一起还回去**（`setObj/freeObj/getObj/getObjPtr` 语义不变）
    - `data_map_` 改 `unordered_map<string, MemSlotBase*>`；`set_ptr()` 覆盖前先 `delete` 旧槽位；同一指针重复登记则保留在用对象、只丢弃新建槽位（避免双删）
    - 新增 `clear()`（先 `swap` 出 map，再在锁外逐个 `delete`，返回回收个数）、`~MemMeta()` 兜底调 `clear()`、删拷贝/赋值
    - 新增 `slot_free<T>()`：默认 `delete`；`slot_free<FrameBuffer*>` 先 `Release()` 再 `delete`（否则 `owned_mem` 的几 MB 又漏回 P0-9）
    - `data<T>()` 改 `dynamic_cast<MemSlot<T>*>`，类型不符返回空 + 一行日志（同修 P1-10）
    - `MemSlot()` 默认构造补 `t(T())`：加了析构之后，默认构造的槽位若 `t` 未初始化，析构就会 `delete` 一个野指针
  - `common/MemManager/MemManager.cpp::FreeMem()`：逐块 `_memData->clear()`，删掉按 `imgMap` 点名的遍历；块取空即 `continue`；退出打印 `释放 N 块 / M 个槽位`
  - `pipeline/moudle_vpss/moudle_vpss.cpp`：两处注释同步（内存归属 / 同名槽位规矩）
- 命令与结果：
  - `cmake -S . -B build && cmake --build build -j8` → `BUILD_RC=0`（10 个 target 全 built）
  - 仓库外探针 `/tmp/meta_probe`（`MemMeta.h`+`frame_buffer.h` 纯头文件，`g++ -std=c++14 -I src/common`）：

    ```
    baseline                  in_use=    73,376 B
    2 slots (nv12+vpss_out)   in_use=12,521,632 B
    overwrite vpss_out        in_use=12,521,760 B   # 只 +128 B 记账，没有多留一帧
    same-ptr re-set kept alive: yes                 # 重复登记不双删
    clear() released=2        → in_use=78,112 B
    after MemMeta dtor        in_use=78,112 B       # 无二次释放、无增长
    ```

  - `/tmp/mem_probe 2 6`（链 `libcommon_api`）：6 轮 `after free` **恒定 82,080 B**（修复前 82208 → 82336 → 82464 …，每轮 +128 B）
  - 四路 `testnode config/node.json`：`[builder] 汇总: 建成 4 条`、`29.63~29.89fps / 23.91 / 23.89 / 23.92fps`、SIGINT → `[MemManager] FreeMem: 释放 50 块 / 100 个槽位` → `[main] FreeMem 完成，进程正常退出`、`exit_code=0`、无残留进程
  - 泛化确认（仓库外探针 `/tmp/meta_probe_generic`，`g++ -std=c++14 -fsanitize=address` + `detect_leaks=1`）：同一套 `clear()` 对三种 `T` 都成立，ASAN 零报错、零泄漏
    - `MyTask*`（普通未知结构体，`new` 出来、析构自带 `delete[]`）：主模板 `slot_free` 直接 `delete` → 覆盖时 `dtor=1`，`clear()` 后 `dtor=2`，`~MemMeta` 后再 `+1` → `ctor==dtor==3`
    - `FrameBuffer*`：走特化（`Release()` + `delete`），6.22 MB `owned_mem` 一并还回（该特化在 §5.15 随 RAII 删除，现走主模板 `delete`，`~FrameBuffer()` 负责 `Release()`）
    - `CBox*`（`malloc` 出来）：在类型旁边特化 `slot_free<CBox*>` 即可，`MemMeta`/`MemSlot` 一行不改，`free=1`
    - `/tmp/meta_probe_dtor_only`：**故意不调 `clear()`**、只 `delete memMeta` → `~MemMeta(){clear();}` 兜底把两个槽位对象全还回去（`ctor==dtor==2`、ASAN 零泄漏、`RC=0`）。说明 `FreeMem()` 里显式 `clear()` 是**为了计数 + 固定顺序**，不是"不写就漏"
    - 反例 `/tmp/meta_probe_bad`（`malloc` 结构体不给特化）：ASAN 直接打出整条析构链 ——
      `MemMeta::clear() MemMeta.h:172 → ~MemSlot<CBox*>() :54/:53 → freeObj() :61 → slot_free<CBox*>() :25 → operator delete`，`ERROR: alloc-dealloc-mismatch (malloc vs operator delete)`
      → 既证明 `delete item.second` 确实经虚析构自动分发到 `~MemSlot<T>()`，也说明"分配器不匹配必须特化"
- 结论：✅ 通过。槽位结构（128 B/轮）与节点自建槽位的整帧（6.22 MB/块）都随 `FreeMem()` 回收；「只有 `imgMap` 里登记过的名字才会被回收」这条限制消失
- 未做 / 受限：
  - `config/node.json` 的 vpss 仍是 `enable:false`，`vpss_out` 路径只有 `MemMeta` 探针覆盖（真机未启转码链路）
  - 新约束：**一个裸指针只能登记在一个槽位名下**，`clear()` 时若同一个指针挂在两个名字上会双删（当前代码无此用法；同名重复登记已有保护）
  - `FrameBuffer` 仍可拷贝、无析构（P0-10 残余，**同日晚已收口，见 §5.15**）；`MppDecoder` 写栈（P0-8）本次不动
  - `MemBlock::~MemBlock()` 仍不 `delete _memMeta`（所有权约定归 `MemManager::FreeMem()`）—— 绕过 `FreeMem()` 直接删块仍会漏 `MemMeta`，本次不动
  - 删 `type_registry.h` 不影响构建（`BUILD_RC=0` 已验证：无 target 引用它）。唯一的文本引用在 `src/main/test/main_testpool.cpp:134`（`TypeRegistry::instance()`），该文件**未进 `src/main/test/CMakeLists.txt`**，且它 include 的 `global_pool_manager.h` 早已不存在 —— 属存量僵尸文件，是否清理另开一笔 `chore:`，本批不动

---

### 5.15 L2 · 收口 P0-10：`FrameBuffer` 改 RAII（析构调 `Release()`）+ 禁拷贝（2026-11-10）

- 目标：`Allocate()` 的堆内存**不需要人记得释放**；把「可拷贝 → 双释放」从语言层面关掉；顺带让 `MemMeta` 不再认识 `FrameBuffer`
- 起点问题：
  1. P0-9 加的 `Release()` 只解决"有地方能释放"，实际靠 `MemManager::FreeMem()` 记得调；
  2. `FrameBuffer` 可隐式拷贝：`FrameBuffer b = *a;` 得到两份 `owned_mem` 指向同一块，谁先析构谁 `delete[]` → 必然双释放（P0-10 ①）；
  3. `MemMeta` 为它单留一个 `slot_free<FrameBuffer*>` 特化 —— 槽位层被迫知道每个类型的释放细节
- 改动（2 个文件）：
  ```cpp
  // common/frame_buffer.h
  struct FrameBuffer {
    FrameBuffer() = default;
    FrameBuffer(const FrameBuffer &) = delete;
    FrameBuffer &operator=(const FrameBuffer &) = delete;
    ~FrameBuffer() { Release(); }   // RAII：Allocate() 的 new[] 随对象归还；BindMemory() 视图 owned_mem 为空 → no-op
  };
  ```
  - `common/MemManager/MemMeta.h`：删 `slot_free<FrameBuffer*>` 特化，主模板 `template <typename T> slot_free(T t){ delete t; }` 一句即归还（`~FrameBuffer()` 负责 `delete[]`）；注释改为"能自己写析构就写析构（首选），槽位侧零知识"
  - 保留 `MemMeta.h` 的 `#include "frame_buffer.h"`：`moudle_push.cpp` 等经 `moudle_push.h → MemPool.h → MemMeta.h` 拿 `FrameBuffer` 完整定义（`data<FrameBuffer*>` 需要完整类型），删了会编译失败；原因写进注释
- 前置核查（为什么这样改是安全的）：
  - 全仓 `new FrameBuffer` 只有 2 处（`MemManager.cpp:25`、`moudle_vpss.cpp:352`），分别由 `MemInit()` 失败分支 `delete` 与 `MemMeta::clear()` 收口
  - 栈上 `FrameBuffer` 3 处（`ffmpeg_decoder.cpp:62`、`main_encode.cpp:74`、`main_pushStream.cpp:202`）全部走 `BindMemory()`，`owned_mem == nullptr` → 析构 no-op
  - 无按值传参 / 返回 / 放入容器；`Release()` 本身幂等（`owned_mem` 用完置空），重复调用安全
- 验证：
  - `cmake --build build -j8` → `BUILD_RC=0`（无新增告警）
  - `/tmp/fb_real_probe`（**直接用仓库头文件** + ASAN）：`new FrameBuffer` + `Allocate(1920,1080,NV12)` → `delete`（**全程未显式 `Release()`**）→ 堆内存由 `~FrameBuffer()` 归还，ASAN 零错误零泄漏 `REAL_RC=0`；栈视图 `BindMemory()` 出作用域后外部缓冲仍可写（写入 `0x5A` 读回 `0x5A`）
  - `/tmp/fb_copy_forbidden.cpp`（编译期反证）：`FrameBuffer copy = *src;` → `error: use of deleted function 'FrameBuffer::FrameBuffer(const FrameBuffer&)'`，`copy2 = *src;` → `error: use of deleted function 'FrameBuffer& FrameBuffer::operator=(const FrameBuffer&)'`，`CC_RC=1`（**预期的失败**）
  - `/tmp/fb_raii_copy_probe`（反例探针：**只加析构、不禁拷贝**，ASAN）→ `ERROR: AddressSanitizer: attempting double-free`，栈里两次 `FrameBuffer::Release()` → 证明"加析构必须连拷贝一起禁"，否则泄漏升级成必崩
  - `/tmp/meta_probe`（重编，`-O1`）：2 槽位 12.52 MB → 覆盖同名不增帧 → 同一指针重复登记保留在用对象 → `clear() released=2` 回落 78,112 B、`META_RC=0`
  - `/tmp/mem_probe 2 6`：`MemInit(2)+FreeMem()` 6 轮 `after free` 恒定 82,080 B（与 P0-11 基线一致，无回归）
  - 四路 `testnode` + SIGINT（`timeout --preserve-status -s INT 15 ./build/run.sh testnode <绝对路径>/config/node.json`）：29.92 / 23.93 / 23.90 / 23.91 fps → `[MemManager] FreeMem: 释放 50 块 / 100 个槽位` → `[main] FreeMem 完成，进程正常退出`，**退出码 0**，`pgrep testnode` 无残留（同命令不带 `--preserve-status` 时 `timeout` 自身返回 124，属 timeout 语义，不代表进程异常）
  - 回归：`bash scripts/tests/run_t5_builder.sh` → `passes=64 fails=0 skips=2`；`run_t3_node_json.sh` → `passes=104 fails=0 skips=0`；`run_t2_node_config.sh` → `ALL PASS (fails=0)`；冒烟 `./build/run.sh EdgeVisionService` → `EdgeVisionService start`
- 结论：✅ 通过。P0-10 的 ① 可拷贝、② 无析构两条关闭；`delete frame` 成为归还内存的唯一入口，`MemMeta` 不再需要按类型特化
- 未做 / 受限：
  - ③ `capacity` 仍是 `w*h*3`（1080p NV12 实需 2.97 MB、实分 6.22 MB），`BindMemory()` 仍会把它重算成 `w*h*3` 掩盖真实分配大小 → 归 STATUS §4.1 第 10 条残余
  - **新增约束**：不能按值拷贝 `FrameBuffer`（编译期拦截）；将来要共享一帧只能传 `FrameBuffer*`，或另加显式 `Clone()` 语义
  - `~FrameBuffer()` 只 `Release()`，不重置 `width/height/stride/fmt/data_size`（对象已销毁，无意义）；视图字段的重置仍由 `Allocate()`/`BindMemory()` 负责
  - `Allocate()` 对同一对象**重复调用未加保护**（第二次会直接覆盖 `owned_mem`，旧块漏掉）。当前两个调用点都只调一次（`MemManager.cpp:29` 是全新对象、`moudle_vpss.cpp:344-356` 槽位为空才建），所以没有活路径；将来若支持"分辨率变化重分配"，应在 `Allocate()` 开头补一句 `Release()`
  - P0-8（`MppDecoder` 往栈上 `FrameBuffer` 里 `memcpy`）与本改动正交，本次不动 —— 栈视图加分片拷贝，本就要在解码侧补分配，不因 RAII 而变

### 5.16 L2 · 帧内存改由 MPP DRM 缓冲提供（`FrameBuffer::Allocate()` 不再 `new[]`）（2026-11-11）

- 目标：把「池块像素内存从哪来」从 `new uint8_t[]` 换成 **MPP 的 DMA 缓冲**（`mpp_buffer_get` + `mpp_buffer_get_ptr`），`FrameBuffer` 的 API、字段语义、节点侧写法**一律不动**；非 rk3588 平台继续走堆
- 起点问题：
  1. 池块内存是普通堆内存，VPU / RGA 拿不到物理地址 → 编码前必须 `memcpy` 整帧进 `mpp_buffer`（1080p NV12 每帧 3 MB 拷贝，四路 30fps ≈ 360 MB/s 纯 CPU 搬运）
  2. 分配点散落在 `frame_buffer.h` 里（`new[]/delete[]` 直接写在 `Allocate()`），将来要换后端就得改所有 include 它的层
  3. 直接让 `frame_buffer.h` 去 include MPP 头不可行：`platform` / `pipeline` / `main` / 测试都 include 它，**x86 构建与离线探针会立刻编不过**
- 改动（**4 个文件改动 + 2 个新文件，其余零改动**）：
  - 新增 `src/common/frame_mem.{h,cpp}`：`namespace pool` 的**唯一分配边界**
    - `FrameMemAlloc(bytes, &native)`：懒建一个 `MppBufferGroup`（`mpp_buffer_group_get_internal(&g, MPP_BUFFER_TYPE_DRM)`）→ `mpp_buffer_get(group, &buf, bytes)` → `mpp_buffer_get_ptr(buf)` 拿 CPU 可写 VA；返回 VA，并把 `MppBuffer` 写进 `native`
    - `FrameMemFree(cpu_ptr, native)`：MPP 模式 `mpp_buffer_put(native)`；堆模式 `delete[] cpu_ptr`
    - `FrameMemIsMpp()` / `FrameMemUsage()`（组内记账字节数）/ `FrameMemDeinit()`（`mpp_buffer_group_clear` + `mpp_buffer_group_put`）
    - MPP 头**只出现在 `frame_mem.cpp`**，由 `EVS_FRAME_MEM_MPP` 宏隔离；`std::mutex` 串起「懒建组 + get/put」（`VpssNode` 运行期会懒建输出帧，与 `FreeMem()` 收尾可能并行）
  - `src/common/frame_buffer.h`：加一个字段 `void *native = nullptr`（平台句柄，与 `owned_mem` 同生共死）；`Allocate()` 的 `new[]` → `pool::FrameMemAlloc(capacity, &native)`；`Release()` 的 `delete[]` → `pool::FrameMemFree(owned_mem, native)`（`owned_mem || native` 任一非空才动手）。**其余结构、字段、`BindMemory()`、RAII 析构、禁拷贝全部保持原样**
  - `src/common/MemManager/MemManager.cpp`：`FreeMem()` 末尾加一行 `pool::FrameMemDeinit();`（原因见「坑 1」）。`FreeMem()` 原有的遍历 / 打印 / 释放顺序**未动**
  - `src/common/CMakeLists.txt`：源文件列表加 `frame_mem.cpp`；`TARGET_PLATFORM STREQUAL "rk3588"` 分支 `find_package(MPP REQUIRED)` + `target_compile_definitions(common_api PRIVATE EVS_FRAME_MEM_MPP=1)` + `target_link_libraries(common_api PUBLIC ${MPP_LIBRARIES})`，configure 打印 `frame mem: MPP DRM buffer (...)`（非 rk3588 打印 `frame mem: heap new[]/delete[]`）
  - 依赖面变化：**`libcommon_api.so` 从此 `NEEDED librockchip_mpp.so`**（链接写成 `PUBLIC`：`common_api` 的 `frame_mem.cpp` 自己调 MPP，`pipeline` / `main` 经它间接依赖，不再靠可执行文件层补链）；MPP 头不进任何 `INTERFACE_INCLUDE_DIRECTORIES`，只在 `common_api` 内部用
- 坑（都是实测踩到的）
  1. **`mpp_buffer_put` 不还系统**：它只把缓冲挂回组的 `unused` 链表（MPP 的池化语义），DMA 内存仍被组占着。所以必须有 `FrameMemDeinit()`（`group_clear` + `group_put`），否则进程退出前账面上一直占着 311 MB；`FreeMem()` 是唯一「池里已无块」的时点，就放在那里
  2. **组默认 80 MB 上限**：`BUFFER_GROUP_SIZE_DEFAULT` 是 80 MB，50 块 1080p NV12 ≈ 311 MB，不显式 `mpp_buffer_group_limit_config(group, 0, 0)` 会从第 13 块起 `mpp_buffer_get` 失败
  3. **DRM 缓冲非 cacheable**：`mpp_buffer_get_ptr` 的映射不走 page cache（实测写满 311 MB 后 `VmRSS` 仍 4 MB，属 `VM_PFNMAP`），CPU 写完 VPU 直接可见，不需要 `cache flush`；代价是 CPU 读写这段内存比堆内存慢（本改动不碰编码路径，先吃这个成本）
  4. `mpp[xxxx]: mpp_platform: client 12 driver is not ready!` 是 MPP 探测无关平台客户端时打的噪音（只在探针首行出现），不代表分配失败
- 验证：
  - `cmake -S . -B build` → `CONFIG_RC=0`（打印 `frame mem: MPP DRM buffer (...)`）；`cmake --build build -j"$(nproc)"` → `BUILD_RC=0`、`warning` **0** 条
  - `/tmp/mpp_pool_probe`（仓库外，`-I src/common -I src/common/MemManager` + `-L build/lib -lcommon_api`，脚本 `MemInit(2) + 取块 + FreeMem` × 2 轮）：
    - `FrameMemIsMpp=1`、`[FrameMem] backend: MPP DRM buffer group (mpp_buffer_get + mmap)`
    - 记账 `12,441,600 B` = 2 × 6,220,800（与堆版 `capacity` 完全等价）；`native != nullptr`
    - `data[1] - data[0] = 2,073,600 = 1920×1080`（NV12 平面布局不变）；写满 3,110,400 B 后首字节 `0x40` / 末字节 `0x80` 回读一致
    - `FreeMem` 后记账 **0 B**；`/proc/self/fd` `11 → 11`（两轮 `delta=0`，无 DMA-buf fd 泄漏）；第二轮重新建组、重新分配全部成功
  - `/tmp/mpp_rss_probe 50`（按真实池规模 50 块）：记账 `296 MB` → 50 块全部写满后 `VmRSS` 仍 4 MB（印证坑 3）→ `FreeMem` 后记账 `0 MB`、`mallinfo2.uordblks = 241 kB`（堆侧无残留）
  - 四路 `testnode ../config/node.json` + 拉流解码统计（判据同 §5.11）：ch1 `U=128.1±10.9 / V=128.0±1.6`（源 `126.6±10.9 / 128.2±1.7` —— 与 P0-13 修复后基线逐位一致）、ch0 `U=127.3±7.1 / V=129.8±6.7`（摄像头静态画面，方差低即无花屏）；`hal_h265e` 报错 **0** 次；fps `29.18 / 23.94 / 23.94 / 23.78`；SIGINT → `[MemManager] FreeMem: 释放 50 块 / 100 个槽位` → `[main] FreeMem 完成，进程正常退出` → `EXIT=0`，`pgrep testnode` 无残留
  - 回归：`run_t5_builder.sh` → `passes=64 fails=0 skips=2`；`run_t3_node_json.sh` → `passes=104 fails=0 skips=0`；`run_t2_node_config.sh` → `ALL PASS (fails=0)`
- 结论：✅ 通过。帧内存改为设备 DMA 缓冲，`FrameBuffer` 对外行为未变（四路画面 / fps / 优雅退出全等价），非 rk3588 仍走 `new[]`
- 未做 / 受限（**明确不在本次范围**）：
  - **编码前的整帧 `memcpy` 仍在**（`rk_mpp_encoder.cpp` 拷贝进 encoder 自己的 `mpp_buffer`）：池内存已是 DMA 缓冲，下一步可以 `mpp_buffer_import` 直接交给编码器（或让编码器直接用池块的 `native` 句柄），那属于编码器改造，本次不动
  - 采集侧仍是 CPU 写（V4L2/FFmpeg 解码后 `sws_scale`/`memcpy` 进 DMA 缓冲），未做零拷贝 DMA-BUF 导入
  - 与本改动无关的既有遗留一律不动：`capacity = w*h*3` 的 P0-10 残余、`VpssNode::process_frame()` 仍为透传、mp4 不 loop（T7b）
  - 非 rk3588 平台路径**只做了代码分支，未在 x86 上实际编译**（本机是板端；分支里只有 `new[] / delete[]`，与改造前的代码逐字相同）

### 5.17 L2 · 探针实测：MPP DRM 缓冲直传 RGA（缩放 / 格式转换 / 裁剪 / 粘贴 / 叠字）（2026-11-11）

- 目标：为后续 `src/platform` RGA 封装做可行性判定，回答五问：
  1. 池块内存（`FrameMem` 分配的 MPP DRM 缓冲）能否**不经 CPU 拷贝**直接交给 RGA？
  2. 预处理要用的**缩放**（1080p→720p）与**格式转换**（NV12→RGB888）能否做？
  3. **裁剪**（crop）能否做？裁剪 + 缩放能否一步完成？
  4. **粘贴**（把小块贴到帧的任意位置）能否做？有没有尺寸下限？
  5. **叠字**（ARGB 小块按 alpha 混合进 NV12 帧）能否做？
- 基线：`12f5480`（工作树干净）；设备：RK3588，`/usr/local/lib/librga.so` 1.9.1（运行期打印 `rga_api version 1.9.1_[4]`）
- 关键环境事实：**RGA 头必须用仓库内 `3rdparty/rk3588/librga-1.9.1/include`** —— `/usr/local/include/rockchip/` 下 17 个 RGA 头全是 **0 字节空壳**（`rga.h` / `im2d*.h` / `Rga*.h`），只有 MPP 头正常；`cmake/FindRGA.cmake` 的 `find_path` 会命中空壳从而误判 `RGA_FOUND`（坑见 COMMANDS §9）
- 探针（全部在仓库外，均 `EXIT=0`）：
  - `/tmp/rga_probe.cpp` —— `imcopy` / `imcvtcolor`
  - `/tmp/rga_probe2.cpp` —— `imresize` + 混合
  - `/tmp/rga_probe3.cpp` —— 混合失败因素拆解（A 组 RGB 目标 / B 组全屏 NV12 目标）
  - `/tmp/rga_probe4.cpp` —— `imcrop` / paste / crop+scale
  - `/tmp/rga_probe5.cpp` —— 小矩形阈值扫描
- 统一编译与运行命令：
  ```bash
  g++ -std=c++14 -O2 -I/usr/local/include \
      -I/mnt/nvme/workspace/EdgeVisionService/3rdparty/rk3588/librga-1.9.1/include \
      /tmp/rga_probeN.cpp -o /tmp/rga_probeN \
      -L/usr/local/lib -lrockchip_mpp -lrga -lpthread
  LD_LIBRARY_PATH=/usr/local/lib:/usr/lib timeout 30 /tmp/rga_probeN; echo "EXIT=$?"
  ```
- 结论矩阵（每项都有 CPU 侧数值校验，不靠肉眼看图）：

| 能力 | API | 结果 | 实测校验 |
| --- | --- | --- | --- |
| 同格式整帧拷贝 NV12→NV12 | `imcopy` | ✅ | 两帧 3,110,400 B 逐字节比对，**不一致数 0** |
| 格式转换 NV12→RGB888 | `imcvtcolor` | ✅ | 首/末像素均 `(55,55,56)`（纯 Y=64 经 BT.601 limited，**不是 64**，符合色彩学预期） |
| 缩放 1080p→720p | `imresize` | ✅ | 纯色输入缩放后 Y 平面 `min=max=64`（stride/尺寸处理正确） |
| 裁剪 640×480 @ (100,50) | `imcrop` | ✅ | `dst[0]=150` = `src(100,50)`、`dst(639,479)=244` = `src(739,529)`，位置精确 |
| 裁剪 + 缩放 640×360→320×180 | `improcess`（`src_rect`+`dst_rect`） | ✅ | 首/末点 `1` / `229` 对源 `0` / `230`（双线性插值 ±1） |
| 粘贴 128×64 → dst (600,400) | `improcess`（无 blend，`IM_SYNC`） | ✅ | 落点像素变为源色 `200`，区外保持 `10`（偏移位置生效） |
| 粘贴 64×32 / 64×64 | 同上 | ❌ | `RGA_BLIT fail: Invalid argument` |
| alpha 混合 ARGB→NV12（全屏 1280×720） | `improcess` + `IM_ALPHA_BLEND_SRC_OVER` | ⚠️ 提交成功但值不符 | `Y(0,0)=205`（期望 ~70）、`UV(0,0)=(110,25)`（期望 ~(109,184)）；透明区 `Y(64,0)=64`、`Y(640,360)=64` 正确 |
| alpha 混合 ARGB→RGB888 64×32 | 同上 | ❌ | `Invalid argument`（与贴图同因：矩形过小，非格式问题） |

- 尺寸阈值扫描（src / dst 同为 1280×720 NV12，只改 rect 尺寸做同尺寸拷贝，**排除缓冲大小因素**）：

| 矩形尺寸 | 结果 |
| --- | --- |
| 16×16 / 32×32 / 32×64 / 64×32 / 64×64 | ❌ 失败 |
| 64×96 / 64×128 / 64×256 | ❌ 失败（宽仍是 64，加高无效） |
| 66×64 | ❌ 失败 |
| **68×64** / 70×64 / 96×64 / 104×64 | ✅ 成功 |
| 128×16 / 128×32 / 128×48 / 160×32 / 256×16 | ✅ 成功（高 16 也能过） |
| 127×64 | ❌ 但错误不同：`src, Error yuv not aligned`（奇数宽） |

→ 判据：**矩形宽度 ≥ 68 且偶数对齐，高度无此下限**；**面积不是判据**（`256×16` 面积 4096 成功，`64×64` 面积同为 4096 却失败）。

- 记账与资源（每轮末尾）：
  - `group usage after put` 恒等于本轮申请字节总量（`4,500,992` / `5,043,072` / `5,083,136`），`group cleared` 正常返回
  - fd 不泄漏：RGA 侧只 import 不 close（fd 归 MPP），`mpp_buffer_put` 后随缓冲回收；RGA 句柄必须 `releasebuffer_handle`
  - 前提是所有 RGA 调用同步完成（`imresize` / `imcopy` 默认 `sync=1`；`improcess` 传 `IM_SYNC`），因此 `mpp_buffer_put` / `FrameMemDeinit()` 之前 job 一定已结束
- 结论：
  - ✅ **MPP DRM 缓冲可直传 RGA**，路径固定为 `mpp_buffer_get_fd(native)` → `importbuffer_fd()` → `wrapbuffer_handle()`，**不需要物理地址、不需要 CPU VA、不需要 cache flush**（DRM 映射非 cacheable，CPU 写完硬件立即可见）
  - ✅ 可落到 `src/platform` 的能力：格式转换、缩放、裁剪、裁剪 + 缩放、纯拷贝粘贴（宽 ≥ 68 且偶数）
  - ⚠️ **叠字暂不走 RGA**：混合能提交但输出值不符 alpha 语义（上表），未标定不能上产品；现有 CPU 叠字（`OsdRenderer` → `FrameDrawer::blit` 直接写 `data[]`，即 DRM 映射的 VA）**零改动可用**
  - 封装必须内置回退：矩形宽 < 68 就走 CPU。否则驱动只回一句笼统的 `Invalid argument`，极易被误判成"内存 / cache 问题"而白查半天
  - 同一块缓冲可串行复用（CPU 叠字 → RGA 缩放 → VPU 编码），每步同步返回后再交接
- 未做 / 受限（明确不在本次范围）：
  - 混合的通道顺序与 alpha 语义**未标定**（需系统写单通道纯色再回读）；`imosd()` / `imcolorkey()` 未测
  - 未测性能：单次耗时、带宽占用、与 VPU/CPU 的争用
  - 非 rk3588 未验证（`FrameMem` 在那条分支仍走 `new[]`，RGA 无从 import）
  - 编码器零拷贝（`mpp_frame_set_buffer`，去掉 `rk_mpp_encoder.cpp` 的两段 `memcpy`）属后续任务，本次**未改一行仓库代码**
  - 探针文件留在 `/tmp`，不进 git

---

### 5.18 L2 · 顶层 `Makefile` 薄包装 + 默认构建目录 `build/` → `out/`（2026-11-12）

- 代码基线：`rk3588 @ 12f5480` + 工作树（`Makefile` / `.gitignore` / `src/platform/CMakeLists.txt` 未提交；`src/main/*` 另有用户改动，未纳入本次提交）
- 目标：给 CMake 加一层薄包装（**规则仍全在 CMake**，Makefile 只转发），默认构建目录从 `build/` 换成 `out/`
- 命令：
  ```bash
  make help          # 列出 target 与可覆盖变量
  make info          # BUILD/JOBS/CMAKE/PLATFORM + 是否已 configure + out/{bin,lib} 内容
  make distclean     # 未加 FORCE=1 → 期望拒绝
  make reconfigure && make
  make run
  ```
- 结果摘要：
  - `make` 首次全量 → `[100%] Built target EdgeVisionService`，`grep -cE 'error:|Error [0-9]'` = **0**
  - `out/lib`：`libcapture.so` `libcommon_api.so` `libdecoder.so` `libencoder.so` `libpipeline_api.so` **`librtsp.so`**；`out/bin/EdgeVisionService`
  - `make help` / `make info` → `RC=0`；`make distclean`（无 `FORCE=1`）→ `RC=1` 拒绝，`out/` 未被创建
  - `make run` → 加载 `../config/node.json`（4 条链）、`*:8550` 监听、3 路 mp4 实测 23.72–24.04fps；`vi0`（`/dev/video0`）打不开但**其余链照常出帧**（P0-2 已修的既有行为），进程需 `SIGINT` 退出
- **踩坑（P1-2 被换目录当场触发）**：
  - 现象：`out/` 首次编译在 `src/pipeline/moudle_push/moudle_push.cpp` 报 `zlm_push_server.h: No such file or directory`，`moudle_vi.h` 报 `libavformat/avformat.h: No such file or directory`；而同一份源码在 `build/` 下一直编得过
  - 定位：比较新旧 `src/pipeline/CMakeFiles/pipeline_api.dir/flags.make` 的 `-I` 列表 → 旧 `build/` 多 `-Iinstall/rk3588/include` 与 `-Isrc/platform/rtsp`；再比 `CMakeCache.txt` → 旧缓存有 `BUILD_RTSP_MODULE:BOOL=ON`，新目录**完全没有这个键**
  - 根因：`BUILD_RTSP_MODULE` 只在 `src/platform/CMakeLists.txt:34` 被读，**从未 `option()` 声明**（同文件另外三个开关都声明了，属遗漏）→ 新目录里它是空值 → `rtsp` target 不加入 → `platform_api` 少 `INTERFACE` 链 `rtsp` → pipeline 丢掉 `rtsp` 的 `PUBLIC ${ZLM_INCLUDE_DIRS}`（= `install/rk3588/include`，而它同时是 FFmpeg 与 ZLM 头的唯一来源）
  - 修复：`src/platform/CMakeLists.txt` 补 `option(BUILD_RTSP_MODULE "..." ON)`；HANDOFF §1.4 第 3 条与 STATUS §4.2 P1-2 同步标已修
  - 为什么难发现：旧 `build/CMakeCache.txt` 里那条手工缓存值把缺陷掩盖了；文档甚至把"新目录不编 rtsp"写成旁注，没当成**必现故障**
- 操作注意（非交互验证）：
  - `make run` 内部是 `sh -c 'cd out && ...'`，外部用 `pkill -INT -f bin/EdgeVisionService` 只命中 `sh`，**不会转发给子进程**；要给真实 PID 发 `kill -INT <pid>`（交互式终端按 `Ctrl+C` 是发给整个前台进程组，没有这个问题）
  - 清理后核对 `pgrep -af bin/EdgeVisionService` 为空 + `ss -ltnp | grep 8550` 为空
- 未做 / 受限：
  - `testnode` / `test_builder` 未构建：`src/main/CMakeLists.txt:20` 的 `# add_subdirectory(test)` 当前为注释（用户有意关闭）；`make run-node` 会给出提示并退出 1
  - **验收脚本路径未统一**：`scripts/tests/run_t{1,2,3,5}*.sh` 内写死 `build/`（`-B build` / `--build build` / `-L build/lib` / `./build/bin/test_builder`），与默认 `out/` 不一致；本次**未改脚本**（超出本任务范围，且 `test/` 目录已按用户要求注释），`make test` / `make test-t5` 只打印提醒。统一路径待后续任务
  - x86 未验证（P0-1 既有失败）

---

### 5.19 L2 · 崩溃现场采集：可读报告 + core（gdb 看变量）+ 诊断脚本（2026-11-14）
- 代码基线：工作树（未提交）；构建目录 `out/`，`CMAKE_BUILD_TYPE=RelWithDebInfo`
- 做了什么：
  - 新增 `src/common/crash/crash_handler.{h,cpp}`：`sigaction(SA_SIGINFO|SA_RESETHAND|SA_ONSTACK)` 抓 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT（+SIGQUIT），写完报告后**先解除信号屏蔽再 `tgkill` 重抛**，让内核按 `core_pattern` 落 core；`std::set_terminate` 抓未捕获异常（demangle 类型 + `what()`）；handler 全程 async-signal-safe（无 `snprintf`/`malloc`/`cerr`）
  - 构建：顶层 `CMakeLists.txt` 未指定时默认 `RelWithDebInfo`（`-g`）+ `-fno-omit-frame-pointer`；`src/main/CMakeLists.txt` 加 `-rdynamic`（否则 `backtrace_symbols` 只有裸偏移）；`main.cpp` 最前面安装 handler + `SetCrashTag("config", ...)`
  - 脚本：`scripts/coredump/{enable_coredump,symbolize_report,analyze_core,gcore_now}.sh` + `evs-edgevision.service`
- 命令与结果：
  ```bash
  make reconfigure && make        # 0 error；crash_handler.cpp 编进 libcommon_api.so
  # 仓库外探针 /tmp/evs_crash_probe.cpp（-O0 -g -rdynamic）
  ulimit -c unlimited; cd /tmp/evs_crash && /tmp/evs_crash_probe
  ```
  - `probe_rc=139`；生成 `crash_evs_crash_probe_<pid>_<epoch>_sig11.log` + `core.<pid>`（589,824 B）
  - gdb：`#3 crash_in_worker (chain_id=7, frame=0x…)`、`p = 0x0`、`#4 main … f = {width = 1920, height = 1080, tag = "vi0"}` → **core 里能读到局部变量**
  - `symbolize_report.sh` → `crash_in_worker(int, Frame*) /tmp/evs_crash_probe.cpp:17`、`main …:38`
  - 真实程序：`cd out && timeout --preserve-status -s QUIT 6 ./bin/EdgeVisionService ../config/node.json` → `RUN_RC=131`（128+3）；报告 `out/crash/crash_EdgeVisionService_75879_1794659287_sig3.log` 含 `config=../config/node.json` tag、栈顶 `clock_nanosleep ← main+0x574`；core `out/core.75879` = 353,808,384 B
  - `analyze_core.sh out/core.75879 out/bin/EdgeVisionService` → gdb 退出码 0、24 个线程全列、`libpipeline_api.so`/`libcommon_api.so` 等 14 个 `.so` 全部 `Yes`（修 solib 顺序后 `Could not load shared library symbols` = 0）
- 结论：✅ 通过（报告 / core / 符号化 / 活进程快照四环打通；修掉两处坑：handler 内信号屏蔽导致无 core、`backtrace_symbols` 偏移语义导致 `?? ??:0`，见 BUG_CASES B12/B13）
- 未做 / 受限：`enable_coredump.sh` 未执行（需 root；本机 `core_pattern` 仍为 `core`，core 落在进程 cwd）；`gcore_now.sh` 未实跑（会 attach 活进程）；systemd 单元未部署

### 5.20 崩溃采集补齐：体检 / 归档改名 / 两个 start.sh / 流程文档（2026-11-14，L2）

- 任务：补上"为什么抓不到"的可执行体检、让 core 名字自动带函数名/文件/行号、给 `out` 与 `build` 各一个 `start.sh`、写全链路文档
- 改动：新增 `docs/ops/CRASH_DUMP.md`、`scripts/coredump/check_coredump_env.sh`、`scripts/coredump/collect_core.sh`、`scripts/run/out/start.sh`、`scripts/run/build/start.sh`；`enable_coredump.sh` 的 pattern 改为 `%e.%p.%t.sig%s`；`evs-edgevision.service` 改 `ExecStart=…/scripts/run/out/start.sh`；`COMMANDS.md` 加 §2.5 / §11.7 / §11.8 / §12
- 命令与结果：
  ```bash
  bash -n scripts/coredump/*.sh scripts/run/*/start.sh     # 8 个脚本全 syntax_ok
  scripts/coredump/check_coredump_env.sh                   # PASS=5 WARN=9 FAIL=0，rc=0
  scripts/coredump/collect_core.sh /tmp/evs_crash/core.75255 /tmp/evs_crash_probe -o /tmp/evs_arch
  scripts/coredump/collect_core.sh out/core.75879 out/bin/EdgeVisionService
  timeout --preserve-status -s INT 5 scripts/run/out/start.sh; echo RC=$?
  timeout --preserve-status -s INT 4 scripts/run/build/start.sh; echo RC=$?
  ```
  - 体检输出：`core_pattern='core'`（相对路径 + 无 %p/%t/%e）4 条 WARN、`ulimit -c 0/unlimited`、`/var/lib/evs/coredump` 不存在、`coredump_filter=0x33` 不含代码段、无 `sysctl.d`/`limits.d`/unit；`PASS=5 WARN=9 FAIL=0`
  - 探针 core 归档名：`core.evs_crash_probe.75255.1794659161.sig11.crash_in_worker@evs_crash_probe.cpp:17`，内含 `core`（inode 相同）/`meta.txt`/`crash_thread_bt.txt`/`backtrace_all_threads.txt`/`sharedlibs.txt`/`report.log`（按 pid 自动配对）
  - 真实 core 归档名：`core.EdgeVisionService.75879.1794659287.sig3.std::this_thread::sleep_for_…@this_thread_sleep.h:82`，`meta.txt` 含 `exe_build_id=5f74f0ea…`、`core_pattern=core`、`ulimit_c=0/unlimited`；`backtrace_all_threads.txt` 20,855 B
  - `out` 版 `start.sh`：打印 6 行环境（`ulimit -c=unlimited`、`EVS_CRASH_DIR=<root>/crash`、`core_pattern=core`）→ 4 路出帧（`实测=23.9~29.9fps`）→ `[crash] 崩溃处理已安装 … rlimit_core=enabled` → `RC=0`
  - `build` 版 `start.sh`：同样 4 路出帧并 `RC=0`；日志**无 `[crash]` 行**（`build/` 是 11-12 旧产物，不含崩溃采集）
- 结论：✅ 通过。core 名字现在可自动带「程序 + pid + 时间 + 信号 + 函数@文件:行号」，且两个构建目录都能一键启动；体检脚本能在复现前就判出"抓不到"
- 未做 / 受限：`enable_coredump.sh` 需 root 未执行（`core_pattern` 仍为 `core`）；`gcore_now.sh` 未实跑；systemd 单元未部署（§12 只给说明与命令）

### 5.21 L2 · T6 通信层：`CommBase` + `UdpComm` / `MqttComm` + 全局 `CommManager`（2026-11-14）

- 代码基线：`rk3588 @ c6d040c` + 工作树（`src/comm/` 未提交）
- 目标：给服务加通信层 —— 抽象基类 + 两种后端（UDP / MQTT）+ server/client 角色在初始化时可选 + 一个全局单例工厂，落在 `src/comm/`
- 改动（新增 12 文件，改 1 文件）：
  - `cmake/FindMosquitto.cmake`：`NO_DEFAULT_PATH` 只认 `install/<平台>`，找 `mosquitto/mosquitto.h` + `libmosquitto.so`，缺失 `FATAL_ERROR`（`install/rk3588` 已有 2.0.22，未新增任何第三方）
  - `src/comm/comm_types.{h,cpp}`：`CommType{kUdp,kMqtt}` / `CommRole{kServer,kClient}` / `CommState` / `CommConfig`（两后端字段并集）/ `CommFrame`（`data` 仅回调期间有效）/ 回调 + `ToString` / `ParseCommType` / `ParseCommRole`
  - `src/comm/comm_base.{h,cpp}`：模板方法 `Init/Start/Stop/Send/SendText` + 内部接收线程 + 状态机 + `last_error()`；纯虚 `DoInit/DoStart/DoStop/DoSend/ReceiveLoop`；`Stop()` 顺序 = `running_=false` → `DoStop()` → `join()`
  - `src/comm/udp_comm.{h,cpp}`：POSIX `socket` + `poll`；server = `bind` + 记住最近对端回包，client = `connect`；`DoStop()` 用 `shutdown` 唤醒 `poll`（使 `Start` 可重复）；pimpl 隐藏 `<netinet/in.h>`
  - `src/comm/mqtt_comm.{h,cpp}`：libmosquitto **C API** + pimpl（头文件只前向声明 `struct mosquitto;`）；`mosquitto_lib_init/cleanup` 进程级引用计数；`NO_CONN`/`CONN_LOST` 按 `reconnect_delay_ms` 重连并重订阅；通配订阅时发布主题自动落 `evs/v1/comm/evt/{server,client}`（带 `#` 的主题不能作为发布主题）
  - `src/comm/comm_manager.{h,cpp}`：`getInstance()` 单例 + `recursive_mutex`；`Create(CommConfig)` / `Create(type,role,host,port)` 工厂 + `<type>-<role>-<n>` 自动命名；`Find/Destroy/Start/Stop/StartAll/StopAll/Clear/names/size`；统一回调注入通道名
  - `src/comm/CMakeLists.txt`（`add_library(comm_api SHARED ...)` + `test_comm` 可执行）、`src/comm/README.md`、`src/CMakeLists.txt`（启用 `add_subdirectory(comm)`）、`scripts/tests/t6_comm.cpp`、`scripts/tests/run_t6_comm.sh`
- 命令与结果：
  ```bash
  cmake -S . -B out && cmake --build out --target test_comm -j8   # configure 打印 Found Mosquitto: install/rk3588/include；build rc=0
  make                                                            # rc=0；out/lib 多出 libcomm_api.so，out/bin 多出 test_comm
  ./scripts/tests/run_t6_comm.sh                                   # 无 broker：管理器 12 项 + UDP 17 项 PASS，MQTT 段 [SKIP]
  ./install/rk3588/sbin/mosquitto -c /tmp/evs_mosq_test.conf -d    # listener 1883 + allow_anonymous true
  LD_LIBRARY_PATH="out/lib:install/rk3588/lib" ./out/bin/test_comm # 43 项全 PASS、fail=0
  timeout 12 make run                                              # 4 路出帧 23.9~29.9fps、FreeMem: 释放 50 块、正常退出
  ```
  - 管理器段：`Clear()` 后为空 / `Create(udp,server)` OK / Init 后未 Start 时 `state=stopped` / 未 Start 就 `Send` 失败 / `Find` 返回同一对象 / 同名创建被拒且 `last_error="通道名已存在: probe-udp"` / `Destroy` 后 `Find=nullptr` / 空 name 自动命名 `udp-server-1` / `port=0` 与 UDP client 缺 host 均创建失败
  - UDP 段：39210 起找空闲端口 → client `ping` → server 收到且 `peer="127.0.0.1:<client 端口>"` → server `pong` 回给最近对端 → client 收到；重复 `Start` 幂等返回 true；`Stop` 两次后 `state=stopped`；`Destroy` 后注册表清空
  - MQTT 段：server 订阅 `evs/v1/#`、client 订阅 `evs/v1/comm/cmd/#` 并上报到 `evs/v1/media/evt/heartbeat`；client 发布 → server 收到且 `frame.topic="evs/v1/media/evt/heartbeat"`；server 发布 `evs/v1/comm/cmd/pong` → client 收到；`Stop` 后两端 `stopped`；broker 日志：两个客户端 `evs-comm-<pid>-1/2` 真实连上（p2, c1, k30）
- 断言分布：管理器 **12** 项 + UDP **17** 项 + MQTT **14** 项 = **43** 项；无 broker 时前 29 项 PASS、MQTT 段 SKIP（不计失败）
- 过程中修正的一处测试缺陷：最初让 client 发布到 `evs/v1/comm/cmd/ping`（落在自己订阅的 `evs/v1/comm/cmd/#` 内）→ MQTT 会把发布者的消息回灌给发布者，client 首帧是自收的 `hello-from-client`，断言「client 内容 = hello-from-server」FAIL。**这不是实现 bug**（MQTT 正常语义），改为上报/命令主题分离后全 PASS，并把这条例外写进 `src/comm/README.md` §3.1
- 结论：✅ 通过。通信层可独立使用（`libcomm_api.so`），UDP 回环与 MQTT pub/sub 回环均实测；主程序与 4 路链路的构建、出帧、`FreeMem` 零回归
- 未做 / 受限：**未接进主链路**（`main.cpp` 未创建通道）；未做 TLS、消息协议/分帧、UDP 多对端会话表、`epoll` 多路复用；MQTT 只测了 v3.1.1 + QoS 0（未测 QoS 1/2 与断线重连的真实 brokers 掉线场景）；`test_comm` 的 MQTT 段依赖本机 broker（无则 SKIP，不计失败）

### 5.22 L2 · 进程拆分：`evs_media` / `evs_comm` + `EVS_PROCESS` 进程可选编译（2026-11-14）

- 目标：媒体处理与通信处理拆成两个独立可执行；`cmake` 与 `make` 都能**只编其中一个进程**，不指定则都编。
- 代码改动：
  - `src/main/media/media_service.{h,cpp}`：把原 `main.cpp` 的编排（崩溃 handler → 配置 → `MemInit` → `build_all` → 信号 → `start_all` → 有序停止 → `FreeMem`）抽成 `evs::media::Run()`；日志前缀保持 `[main]`（既有 TESTLOG 期望逐行不变）。
  - `src/main/media/main_media.cpp`：`evs_media` 入口（argv → app_name `evs_media` → `Run()`）。
  - `src/main/comm/main_comm.cpp`：`evs_comm` 入口 —— 读 `config/comm.json` / `../config/comm.json`（或 `argv[1]`）→ `CommManager::Create` → `SetReceiveCallback` / `SetStateCallback` → `StartAll` → 等信号 → `StopAll` + `Clear`。**配置语法/取值错误退出码 1**；配置缺失 / 无通道 / 单通道启动失败**不退出**。
  - `src/main/CMakeLists.txt`：新增 `EVS_PROCESS`（CACHE STRING，默认 `all`；取 `media | comm | media,comm`）→ `EVS_WITH_MEDIA` / `EVS_WITH_COMM`；未选中的可执行设 `EXCLUDE_FROM_ALL`；非法值 `FATAL_ERROR`。
  - `src/main/main.cpp`：降为兼容壳（`EdgeVisionService` → `evs::media::Run()`）。
  - 顶层 `Makefile`：`media` / `comm`（单 target）、`proc PROC=...`（改默认集 + reconfigure + 构建）、`run-media` / `run-comm`、`info` 打印 `EVS_PROCESS`、`help` 增「进程」段。
  - `config/comm.json`：示例通道 `cmd`（`mqtt` / `server` / `127.0.0.1:1883` / `evs/v1/#`）。
- 构建验证：
  | 命令 | 结果 |
  | --- | --- |
  | `make`（默认 all） | rc=0；`out/bin` 四个可执行：`evs_media` / `evs_comm` / `EdgeVisionService` / `test_comm` |
  | `cmake -S . -B /tmp/evs-pm -DEVS_PROCESS=media` + build | rc=0；`bin` 只有 `evs_media` + `EdgeVisionService`；打印 `主进程选择: EVS_PROCESS=media -> media=TRUE comm=FALSE` |
  | `cmake -S . -B /tmp/evs-pc -DEVS_PROCESS=comm` + build | rc=0；`bin` 只有 `evs_comm`（+ `test_comm`） |
  | 在 `-DEVS_PROCESS=media` 的树里 `cmake --build <树> --target evs_comm` | rc=0 —— 未选中 ≠ 不存在，单 target 随时可编 |
  | `make media` / `make comm` | rc=0（不重新 configure，只编对应 target） |
  | `make proc PROC=comm` → `make info` → `make proc PROC=all` | 全 rc=0；中间 `EVS_PROCESS = comm`，恢复后 `= all` |
  | `cmake -S . -B /tmp/evs-bad -DEVS_PROCESS=foo` | rc=1；报 `EVS_PROCESS 不认识 'foo'；可选值：all \| media \| comm \| media,comm` |
- 运行验证：
  - `timeout --preserve-status -s INT 14 make run-media` → 4 条链建成（`ch0: vi0→vpss0→rtsp0→print0` …）、`[print3] 实测=23.82fps`、`[MemManager] FreeMem: 释放 50 块 / 100 个槽位`、`[main] FreeMem 完成，进程正常退出`；ZLM 日志前缀 `[evs_media]`（app_name 生效）。
  - `timeout --preserve-status -s INT 14 make run`（兼容壳）→ 同样 4 路 + 释放 50 块 + 正常退出；ZLM 前缀 `[EdgeVisionService]`。**与拆分前输出一致 ⇒ 薄壳回归通过**。
  - `make run-comm`（无 broker）→ `[comm] 配置来源: ../config/comm.json`、`通道 1 个`、`通道就绪 cmd（mqtt / server / 127.0.0.1:1883）`、`状态 → error`、`启动通道 cmd 失败: … Connection refused（broker 起了吗？…）`、`运行中（Ctrl+C 退出）`；SIGINT 后 `StopAll` + `Clear` → `通道已清理，进程正常退出`（直接跑二进制 **rc=0**）。
  - `make run-comm`（临时 broker：`./install/rk3588/sbin/mosquitto -c /tmp/evs_mosq_test.conf -d`）→ `通道=cmd → running`、`1 个通道已启动`；同时跑 `out/bin/test_comm` → `evs_comm` 打出 `[comm] 收包 通道=cmd topic=evs/v1/media/evt/heartbeat peer=127.0.0.1:1883 size=17 body="hello-from-client"` 与 `topic=evs/v1/comm/cmd/pong … body="hello-from-server"`；`test_comm` 仍 43 项 PASS、`fail=0`。
- 结论：✅ 通过。媒体行为与拆分前逐行一致（薄壳回归）；通信进程可独立启停，收包路径端到端可用（broker → `evs_comm` → 回调打印）。
- 未做 / 受限：`evs_media` **尚未**接入 `comm_api`（不建通道、不发 `media/evt/*`）；`evs_comm` 不拉起/守护 broker；无进程间消息编解码与命令路由、无心跳超时判定；`src/main/test/` 仍未接入构建；`evs_upgrade` 未创建。
- 收尾回归（同日）：`make` rc=0（`out/bin` = `EdgeVisionService` / `evs_comm` / `evs_media` / `test_comm`）；`bash scripts/tests/run_t6_comm.sh` rc=0（UDP 段全 `[PASS]`、MQTT 段因本机无 broker `[SKIP]`，`fail=0`）。

### 5.23 L2 · `make run-*` 按下 Ctrl+C 后的收尾：去掉误导性的 `*** Interrupt`（2026-11-14）

- 现象：关闭程序（Ctrl+C）后，日志在程序自己的「进程正常退出」之后多出一行 `make: *** [Makefile:131: run-media] Interrupt`；`timeout --preserve-status -s INT … make run-media` 退出码 **130**。看起来像崩溃，实际程序已 `return 0`。
- 根因（三层，缺一层都不成立）：
  1. 运行目标的配方是 `@cd $(BUILD) && $(RUN_ENV) ./bin/…` —— 有 `&&` 就必须有 shell；
  2. Ctrl+C 把 SIGINT 发给**整个前台进程组**：make、外层 shell、程序三者都收到；
  3. 程序自己 `signal(SIGINT, OnSignal)` → 有序停止 → `return 0`；但外层 shell 被 SIGINT 打死 → make 判定该命令「被中断」→ 打印 `Interrupt`；make 自身也被 SIGINT 打断 → `timeout --preserve-status` 原样转出 130。
- 修复：`run` / `run-app` / `run-media` / `run-comm` / `run-node` 配方末尾加 `exec` —— shell 被程序**替换**，make 的直接子进程就是程序本身（正常退出 0），不再有 shell 被打死 → `Interrupt` 行消失。原因写在 `Makefile` 的 `RUN_ENV` 注释里（防后人删 `exec`）。
- 验证（两种等价 Ctrl+C：`os.killpg(pgid, SIGINT)`、`timeout --preserve-status -s INT`；2026-11-14）：
  | 命令 | `Interrupt` 行 | make / 程序退出码 |
  | --- | --- | --- |
  | `make run-media`（改前） | 1 行 | 130 |
  | `make run-media`（改后） | 0 行 | 130（make 被打断）/ 程序 0 |
  | `make run-comm`（改后，无 broker） | 0 行 | 130 / 程序 0 |
  | `make run`（改后，兼容壳） | 0 行 | 130 / 程序 0 |
  | `scripts/run/out/start.sh`（脚本本身 `exec`） | — | **0** |
  | 直跑 `out/bin/evs_comm ../config/comm.json` | — | **0** |
  三次 `make run-*` 的尾部都仍是 `[main] FreeMem 完成，进程正常退出` / `[comm] 通道已清理，进程正常退出`；`grep -c 'Interrupt'` = 0。
- 顺带纠正文档旧结论：`timeout --preserve-status -s INT … make run-*` **不可能**是 0（SIGINT 打在 make 上），`docs/ops/COMMANDS.md` §2.1 / §2.3 / §10 已改为「make 形式 130、程序自身 0、要看程序 rc 用 `start.sh` 或直跑二进制」，§9 踩坑表加了三行（Interrupt / 130 / `make run` 与 `run-media` 输出相同）。
- 用户提问澄清：`make run` 与 `make run-media` 输出逐行相同，是因为两者调用**同一个** `evs::media::Run()`（`EdgeVisionService` 只是历史兼容名），差异只有崩溃报告里的 `app_name`（`EdgeVisionService` / `evs_media`）与 ZLM 日志前缀；`evs_comm` 是另一个进程，打 `[comm]` 前缀。
- 改动范围：只改 `Makefile`（4 个配方 + 注释）与文档，**未动 `src/`**，所以本机最后一次代码构建仍是 `0117907`，无需重编。
- 结论：✅ 通过。

## 6. 维护约定

1. 提交代码时，同一 commit 里**必须**包含对应的 TESTLOG 记录（L1 或 L2）。
2. 本文只写**测试证据**；方案写 `DESIGN_MULTI_PIPELINE.md`，现状写 `STATUS.md`，规矩写 `HANDOFF.md`，**已定案 bug 的原理剖析与面试要点写 `BUG_CASES.md`**（分工见 HANDOFF R5）。
3. 本文超过 ~400 行时，把最早的 L1 行折叠成一行汇总（"2026-11 共 N 次构建通过"），L2/L3 明细保留。
4. 若某条记录后来被推翻（例如复测失败），**不改旧记录**，追加新记录并注明「推翻 5.x」。
