# EdgeVisionService

> 边缘视觉服务原型：**配置驱动装配 → 采集 → 统一内存池 → 节点流水线 → 硬件/软件编码 → RTSP 推流**。
> 主平台 RK3588（MPP / V4L2 / RGA），x86_64（FFmpeg）作为开发调试目标。
> 当前阶段：**单条链路已能真机出帧并优雅退出，多链路装配已跑通、4 路同时出帧待 T7**；详细现状见 [docs/STATUS.md](docs/STATUS.md)。

## 目录

1. [现在能做什么](#1-现在能做什么)
2. [架构一图流](#2-架构一图流)
3. [快速开始](#3-快速开始)
4. [当前状态](#4-当前状态)
5. [路线图](#5-路线图)
6. [文档导航](#6-文档导航)
7. [开发约定](#7-开发约定)

## 1. 现在能做什么

- **配置驱动编排（T5/T6，已可跑）**：`config/node.json`（v1）描述 `global` + `pipelines[]`；`NodeConfigManager` 负责解析、字段级校验（错误带字段路径）、`${global.*}` 引用展开与默认值合并；`PipelineBuilder` 负责拓扑校验（R1–R5）→ `NodeFactory` 造节点 → 连队列 → 批量启停。
- **真实入口是配置编排，不是手写装配**：`src/main/test/main_node.cpp`（可执行名 `testnode`）只做五件事 —— 读配置 → 建进程级资源（内存池）→ `build_all` → 等信号 → `stop_all` + `FreeMem`。代码里没有节点名、没有设备路径、没有 `while(1)`。
- **SIGINT 优雅退出（T6，已实测）**：`Ctrl+C` / `SIGTERM` → 按链路**正序**（采集 → 推流 → 打印）停线程 → 释放 ZLMediaKit 流与服务 → `FreeMem` → 退出码 0，无残留进程。
- **采集**：`FfmpegCapture` 支持本地文件、摄像头、RTSP/RTMP/HTTP 输入；`V4l2Capture` 面向 RK 的 `/dev/video*`。两者统一输出 NV12。
- **内存池与流水线**：`MemManager::MemInit(50)` 预建 50 个 1920×1080 NV12 块；节点间用 `MemQueue`（深度 20，满则丢最老帧）连接。⚠️ 这两个数字当前仍写死在代码里，`global.pool.blocks / queue_depth / quota_per_channel` 尚未被读取（T10a）。
- **硬件编码**：`MppEncoder` 支持 H.264/H.265，NV12 输入、128 字节 stride 对齐、固定 QP 33、GOP 150，回调输出裸流。
- **RTSP 推流**：通过 ZLMediaKit C API 起 RTSP 服务、建流、送裸流，播放地址 `rtsp://<板端IP>:8550/live/<stream>`（端口/app/stream/codec/fps/码率均可配）。
- **耗时统计**：每个块挂 `profile::NodeProfiler`，各节点记录自身耗时，终点 `PrintNode` 周期打印「各节点耗时 + 合计 + 首尾跨度 + 实测 fps」。
- **可重跑的自检脚本**：`scripts/tests/` 四套（T1 RTSP 键 18 项、T2 配置解析 53 项、T3 `node.json` 104 项、T5 装配 64 项断言），全部无需硬件。

当前限制（每条都有对应任务，不是"大概不行"）：

- **只有一条链能真出帧**：`ViNode` 仍硬编码 `CAPTURE_V4L2` + `/dev/video0`（P0-2 / **T7**），所以 4 路配置里 ch1~ch3 打不开各自源；`vi.width/height` 也未生效。
- `FfmpegCapture` 不循环、不缩放（**T7b**），mp4 播完即停。
- 一个源扇出到多个下游时**块引用计数未实现**（P0-5 / T9），故当前是"每路独立源"，不是一源多流。
- RGA 预处理、OSD、录像、统一日志 / 指标 / 健康检查均未落地；媒体服务仍由 `PushNode` 内部启停（**T4** 才上提到 `MediaServer`）。
- x86 本机构建仍会失败（`install/x86_64` 为空，P0-1）；FFmpeg 软编/软解未完成。

> 模块完成度、P0–P2 明细与优化建议见 [docs/STATUS.md](docs/STATUS.md)；多链路的决策与任务清单见 [docs/DESIGN_MULTI_PIPELINE.md](docs/DESIGN_MULTI_PIPELINE.md)。

## 2. 架构一图流

```text
config/node.json（v1：global + pipelines[]）
        │
        ▼
NodeConfigManager ── 解析 / 字段级校验 / ${global.*} 展开 / v0 push.json 适配
        │
        ▼
PipelineBuilder ──── 拓扑校验(R1–R5) → NodeFactory 造节点 → link_to 连线 → 批量 start/stop
        │
        ├─ ch0  ViNode ──MemQueue(20)──▶ [VpssNode] ──▶ PushNode ──▶ ZLMediaKit RTSP  rtsp://<ip>:8550/live/ch0
        ├─ ch1  ViNode ──MemQueue(20)──▶ PushNode ──▶                    .../live/ch1
        ├─ ch2  ViNode ──…                                                        …
        └─ ch3  ViNode ──…                                                        …
                   终点 PrintNode：打印各节点耗时 + 实测 fps，并归还 MemBlock
                                   │
                                   ▼
                    共享内存池 MemPool（50 × 1920×1080 NV12，全进程一份）
```

链路由 `config/node.json` 决定：节点数量、顺序、参数都写在配置里（`[VpssNode]` 是可选的中间处理节点，ch0 配了、ch1~ch3 没配）。

进程级资源由入口建、由入口拆：`load 配置 → MemInit → build_all → start_all → 等 SIGINT → stop_all → sets.clear() → FreeMem`。

> 顺序是硬约定：`stop_all` 之后必须 `sets.clear()`（析构节点、释放队列）再 `FreeMem`（销毁池）；反过来会用到已销毁的块。详见 [docs/HANDOFF.md](docs/HANDOFF.md) §1.7。

当前每条链是**独立源**（一链一个 `ViNode`）；一源多下游（主/子码流、算法、录像）依赖块级引用计数（P0-5 / T9）。分层、内存模型、线程/背压与平台接口详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)；多链路方案取舍见 [docs/DESIGN_MULTI_PIPELINE.md](docs/DESIGN_MULTI_PIPELINE.md)。

## 3. 快速开始

### 3.1 依赖

| 依赖 | 期望位置 | 仓库里有什么 |
| --- | --- | --- |
| FFmpeg 5.x（头文件 + `.so`） | `install/<platform>/{include,lib}` | **不入库**（在 `.gitignore` 里），需自备 |
| Rockchip MPP | 同上 | 同上（`librockchip_mpp.so`） |
| ZLMediaKit（`libmk_api.so`） | 同上 | 同上 |
| RGA | 同上 | 同上（代码尚未使用） |
| FreeType（**可选**，OSD 矢量字体） | 系统库或 `install/<platform>` | 未提供。默认 `BUILD_FREETYPE_OSD=OFF`，OSD 只用内置点阵字体（`.evsfont`） |
| 交叉工具链（**可选**） | `cmake/rk3588.toolchain.cmake` | 路径需按本机实际安装修改 |

> ⚠️ `install/`、`3rdparty/`、`build/` 全在 `.gitignore` 里且**从未提交过**（`git ls-files install` 输出为空）。clone 下来是纯源码，必须自己把依赖树放到 `install/rk3588/`，否则 `cmake -S . -B build` 会在 `find_package(FFMPEG)` 处直接失败。

### 3.2 板端本机构建（aarch64，当前默认路径 · 已实测）

在 RK3588 板子上直接编——这是本项目的日常路径：`aarch64` 会被自动识别成 `TARGET_PLATFORM=rk3588`，不需要工具链文件。

```bash
cmake -S . -B build
cmake --build build -j"$(nproc)"
```

产物在 `build/lib/*.so` 与 `build/bin/`。当前**启用**的可执行目标只有三个：

| 目标 | 源码 | 说明 |
| --- | --- | --- |
| `EdgeVisionService` | `src/main/main.cpp` | 空壳，只打印一行（冒烟用） |
| `testnode` | `src/main/test/main_node.cpp` | **真实主链路**：配置 → builder → SIGINT 优雅退出 |
| `test_builder` | `scripts/tests/t5_builder.cpp` | T5 装配自检（只 build 不 start，不碰硬件） |

`build/bin/` 里若还有 `test_encoder` / `test_pushStream` / `test_decoder` / `testpool`，那是旧产物：对应源码在 `src/main/test/CMakeLists.txt` 中已注释或与当前 API 不符，**别用**。

> 是否要 `rm -rf build`：先看 `build/CMakeCache.txt` 里的 `CMAKE_HOME_DIRECTORY` 是否等于本机源码路径；一致就可以直接复用。

### 3.3 RK3588 交叉编译（可选，非当前路径）

```bash
cmake -S . -B build-rk \
      -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake
cmake --build build-rk -j"$(nproc)"
```

注意：

- 工具链默认写死 `/usr/aarch64-linux-gnu/bin/aarch64-linux-gnu-g++`，需改成实际编译器路径（例如 `/usr/bin/aarch64-linux-gnu-g++-11`）。
- 未设置 `CMAKE_SYSROOT`；板端 sysroot 与 glibc 版本需先确认。
- 一旦传了 `CMAKE_TOOLCHAIN_FILE`，`TARGET_PLATFORM` 会被直接钉成 `rk3588`（不看宿主架构）。

### 3.4 x86_64 本机构建（当前会失败，P0-1）

```bash
cmake -S . -B build-x86
```

> `cmake/FindFFMPEG.cmake` 用 `NO_DEFAULT_PATH` 只查 `install/x86_64`，而该目录当前是空的 → 配置阶段直接失败。开发机要跑起来需先补 x86 依赖，或给 `FindFFMPEG.cmake` 加 `pkg-config` fallback（见 [docs/STATUS.md](docs/STATUS.md) P0-1）。

### 3.5 运行与优雅退出

```bash
# 方式一：板端自带的便利脚本（build/run.sh 不入库，删掉 build/ 后需自建，见 docs/HANDOFF.md §1.5）
./build/run.sh testnode

# 方式二：不依赖 run.sh 的等价写法
export LD_LIBRARY_PATH="$PWD/build/lib:$PWD/install/rk3588/lib:$LD_LIBRARY_PATH"
./build/bin/testnode "$PWD/config/node.json"
```

`run.sh` 会 `cd` 到 `build/`、导出 `LD_LIBRARY_PATH`，然后 `exec bin/<name> "$@"`——因为用了 `exec`，终端的 `Ctrl+C` 直接送到二进制。

**读哪份配置**（`NodeConfigManager::load_with_fallback`；候选路径相对当前工作目录，而 `run.sh` 的 cwd 是 `build/`）：

| 命令 | 实际读到 | 跑几条链 |
| --- | --- | --- |
| `./build/run.sh testnode` | 依次试 `config/node.json`、`../config/node.json` → 命中后者 | **4 条**（v1 `config/node.json`） |
| `./build/run.sh testnode ../config/push.json` | `argv[1]` 显式指定，只认它 | **1 条**（v0 单链路） |
| `./build/run.sh testnode /abs/path/node.json` | 绝对路径 | 按该配置 |

**期望输出**（4 路配置，实测）：

```text
[main] global: rtsp 端口 8550 / app "live" / pool.blocks 64 / queue_depth 8
[main] 注意: pool.blocks / queue_depth / quota_per_channel 尚未接线（T10a），当前仍用 MemInit() 默认值
[builder] 链路就绪: ch0[0] vi0 -> vpss0 -> rtsp0 -> print0
[builder] 汇总: 建成 4 条, 失败 0 条, 跳过 0 条
[main] 4 条链路已启动（Ctrl+C 退出）
[print0] ……各节点耗时 + 实测 fps（ch0 约 30fps）
^C
[main] 收到退出信号，开始有序退出
[vi0] VI Node Stopped.          ← 正序：采集 → 推流 → 打印
[rtsp0] Push Node Stopped.
[print0] Print Node Stopped.
[main] FreeMem 完成，进程正常退出
```

退出码 0、无残留进程。`ch1~ch3` 会报 `open file failed:/dev/video0`——这是 `ViNode` 硬编码同一个设备导致的（P0-2 / T7），**不是配置写错**。

自动化 / 长跑验证用 `timeout` 代替手按 Ctrl+C：

```bash
timeout --preserve-status -s INT 12 ./build/run.sh testnode ../config/push.json; echo "EXIT=$?"
```

播放：`ffplay rtsp://<板端IP>:8550/live/ch0`（`stream` 每路不同：`ch0`~`ch3`）。只调采集不推流时，把配置里的 `push.enable` 设为 `false`。

### 3.6 自检脚本

```bash
./scripts/tests/run_t1_zlm_rtsp_key.sh   # RTSP key / ref_cnt / 入参校验，18 项断言
./scripts/tests/run_t2_node_config.sh    # NodeConfigManager 解析/校验/v0 适配，53 项断言
./scripts/tests/run_t3_node_json.sh      # config/node.json 结构与字段，104 项断言
./scripts/tests/run_t5_builder.sh        # NodeFactory + PipelineBuilder，64 项断言
```

四套脚本都会自己 `cmake --build` 出所需 target（前提是 `install/` 已就绪），工作目录固定为仓库根，结尾打印 `passes=N fails=0`。

- **不需要摄像头/采集设备**，也不起业务流水线；T2/T3/T5 纯逻辑与配置。
- **T1 例外**：它真的会起停一个 ZLMediaKit RTSP 服务（默认端口 `18550`，避开现场 8550），并用 `ss` 观察端口是否释放。

每次真实验证的结论都记在 [docs/TESTLOG.md](docs/TESTLOG.md)（L1 一行 / L2 小节），**没记录的"已通过"视为未验证**。

## 4. 当前状态

| 模块 | 状态 | 说明 |
| --- | --- | --- |
| 配置管理与装配 | 🟢 85% | T2/T5：v1 解析 + 字段级校验（带字段路径）+ `${global.*}` 展开 + `NodeFactory` + 拓扑校验 + 批量启停；`global.pool.*` 尚未接线（T10a） |
| 进程编排 / 主程序 | 🟢 75% | T6：`testnode` = 配置 → builder → SIGINT 优雅退出 → `FreeMem`，退出码 0（实测）；`src/main/main.cpp` 仍是空壳；媒体服务未上提（T4） |
| 内存池 / 流水线 | 🟡 65–80% | 单消费者链路可跑；块级引用计数未做（P0-5 / T9）；池块数与队列深度硬编码 50/20（T10a） |
| 采集 | 🟡 65–75% | FFmpeg 文件/摄像头/网络可用但不循环、不缩放（T7b）；V4L2 参数硬编码，`ViNode` 仍写死 `CAPTURE_V4L2` + `/dev/video0`（P0-2 / T7） |
| 编码 | 🟡 70% | MPP H.264/H.265 可跑；x86 软编未完成；每帧 `memcpy`，无 EOS flush |
| RTSP 推流 | 🟡 65% | 单路可演示；P0-6 / P0-7 已修（T1）；多路并存与"停一路不影响其他路"待 T4 |
| 测试 / 文档 | 🟡 35–40% | `scripts/tests/` 四套可重跑验收脚本（共 239 项断言）+ TESTLOG 留痕制度；仍无 CTest / CI |
| 预处理 / OSD / 录像 / 算法 | 🔴 10–40% | `VpssNode` 骨架、预处理框架（转码 / 叠加 / 绘制）有代码；业务逻辑与 RGA 未落地 |

> P0 共 12 项，已修 5 项（P0-3 / 4 / 6 / 7 / 12），剩 7 项。完整模块表、P0/P1/P2 明细与优化建议见 [docs/STATUS.md](docs/STATUS.md)。

## 5. 路线图

任务编号（T1~T19）与验收标准见 [docs/DESIGN_MULTI_PIPELINE.md](docs/DESIGN_MULTI_PIPELINE.md) §7。

| 里程碑 | 目标 | 任务与进度 |
| --- | --- | --- |
| **M1**（P0 必做） | 4 路能跑、媒体服务脱节点 | T1 ✅ · T2 ✅ · T3 ✅ · T5 ✅ · T6 ✅ → **下一个 T7**（`ViNode` 解析 `vi.src`）→ T7b（循环+缩放）→ T4（`MediaServer`）→ T10a（池参数可配）→ T8（删 `push.json`）|
| **M2**（P1） | 多路真正稳定 | T9 块级引用计数 · T10b 池规格化 · T11 `Logger` · T12 `MetricsRegistry` · T13 `DeviceRegistry` · T14 `ResourceBudget` · T15 校验器增强 |
| **M3**（P2，可选） | 可测试、可运维 | T16 `ServiceKernel` · T17 配置热加载 · T18 `Supervisor` · T19 多进程演进评估 |

M1 的三条硬验收（当前**未**全部满足）：

1. `config/node.json` 启动 4 路（ch0 摄像头 + ch1~ch3 三个 mp4 循环），4 个 `rtsp://<ip>:8550/live/chX` 连续出帧 **30 分钟**。
2. 停掉 ch0 的推流**不影响** ch1~ch3 的 RTSP 服务。
3. `Ctrl+C` → `stop_all → MediaServer::shutdown() → FreeMem` 顺序正确，`valgrind --leak-check=full` 无 definite leak。

> 长期 WBS、12 周排期、技术方案、验收指标与风险矩阵见 [docs/ROADMAP.md](docs/ROADMAP.md)。

## 6. 文档导航

| 想了解什么 | 看哪里 |
| --- | --- |
| **接手项目：现状快照、必须遵守的规定、常见任务流程** | **[docs/HANDOFF.md](docs/HANDOFF.md)**（新接手先读这篇） |
| AI 助手自动加载入口（Cline / 其他 agent） | [`.clinerules/00-handoff.md`](.clinerules/00-handoff.md)、[`AGENTS.md`](AGENTS.md)（均指向 `docs/HANDOFF.md`） |
| 当前架构、目录、数据流、内存/线程模型、平台接口 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 模块完成度、已实现/占位、P0–P2、优化建议 | [docs/STATUS.md](docs/STATUS.md) |
| 多链路（一进程多 RTSP）的方案取舍、决策记录、任务与验收标准 | [docs/DESIGN_MULTI_PIPELINE.md](docs/DESIGN_MULTI_PIPELINE.md) |
| **测试怎么做、某条"已通过"当初怎么验的** | **[docs/TESTLOG.md](docs/TESTLOG.md)**（L1 冒烟表 + L2/L3 明细） |
| 长期 WBS、排期、技术方案、风险与待确认项 | [docs/ROADMAP.md](docs/ROADMAP.md) |
| 源码入口 | `src/`；真实主链路在 `src/main/test/main_node.cpp`，节点在 `src/pipeline/` |
| 配置 | `config/node.json`（v1，4 路，当前主用）、`config/push.json`（v0 单链路，T8 删除） |

## 7. 开发约定

**红线（违反即错误交付）**

1. **C++14**：`set(CMAKE_CXX_STANDARD 14)`。不用 `std::any` / `if constexpr` / 结构化绑定等 C++17+ 特性。
2. **不引入 `install/` 里没有的新第三方库**；不重命名既有目录（`moudle_vi` / `moudle_push` / `moudle_print` 是历史拼写，改名属独立任务）。
3. **取块必有释放**：`acquire()` / `getBlock()` 之后，所有失败分支都必须 `release()`，否则池子会被几帧内耗干。
4. **改了代码/配置必须留测试记录**：按 [docs/TESTLOG.md](docs/TESTLOG.md) 分级写（L1 一行冒烟 / L2 任务小节 / L3 里程碑）；没记录的"已通过"视为未验证。
5. **不提交** `build/`、`install/`、`3rdparty/`、`.vscode/`（已在 `.gitignore`）；不 `git push --force`、不 `git reset --hard`、不碰 `main`。

**写法约定**

- 新节点继承 `PipelineNode`：`push_input()` 只入队，耗时处理放 `run()`，处理完 `dispatch_to_children()`，终点 `release_block()`。
- 新节点要在 `NodeFactory` 注册 `type` 名，并在 `config/node.json` 里可用；装配交给 `PipelineBuilder`，**不要在手写入口里 `make_shared` + `link_to`**。
- 配置统一走 `config/`；取值非法要明确报错（带字段路径），**不要静默忽略**。新增配置字段如果还没接线，入口要打印"尚未接线"，别让人以为生效了。
- 提交信息：`<模块>: <动词> <对象>`，例如 `pipeline: 新增 NodeFactory`、`main: 重写真实入口`、`rtsp: 修复 P0-6`、`config: 定稿 node.json`、`docs: 坐标回写`。
- 合并前：板端构建通过、`scripts/tests/` 四套全绿、无新增编译告警。

> 完整红线、构建坑、内存池/节点 API 速查见 [docs/HANDOFF.md](docs/HANDOFF.md) §1.7 与 §2。

---

最后更新：2026-11-09；适用分支：`rk3588`；代码校准点：`29ab58b`（T6 重写真实入口）。
