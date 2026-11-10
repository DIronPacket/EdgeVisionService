# EdgeVisionService 接手工作流

> **给谁看**：接管本项目的 AI 助手或新加入的工程师。
> **解决什么问题**：切换对话 / 换人后，不用重新读全部代码就能在 10 分钟内安全动手。
> **怎么用**：先读 §0 与 **第一部分（当前项目情况）**，动手前必读 **第二部分（必须遵守的规定）**，具体任务照 **第三部分（操作规程）** 执行。
>
> 本文只写"接手要知道什么、不许做什么、按什么流程做"。
> 架构细节 → [ARCHITECTURE.md](ARCHITECTURE.md)；现状与 P0–P2 问题 → [STATUS.md](STATUS.md)；未来计划 → [ROADMAP.md](ROADMAP.md)。
> **事实冲突时的优先级：实际代码 > STATUS.md > ARCHITECTURE.md > 本文 > README > ROADMAP.md。**
>
> 最后更新：2026-11-10；适用分支：`rk3588`；校准点：`15cf975`。
> 纯 `docs:` 提交不算"基线漂移"，不需要回写校准点；只有 `src/`、`CMakeLists.txt`、依赖或构建方式变化时才更新本文。

## 0. 30 秒事实卡（**只想知道现状 → 读完本节即可停止**）

| 项 | 值 |
| --- | --- |
| 项目是什么 | RK3588S 边缘视觉原型：采集 → 内存池 `MemBlock` → `PipelineNode` 流水线 → MPP 编码 → ZLMediaKit RTSP 推流 |
| 主线状态 | 单链路出帧可跑通、进程编排已配置驱动（T5/T6）、**采集源已配置化**（T7：`vi.src` 前缀选后端，4 路同时出帧已实测），未产品化（仍无日志 / 单测 / CI）；mp4 loop 待 T7b |
| 分支 / 基线 | `rk3588`（跟踪 `origin/rk3588`）· 工作树干净 · 与 `origin/rk3588` 同步（无未推送提交）· 本文**代码**校准点 `15cf975`（P0-9 / P0-11 修复 + P0-10 ①② RAII 收口：`FrameBuffer` 所有权、`MemMeta` 槽位自动销毁）· 前一代码校准点 `114c94d`（P0-14：两个采集后端写块前重绑 `FrameBuffer` 视图，实际修复 `eee3b01`）、再前 `c557d89`（P0-13 `MppEncoder` 按平面拷贝 NV12）、`eac4bb0`（T7）、`29ab58b`（T6）；其后只改文档 / JSON 注释的 `docs:`、`config:` 提交不算漂移） |
| 构建（已实测通过） | `cmake -S . -B build && cmake --build build -j"$(nproc)"` |
| 冒烟运行 | `./build/run.sh EdgeVisionService` → 期望输出 `EdgeVisionService start` |
| 真实主链路 | `src/main/test/main_node.cpp`（可执行名 `testnode`）—— **T6 起由配置驱动**：`load_with_fallback` → `MemInit` → `PipelineBuilder::build_all` → SIGINT → `stop_all` → `FreeMem`；`src/main/main.cpp` 只是空壳 |
| 语言 / 环境 | **C++14**（禁用 17+）· aarch64 板端本机编译 · cmake 3.22 · g++ 11.4 |
| 最大问题 | P0 共 14 项（P0-2 / 3 / 4 / 6 / 7 / 12 / 13 / 14 已修复；**P0-5 于 2026-11-10 核实为非缺陷**；**P0-9 / P0-11 于 2026-11-10 修复**，同日 `FrameBuffer` 补 RAII 收口 P0-10 的 ①②；剩 3 项 = P0-1 x86 FFmpeg 路径 / P0-8 解码器写栈 / P0-10 残余（仅 `capacity` 语义）），见 [STATUS.md §4.1](STATUS.md) |
| 五条硬约束 | C++14 不变 · 不引新库 · 不改目录名 · 不 force push · 取块必有 `release` |

## 0.1 按需跳读表（全文约 10k token，**不要默认全文读**）

| 你要做的事 | 读哪些章节 | 大致 token |
| --- | --- | --- |
| 只想了解现状 | §0 事实卡 + §1.2 快照 | ~800 |
| 要动手改代码 | §0 + §1.2 + §1.4 目录 + §1.7 API + **§2 全部规定** | ~4k |
| 修某个 P0/P1 | §1.9 索引表 + §1.5 构建坑 + R1 + R4 + STATUS 对应行 | ~2k |
| 加一个新节点 | §2.4 内存规定 + R2 + §1.7 | ~1.5k |
| **复盘某个 bug 为什么发生 / 面试怎么讲** | 不在本文：用 **`docs/BUG_CASES.md`**（原理剖析 + 函数级位置 + 定位手法 + 面试说法；修 bug 后必须追加） | ~1k |
| 首次全量接手 / 换人 | 全文（约 450 行） | ~10k |

> 章节行号自查（编辑后行号会漂移）：`grep -nE '^#{1,2} ' docs/HANDOFF.md`

> 章节标题自查：`grep -nE '^#{1,2} ' docs/HANDOFF.md`（本文编辑后行号会漂移，先用这条命令刷新再定向读）。
> 需要**文件清单**时看 §1.4，需要**当前状态**看 §0/§1.2，需要**限制**看 §2 —— 这三处覆盖了 90% 的接手问题，不必 `search_codebase` 全仓扫描。

## 0.2 接手三步（新对话开头照做）

| 步骤 | 动作 | 目的 | 耗时 |
| --- | --- | --- | --- |
| 1 | 读 §0 事实卡 + §1.2 快照 | 建立事实认知：分支、环境、构建、主链路 | 1 min |
| 2 | 跑 **附录 A** 的自检命令 | 确认代码与本文描述一致（git / build / 依赖） | 1 min |
| 3 | 动手前读 **§2 全部规定** | 避免违反项目红线（C++14、依赖、命名、git） | 3 min |

**不要做的事**：不要用 `search_codebase` 全仓扫描来"重新了解项目"——文件清单在 §1.4，现状在 §0/§1.2，限制在 §2，索引在 §1.9。

**不知道该看哪个文件？** 直接查 §1.9「任务 → 文件」索引表。

---

# 第一部分　当前项目情况

## 1.1 一句话定位

边缘视觉服务原型：**V4L2/FFmpeg 采集 → 统一内存池 `MemBlock` → `PipelineNode` 节点流水线 → MPP 硬编码 → ZLMediaKit RTSP 推流**。
主目标平台 RK3588S（本机即板端，原生 aarch64 编译）；x86_64 是规划中的开发调试目标，**当前不可用**。

当前阶段：**原型链路已跑通，未产品化**。推流参数已由 `config/push.json` 配置化（采集源路径、编码 QP 仍未配置化），没有日志/指标、没有单测/CI。

## 1.2 关键坐标（2026-11-10 快照）

| 项 | 值 |
| --- | --- |
| 本地分支 | 只有 `rk3588`，跟踪 `origin/rk3588`；工作树干净，**已与远程同步（无未推送提交）** |
| HEAD | `15cf975 common: 修 P0-9/P0-11 并收口 P0-10 —— FrameBuffer 改 RAII、MemMeta 槽位自动销毁`（**当前代码校准点**）；前置 `ea7d31c`（`docs:` 记录）/ `114c94d`（P0-14 注释编号对齐，实际修复 `eee3b01`）/ `c557d89`（P0-13）/ `eac4bb0`（T7）/ `29ab58b`（T6）。此后的 `docs:`、`config:` 提交不改变代码校准点 |
| 前几个提交 | `eac4bb0`（`pipeline:` T7 采集源解析 + P0-2）、`33d6690` / `fe48838`（`docs:` / `config:` 纯文档与注释）、`29ab58b`（`main:` T6 重写真实入口）、`3319441`（`docs:` T6 坐标回写）、`b461ece`（`pipeline:` T5 `NodeFactory` + `PipelineBuilder`）、`1402903`（`docs:` T5 坐标回写）、`0635ad1`（`docs:` T3 坐标回写）、`e0ce88f`（`docs:` 坐标回写）、`8d87731`（`config:` 新增 `NodeConfigManager`，T2）、`e9a620c`（`docs:` 坐标回写）、`87127e0`（`rtsp:` 修 P0-6/P0-7）、`e82e39e`（预处理框架 + `VpssNode`）、`d9f5ca2`（多链路设计 + `node.json` + `TESTLOG` 制度）、`7a2055b`（`docs:` 同步 fps）、`d158eca`（P0-3 修 `read_frame` 未归还）、`3ee3913`（报表加实测 fps）、`ccc305f`（`docs:`）、`9678e42`（耗时统计 + `PrintNode`）、`283bab5`（`config/push.json` 配置化），再往前 `fa082f7`、`af6b307`、`aa5846d`、`15e4438`、`e30ccb3`、`787edbd`（仓库共 25 个提交） |
| 远程 | `origin = git@github.com:DIronPacket/EdgeVisionService.git`（SSH，密钥 `~/.ssh/id_ed25519`） |
| 远程其他 ref | `refs/heads/main = b9c39ef`（旧代码，仍含已删除的 media 试验框架）、`refs/tags/base = 90b8c88` |
| 历史状态 | 本地与远程旧 `rk3588`（`776fb4f`）无共同祖先，已于 2026-11-05 用 `--force-with-lease` 覆盖。旧代码只能从 GitHub 提交页或 `base` tag 追溯，本地已无引用 |

> ⚠️ 因为主干历史已被重写，**任何"重新 clone 后合并"的思路都会失败**，见 §2.6。

## 1.3 环境与工具链

| 项 | 值 | 说明 |
| --- | --- | --- |
| 架构 / 核数 | `aarch64` / 8 核 | 本机就是 RK3588S 板端，非交叉编译 |
| CMake | 3.22.1 | 顶层要求 ≥ 3.16 |
| 编译器 | g++ 11.4.0（Ubuntu 22.04） | `CMAKE_CXX_COMPILER=/usr/bin/c++` |
| **C++ 标准** | **C++14**（`CMakeLists.txt:4`） | **禁止**使用 C++17+ 特性（`std::any`、`if constexpr`、结构化绑定等） |
| 库类型 | `BUILD_SHARED_MODULE=ON` → 自研模块编 `.so` | 产物统一输出到 `build/lib` 与 `build/bin` |
| 依赖位置 | `install/rk3588/{include,lib}` | FFmpeg 5.x、Rockchip MPP、ZLMediaKit(`libmk_api.so`)、RGA(`librga.so`) |
| 交叉工具链 | `cmake/rk3588.toolchain.cmake` | 路径写死 `/usr/aarch64-linux-gnu/bin/aarch64-linux-gnu-g++`，**本机不存在，当前不可用** |
| 被忽略目录 | `build/` `install/` `3rdparty/` `bin/` `lib/` `.vscode/` `*.zip` | 见 `.gitignore`；**这些目录里的文件不属于仓库内容** |

## 1.4 目录与模块地图（只列真实存在的东西）

```text
EdgeVisionService/
├── .clinerules/00-handoff.md   # ★ AI 助手自动加载入口（现状卡 + 跳读表 + 硬约束）
├── AGENTS.md                   # ★ 跨工具 agent 入口（均指向 docs/HANDOFF.md）
├── CMakeLists.txt              # 平台探测、库类型、全局输出目录、add_subdirectory(src)
├── cmake/                      # FindFFMPEG / FindMPP / FindZLMediaKit / FindRGA / rk3588.toolchain
├── config/                     # push.json：v0 单链路（推流宽高 / 编码 / 协议 / 端口 / print 间隔，T8 删除）；node.json：v1 多链路配置（4 路，T6 起由 testnode 直接消费，T7 起 `ViNode` 按 `vi.src` 前缀选采集实现）
├── scripts/tests/               # 验收脚本：run_t1_zlm_rtsp_key.sh（RTSP key / ref_cnt / 入参校验，18 项断言）· run_t2_node_config.sh + fixtures/（NodeConfigManager 解析/校验/v0 适配，53 项断言）
├── docs/                       # ARCHITECTURE · STATUS · ROADMAP · HANDOFF(本文) · DESIGN_MULTI_PIPELINE(多链路配置设计提案) · TESTLOG(测试与验证记录) · BUG_CASES(bug 原理剖析 + 面试要点存档)
├── install/rk3588/             # 第三方头文件与库（不入库）
├── 3rdparty/{rk3588,x86_64}/   # 第三方源码包（不入库）
├── build/                      # 构建产物（不入库，见 §1.5）
└── src/
    ├── CMakeLists.txt          # add_subdirectory: common · pipeline · platform · main
    ├── common/                 #                      → libcommon_api.so
    │   ├── frame_buffer.h      # 多平面 FrameBuffer（data[3]/stride[3]/fmt/size + owned_mem；~FrameBuffer() 调 Release 归还，拷贝已删）
    │   ├── json.hpp            # nlohmann/json 单头库
    │   ├── configmgr/          # 节点配置管理：node_config.h（数据模型 ImgSpec/NodeDesc/PipelineDesc/GlobalDesc…）· node_config_manager.{h,cpp}（v1 node.json 解析 + 校验 + ${global.*} 展开 + v0 push.json 适配）
    │   ├── profiler/           # 耗时统计：node_profiler.h（NodeProfiler）+ block_profiler.h（get_profiler / RAII 作用域）
    │   ├── preprocess/         # 预处理框架：image_preprocessor（门面：转码+叠加）· frame_converter（格式互转）· frame_drawer（画框/贴图/截图）· osd_renderer（字符叠加）· glyph_source + freetype_glyph_source（点阵 / FreeType 字形）
    │   └── MemManager/         # MemMeta.h · MemPool.{h,cpp} · MemManager.{h,cpp} · media_define.h
    ├── pipeline/               #                      → libpipeline_api.so
    │   ├── pipeline_node.{h,cpp}   # 节点基类：link_to / push_input_pipe / push_data / m_queue
    │   ├── node_factory.{h,cpp}    # T5：type → 节点对象（单例注册表，内置 vi/vpss/push/print）
    │   ├── pipeline_builder.{h,cpp}# T5：PipelineDesc → 已连线链路（拓扑校验 R1–R5 + 装配 + 批量启停）
    │   ├── moudle_vi/              # ViNode：采集源节点
    │   ├── moudle_push/            # PushNode：编码 + 推流节点
    │   ├── moudle_print/           # PrintNode：终点节点，打印各节点耗时与合计
    │   └── moudle_vpss/            # VpssNode：中间处理节点（转码+叠加，样板齐全，process_frame 待填）
    ├── platform/               #                      → platform_api（INTERFACE 聚合层）
    │   ├── include/            # capture / decoder / encoder / processor 四个抽象接口
    │   ├── capture/            # capture_factory + ffmpeg_source/ + v4l2_source/ + desktop_source/(空)
    │   ├── encoder/            # rk_mpp_encoder（RK 可用）+ ffmpeg_encoder（x86 未完成）
    │   ├── decoder/            # rk_mpp_decoder（草稿）+ ffmpeg_decoder（x86 未完成）
    │   └── rtsp/               # zlm_push_server：ZLMediaKit C API 封装
    └── main/
        ├── main.cpp            # 只打印一行，未装配流水线
        └── test/               # main_node.cpp（testnode = 真实入口，T6 起 builder 驱动 + SIGINT 优雅退出）· main_encode/main_pushStream（CMake 中已注释）· main_testpool（旧 API，未编译）
```

必须知道的四条：

1. **`moudle_vi` / `moudle_push` / `moudle_print` 是历史拼写错误**（正确为 `module`）。重命名会牵动 CMake 与 `#include`，属于独立任务，**不要顺手改**（`moudle_print` 是后加的，为与既有目录保持一致沿用了同样的拼写）。
2. `src/pipeline/moudle_vpss/`、`src/platform/capture/desktop_source/` 是**空目录**，git 不跟踪，重 clone 不存在。
3. `BUILD_RTSP_MODULE` 在 `src/platform/CMakeLists.txt:34` 被使用，但**源码里从未 `option()` 定义**（P1-2）。因此：
   - **干净配置（新 build 目录）不会编译 `rtsp`**，`zlm_push_server.cpp` 不参与构建（已实测：`/tmp` 下全新配置的目标只有 `common_api`/`capture`/`encoder`/`decoder`/`pipeline_api`/`EdgeVisionService`/`testnode`）；
   - **但现有 `build/CMakeCache.txt` 里恰好缓存了 `BUILD_RTSP_MODULE:BOOL=ON`**，所以在 `build/` 下 `cmake --build` 会编出 `librtsp.so`。
   → 判断"rtsp 到底编没编"要看配置来源，别被两种现象搞混；修 P1-2 时应把它改成正式的 `option(BUILD_RTSP_MODULE "..." ON)`。
4. `pipeline_api` **T5 起 `PUBLIC` 链 `common_api` + `platform_api`**（`PipelineBuilder` 要读 `NodeConfigManager`，同时拿到 `configmgr` 头路径）。2026-11-08 之前它只链 `platform_api`，`common_api` 靠可执行文件层补链（`testnode` 里手写）——那段隐式依赖已随 T5 消除，但**其它新增 target 仍应显式链 `common_api`**，别指望传递依赖。

## 1.5 构建产物现状（最容易踩坑的地方）

`build/` 目录存在且**在部分情况下可直接复用**，但里面混着陈旧产物：

| 事实 | 说明 |
| --- | --- |
| `build/CMakeCache.txt` 的 `CMAKE_HOME_DIRECTORY` | `/mnt/nvme/workspace/EdgeVisionService`，**与本机一致，缓存仍有效**（不必盲目 `rm -rf build`） |
| 产物架构 | `build/bin/*`、`build/lib/*` 都是 ARM aarch64 ELF，本机可直接运行 |
| ✅ 真实可跑 | `./build/run.sh EdgeVisionService` → 输出 `EdgeVisionService start`（已实测） |
| ✅ 构建已实测 | `cmake -S . -B build && cmake --build build -j8` 全量通过（2026-11-05，含 `rtsp` 目标，原因见 §1.4 第 3 条） |
| ✅ 真实主链路 | `./build/run.sh testnode`（来自 `src/main/test/main_node.cpp`） |
| ⚠️ 陈旧产物 | `build/lib/libmoudleRtsp.a`、`libmoudleRtsp.so`（10 月产物，**对应源码已删除**，勿再引用） |
| ⚠️ 陈旧可执行 | `build/bin/test_decoder`、`test_encoder`、`test_pushStream`、`testpool`（对应源码在 `test/CMakeLists.txt` 中已注释或与当前 API 不符） |
| ❗ `build/run.sh` 未入库 | 属于 `build/` 被忽略的内容；**删除 `build/` 后需重新创建**，否则 §2.2 的运行验证方式失效 |

`build/run.sh` 做的事（重建时照此写）：

```bash
#!/bin/bash
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); cd "${SCRIPT_DIR}"
export LD_LIBRARY_PATH="${SCRIPT_DIR}/lib:${SCRIPT_DIR}/../install/rk3588/lib:$LD_LIBRARY_PATH"
exec "${SCRIPT_DIR}/bin/$@"
```

## 1.6 运行时主链路（代码级）

```text
入口（T6，`main/test/main_node.cpp`，加一路只改配置）：
  NodeConfigManager::load_with_fallback(argv[1])   // 优先 node.json，没有则 v0 push.json
        → MemManager::MemInit()                     // 进程级资源（池块数待 T10a 配置化）
        → PipelineBuilder::build_all(sets)          // 工厂 + 拓扑校验 + 连线，单链失败只告警
        → signal(SIGINT/SIGTERM) → start_all(sets)  // 逆序：终点先起
        → while (g_run) sleep 200ms                 // 取代旧 while(1)
        → stop_all(sets)（正序：源头先停）→ sets.clear() → FreeMem() → return 0
        │
        ▼  每条链路内部（node.json 的一条 pipelines[i]）
MemManager::getInstance().MemInit(50)        // 预建 50 块 1920x1080 NV12，每块再挂一个 NodeProfiler
        │
        ▼
ViNode::create(0, "vi")   → start()          // 独立线程 run()：按通道取 CaptureInterface，30fps 节流
   采集：FfmpegCapture / V4l2Capture → 写 FrameBuffer(NV12, stride 128 对齐) → MemMeta::set_ptr
   耗时：profiler->begin("vi0") / end("vi0") 包住 read_frame（不含末尾的节流 sleep）
        │
        ▼  push_input_pipe(block)  → 下游 m_queue.pushFrame(block)（深度 20，满则丢最老帧）
PushNode::create(0, "rtsp") → start()        // 独立线程 run()：MppEncoder，codec/fps/码率来自 config/push.json
        │  编码回调 EncodedPacket → zlm_rtsp_input_encodeData() → ZLMediaKit RTSP Server
        │  耗时：profiler->begin("rtsp0") / end("rtsp0") 包住 encode
        ▼
PrintNode::create(0, "print") → start()      // 终点：按 print.interval 打印各节点耗时与合计（默认每 30 帧）
        │
        ▼  m_queue.release(block) 归还池子（MemBlock::reset() 顺带清空 profiler，下一帧从零开始）
```

节点连接关系存在 `PipelineNode::next_nodes_` / `parent_nodes_`（**公有裸指针 `std::list`**），`link_to()` 双向登记。`PipelineNode` 继承 `std::enable_shared_from_this`，但链路本身用裸指针。

**已知与上图不一致的地方**（照 STATUS.md 执行修复，不要"顺手美化"）：

- `main.cpp` 根本没装配流水线，真实入口在 `main/test/main_node.cpp`（**T6 起由 `PipelineBuilder` 驱动**：配置 → 进程资源 → 装配 → `start_all`/`stop_all` → `FreeMem`；旧的 `while(1)` 与永不执行的 `stop()` 已删除，退出码 0 实测见 TESTLOG §5.9）。
- `ViNode` 写死 `CAPTURE_V4L2`，x86 下工厂返回 `nullptr` 后解引用（P0-2）。
- `PushNode` 编码器是**值成员** `MppEncoder enc`（不是接口指针），`EncoderInterface` 抽象未真正生效。
- 队列丢帧、丢帧计数、背压都没有对外开放。
- 配置只覆盖「采集输出分辨率/帧率 + 编码 codec/fps/码率 + RTSP 端口/app/stream」；`gop` 字段已解析但 `EncoderInterface` 没暴露，**暂不生效**；`rtmp`/裸 RTP 协议**后端未实现**，写进配置会明确报错并跳过推流。

## 1.7 内存池与节点 API 速查（改代码前先看这段）

命名空间 `pool`（`src/common/MemManager/MemPool.h`）：

| 类 | 关键接口 | 要点 |
| --- | --- | --- |
| `MemPool`（单例） | `getInstance()` · `addBuffer(MemMeta*)` · `acquire(name)` · `release(block,name)` · `pushBlock(block,name)` · `registerQueue(queue,name)` · `delGetBlock()` | 全局 `free_list_` 单锁 `free_mtx_`；**`Size()` 读 size 未加锁（P1-9）** |
| `MemQueue` | `InitQueue(name, size=20, type)` · `getBlock(timeout=100)` · `pushFrame(block)` · `release(block)` · `release(block,name)` · `Size()` | `QueueType::OWNER`（源头取空块）/ `USER`（下游收块）；满则 drop-oldest |
| `MemBlock` | `data()`→`MemMeta*` · `userCnt()` · `setOwner/addUser/subUser` · `reset()` | 引用计数是"名字列表"实现，**多下游扇出时不可靠（P0-5）**；`reset()` 同时清空块上的 `NodeProfiler` |
| `MemMeta` | `set_ptr<T>(name,ptr)` · `data<T>(name)` · `has(name)` · `clear()` | 内部 `unordered_map<string, MemSlotBase*>` + `MemSlot<T>`：槽位析构会调 `freeObj()` → `slot_free(t)` → `delete t`（`FrameBuffer*` 也走主模板，`~FrameBuffer()` 自己 `Release()`，**已无 `slot_free<FrameBuffer*>` 特化**）；有析构、删了拷贝，`set_ptr` 覆盖同名先释放旧槽位（同一指针重复登记则保留在用的对象），`data<T>` 用 `dynamic_cast` 校验类型（P0-11 / P1-10 已于 2026-11-10 修，TESTLOG §5.14）。**放新类型的规矩**：`new` 出来的裸指针直接挂（主模板 `delete` 就够，析构照常跑）；首选让类型自己写析构；分配器不匹配 / 不便加析构的（`malloc`/`new[]`/C 句柄）在该类型旁特化 `slot_free<T*>`；栈对象和 `BindMemory` 视图**不要入槽位**（槽位即所有权，`clear()` 会无条件 `delete`）；同一裸指针只能挂**一个**槽位名（`clear()` 不查重，挂两处即双删） |
| `MemManager`（单例） | `MemInit(size=50)` · `FreeMem()` · `addImgConfig(ImgConfig)` | 建块时挂 `FrameBuffer` + `profile::NodeProfiler`（slot 名 `"profiler"`）；`FreeMem()` 统一调 `MemMeta::clear()` 把所有槽位（含节点自建的 `vpss_out`）连对象一起还回去，再删 `MemMeta` / `MemBlock` —— 不再按 `imgMap` 点名（P0-9 + P0-11 已修，TESTLOG §5.13 / §5.14） |
| `FrameBuffer` | `Allocate(w,h,fmt,align)` · `BindMemory(base,w,h,fmt,align)` · `Release()` · 字段 `data[3]/owned_mem/native/stride[3]/fmt/width/height/capacity/data_size/timestamp_ms` | 两种来源：`Allocate()` 自己 `new[]` 一块（基址记 `owned_mem`）、`BindMemory()` 只绑外部内存（`owned_mem == nullptr`）。**析构调 `Release()`（RAII）**：`delete frame` 即归还，栈上视图是 no-op；**拷贝/赋值 `= delete`**（拷一份就是两份额度 → 双释放）。**同一份 `FrameBuffer` 挂在块上、被所有链路共享，所以这些字段是「块级状态」**：任何写入方（采集后端等）在使用前必须按自己的输出分辨率 `BindMemory` 一次，否则会沿用别的链路留下的 `stride/宽高/UV 偏移`（P0-14，TESTLOG §5.12）。P0-10 只剩 `capacity` 语义（恒为 `w*h*3`）未改 |

节点工厂 / 装配（T5，`src/pipeline/`）：

- `NodeFactory::getInstance()`：`register_type(type, creator)` · `create(type)` · `has_type(type)` · `types()` · `last_error()`。内置 `vi`/`vpss`/`push`/`print` 在 `node_factory.cpp::RegisterBuiltins()` 里集中注册（**不要改成各模块静态自注册**：`pipeline_api` 是动态库，未引用的 TU 会掉符号）；`create()` 失败返回 `nullptr` 并写 `last_error()`，**不抛异常**。
- `PipelineBuilder(const config::NodeConfigManager&)`：`build_one(desc, set)` · `build_all(sets)` · `start_all(sets)` · `stop_all(sets)`；静态 `check_order(desc, err)` 可单独校验拓扑（R1 至少 2 节点 · R2 首节点必须是 `vi` · R3 末节点必须是 `push`/`print` · R4 `vi` 只能在首位 · R5 `print` 只能在末位），错误串带字段路径 `pipelines[i].nodes[j].…`。
- 装配顺序固定：`factory.create(type)` → `node->create(channel, name)` → `set_data(配置原文)` → `link_to(下一个)`；**`start()`/`stop()` 只经 builder 调用**（`start_all` 倒序、`stop_all` 正序），不要自己逐个 `start()` 打乱启停顺序。`build_all` 是**单链失败隔离**（N3）：失败链跳过并记 `errors()`，其余链继续。
- 入口侧（T6，`src/main/test/main_node.cpp`）的固定序列：`load_with_fallback` → `MemInit` → `build_all` → `signal` → `start_all` → `while (g_run) 200ms` → `stop_all` → `sets.clear()` → `FreeMem` → `return 0`。**`sets.clear()` 必须在 `stop_all` 之后、`FreeMem` 之前**（先释放节点再放池）；`PipelineSet` 持 `shared_ptr`，析构里的 `stop()` 是空操作，所以生命周期只由 builder 管。信号处理函数只写 `volatile sig_atomic_t g_run`（**不打印、不加锁、不分配**）。

编码器 / 推流的常用入口：

- `MppEncoder`：`open(w,h,CodingType,fps,bitrate)` · `encode(FrameBuffer*)` · `set_encode_callback(EncodeCallback)`；NV12、stride 128 对齐、QP 33、GOP 150。
- `CaptureInterface`：`open(uri)` · `read_frame(FrameBuffer*)` · `set_capture_callback` · `close()`；工厂 `CreateCapture(CaptureType)`（`CAPTURE_FFMPEG` / `CAPTURE_V4L2`）。
- ZLM 封装（`zlm_push_server.h`）：`zlm_rtsp_onekey_start(...)` · `zlm_rtsp_input_encodeData(...)` · `zlm_rtsp_stop(...)`；**`stop` 的流 key 与注册时不一致（P0-6）、入参校验顺序错（P0-7）**。

节点耗时统计（`src/common/profiler/`，两个 header-only 头文件，不产生 `.so` 符号）：

- `profile::get_profiler(block)`（`block_profiler.h`）：从 block 的 `MemMeta` 取 `NodeProfiler*`；块/槽位缺失返回 `nullptr`，**调用方必须判空**。
- `NodeProfiler::begin(节点名)` / `end(节点名)`（`node_profiler.h`）：节点拿到块后写开始时间戳、处理完传下游前写结束时间戳，内部用 `steady_clock` 微秒。
- `report(frame_no, fps = -1)`：单行报表「各节点耗时 + 节点合计 + 首尾跨度 [+ 实测帧率]」；`fps < 0`（默认）时不输出帧率段，`PrintNode` 传入自己算的窗口帧率。`NodeCostScope` 是 RAII 版，**代码块里含等待下一帧的 sleep 时不要用**（会把空闲时间算进去）。
- 挂在块上的 slot 名是 `profile::kProfilerSlot`（`"profiler"`），由 `MemManager::MemInit()` 注册、`MemBlock::reset()` 清空。
- 读表口径：**首尾跨度 ≈ 帧节拍 `1/fps` + 末端节点耗时**，与「节点合计」的差值主要是 `ViNode` 的帧率节流 sleep（`end("vi0")` 写在 `sleep_for()` 之前），队列排队实测 ~0.1ms。实测 30fps → 合计 14.9/跨度 40.0，15fps → 13.1/71.7，10fps → 39.1/106.5（推导见 ARCHITECTURE §2.2）。
- **实测 fps 不等于 `1000/跨度`**（跨度是重叠窗口，取倒数会偏小）：它是「上一行报表到本行报表的帧数 / 墙钟时长」。`fps` 配得比采集端高时（如 60 配 V4L2 30fps），实测会稳定在采集端帧率、跨度反而小于帧节拍。

## 1.8 当前能力与缺口（摘要，明细看 STATUS.md）

| 能力 | 现状 |
| --- | --- |
| 采集 | FFmpeg（本地文件 / 摄像头 / RTSP / RTMP / HTTP 输入）可用；V4L2 固定 YUYV→NV12、无 `poll`；**T7 起采集源由 `vi.src` 前缀选择**（`dev:`→V4L2，`file:` / 网络流→FFmpeg，无前缀按老规则兜底），源分辨率仍由 `dev:` 的 5 段写法或文件本身决定，输出分辨率可配（TESTLOG §5.10） |
| 内存池 | `MemManager + MemPool + MemBlock + MemQueue` 单消费者链路可跑通；引用计数链路完整（`acquire/push` → `addUser`、`release` → `subUser`，P0-5 已核实为假阳性），但 **fan-out 拓扑不可达**（builder 只连 `i→i+1`，STATUS §4.2 第 24 条） |
| 流水线 | `ViNode → PushNode → PrintNode` 单链路可运行；分辨率/帧率/codec/码率/端口已由配置驱动，错误路径丢块仍未全修。**T5 起装配不再手写**：`NodeFactory`（type→节点）+ `PipelineBuilder`（拓扑校验 + 装配 + 连线 + `start_all`/`stop_all`），`config/node.json` 能直接建成并启动 4 条链路（TESTLOG §5.8）。**T6 起真实入口已接入 builder**：`main_node.cpp` 只做配置 → 进程资源 → 装配 → 启停 → `FreeMem`，SIGINT 后正序 `stop_all` + `FreeMem` + 退出码 0 已实测（TESTLOG §5.9）；**T7 起 `ViNode` 按 `vi.src` 前缀选采集实现，4 路（1 摄像头 + 3 mp4）同时出帧已实测**（TESTLOG §5.10） |
| 耗时统计 | 每块挂 `NodeProfiler`；Vi/Push 各自记录本节点耗时，终点 `PrintNode` 按 `print.interval` 打印（默认每 30 帧），报表带实测 fps |
| 编码 | MPP H.264/H.265 可用（每帧 `memcpy`，无 EOS flush）；FFmpeg 软编/软解未完成 |
| 推流 | 单路 RTSP 可演示，参数来自 `config/push.json`；stop/空指针问题待修；`rtmp`/裸 RTP 未实现 |
| 配置解析 | `common/configmgr`（M1/T2）：能读 v1 `node.json`（多链路 + 字段级校验 + `${global.*}` 展开 + 默认值）与 v0 `push.json`（合成单链路），见 TESTLOG §5.6；`PipelineBuilder`（T5）与真实入口 `main_node.cpp`（T6）都已消费它，**入口不再手写装配**（TESTLOG §5.8 / §5.9），`global.pool.*` 仍无人读取（T10a）。`config/node.json` 已按 4 路定稿（ch0 = `dev:/dev/video0` 1080p@30，ch1~ch3 = 3 个 mp4，实测 **1024×576@24**），T3 验收 104 项断言见 TESTLOG §5.7；**4 路同时出帧已实测**（T7：`ViNode` 按 `vi.src` 选后端，TESTLOG §5.10），mp4 loop 待 T7b |
| 未落地 | RGA 预处理、OSD、录像、多通道、配置热加载、日志/指标/健康检查、单测、CI、Docker、OTA |

**P0 共 14 项**（构建失败 / 必现崩溃 / 资源泄漏；P0-2、P0-3、P0-4、P0-6、P0-7、P0-12、P0-13、P0-14 已修复，P0-5 已核实为**假阳性**（引用计数链路完整），P0-9 于 2026-11-10 修复（TESTLOG §5.13 / BUG_CASES §2.10），P0-11 于 2026-11-10 修复（TESTLOG §5.14 / BUG_CASES §2.11），**剩 3 项**：P0-1、P0-8、P0-10（部分修复：`owned_mem`/`Release()` 已加，仍可拷贝无析构）；编号保留以维持引用稳定），清单见 [STATUS.md §4.1](STATUS.md)。接手后若不知从哪下手：**先修 P0-1（x86 构建），改动小、收益大。**

## 1.9 任务 → 文件 索引表

| 我要做的事 | 主文件 | 关联 |
| --- | --- | --- |
| 改推流分辨率/编码(h264,h265)/协议/端口 | `config/push.json`（改完重跑 `testnode` 即生效，无需重编） | `moudle_push.cpp::set_data`、`moudle_vi.cpp::set_data` |
| 改配置格式 / 调配置校验规则 / 加配置字段 | `src/common/configmgr/node_config_manager.cpp`（`ParseV1` / `ParseV0` / `ExpandRefs` / 数值守护） | `node_config.h`（数据模型 `GlobalDesc`/`PipelineDesc`/`NodeDesc`）、`config/node.json`、DESIGN §5、验收脚本 `scripts/tests/run_t2_node_config.sh`（53 项断言，改校验必跑） |
| 改多链路配置（4 路：摄像头 + mp4） | `config/node.json`（ch0 = `dev:/dev/video0`；ch1~ch3 = `file:...mp4` + `loop`；**vi 与 push 的宽高/fps 必须等于素材实测值**，换素材先 `ffprobe`） | 验收脚本 `scripts/tests/run_t3_node_json.sh`（104 项断言，改配置必跑；含 ffprobe 交叉校验与 v0 等价断言）、`config/push.json`（v0 对照）、TESTLOG §5.7 |
| 改采集行为（含「哪种源用哪个后端」） | `platform/capture/{ffmpeg_source,v4l2_source}/*.cpp`；`vi.src` 前缀 → 后端的映射在 `pipeline/moudle_vi/moudle_vi.cpp` 的 `ResolveSrc()`（T7） | `capture_interface.h`、`capture_factory.cpp`、TESTLOG §5.10 |
| 改编码参数（QP/GOP/码率） | `platform/encoder/rk_mpp_encoder.cpp`、`pipeline/moudle_push/moudle_push.cpp` | `config/push.json`、`moudle_push.h` 里的默认成员 |
| 新增 rtmp / 裸 RTP 推流后端 | `platform/rtsp/zlm_push_server.cpp`、`moudle_push.cpp` | `platform/CMakeLists.txt`（`BUILD_RTSP_MODULE`） |
| 改内存池/引用计数 | `common/MemManager/MemPool.{h,cpp}`、`MemMeta.h`、`MemManager.cpp` | `common/frame_buffer.h` |
| 加/改流水线节点 | `pipeline/pipeline_node.h`、`pipeline/moudle_*/` | `pipeline/CMakeLists.txt`、`pipeline/node_factory.cpp`（在 `RegisterBuiltins()` 里加一行 `type → creator`，否则 `create()` 认不出新 type） |
| 改装配逻辑 / 拓扑规则（谁能接谁、第几个） | `pipeline/pipeline_builder.cpp`（`check_order` 的 R1–R5 规则、`build_one` 的 `create → node->create → set_data → link_to` 顺序） | `pipeline/pipeline_builder.h`（`PipelineSet` / `describe()` / 计数与 `errors()`）、验收脚本 `scripts/tests/run_t5_builder.sh`（64 项断言，改规则必跑）、TESTLOG §5.8 |
| 接入 / 修改预处理节点（转码+叠加，VPSS 位置） | `pipeline/moudle_vpss/moudle_vpss.{h,cpp}`（`VpssNode::process_frame` 是唯一待填函数） | `config/push.json` 的 `vpss` 段、`common/preprocess/image_preprocessor.{h,cpp}`、`main/test/main_node.cpp`（装配位置） |
| 做多链路 / 改配置格式 / 上提进程级资源 | `docs/DESIGN_MULTI_PIPELINE.md`（方案与分期；**已确认 D1–D8：4 路 = 1 摄像头 + 3 路 mp4、共用端口、不做热加载/鉴权/子码流、迁移后删 `push.json`**；**已落地 T2 配置 / T3 定稿 / T5 builder / T6 入口 / T7 采集源**，剩 T7b（loop + 缩放）、T8（删 `push.json`）、T10a（池参数接线）） | 现状事实见本文 §1.4/§1.7、`config/push.json`、`main/test/main_node.cpp`、`platform/rtsp/zlm_push_server.cpp`、`pipeline/moudle_vi/moudle_vi.cpp`（T7 的 `ResolveSrc`）、`platform/capture/ffmpeg_source/ffmpeg_capture.cpp`（文件源不循环、不缩放）、STATUS §4.1 的 P0-5 |
| 转码 / 画框 / 贴图 / OSD 字符叠加（预处理） | `common/preprocess/image_preprocessor.{h,cpp}`（门面） | `frame_converter`（格式互转）、`frame_drawer`（画框/贴图/截图）、`osd_renderer` + `glyph_source`（字形）、`common/preprocess/README.md`、开关 `BUILD_FREETYPE_OSD` |
| 看每帧各节点耗时 / 调打印频率 | `config/push.json` 的 `print` 段（改完重跑即生效） | `common/profiler/*.h`、`pipeline/moudle_print/*`、`moudle_vi.cpp::run`、`moudle_push.cpp::run` |
| 装配服务入口 | `main/test/main_node.cpp`（**T6 起是唯一真实入口**：config → `MemInit` → `build_all` → 信号 → `start_all` → `stop_all` → `FreeMem`）、`main/main.cpp`（仍是空壳） | `pipeline/pipeline_builder.cpp`（`build_all` + `start_all`/`stop_all`）、`common/configmgr/node_config_manager.cpp`（`load_with_fallback`）、`main/CMakeLists.txt`、TESTLOG §5.9 |
| 改构建/依赖查找 | 顶层 `CMakeLists.txt`、`cmake/Find*.cmake` | `src/*/CMakeLists.txt` |
| 查"为什么跑不起来" | [STATUS.md §4](STATUS.md) | 本文 §3 排障规程 |
| 记测试 / 查回归结果 / 证明"已修复" | [TESTLOG.md](TESTLOG.md) | §4 是 L1 冒烟汇总表，§5 是 L2/L3 明细；写完仍按本文 §2.2 顺序验证 |


---

# 第二部分　接手本项目必须遵守的规定

> 这一部分是**红线**。违反其中任何一条，改动即使在技术上"更好"也算错误交付。

## 2.1 绝对禁止（红线）

| # | 禁止事项 | 原因 |
| --- | --- | --- |
| 1 | 未经任务明确要求**重命名目录 / 重构结构**（尤其 `moudle_*` → `module`） | 会牵动 CMake 与全部 `#include`，必须作为独立任务做 |
| 2 | 使用 **C++17 及以上**特性（`std::any`、`if constexpr`、结构化绑定、`std::string_view`） | 工程标准是 C++14（`CMakeLists.txt:4`） |
| 3 | 引入 `install/` 中**不存在**的新第三方库（OpenCV、gtest、spdlog、fmt…） | 板端依赖固定，加库等于加不可控风险；需要时先提方案 |
| 4 | 修改 `install/`、`3rdparty/`、`build/` 里的第三方文件或旧产物 | 非仓库内容，改了下游无人知晓 |
| 5 | `git push --force` 到 `rk3588` / `main`，或 `git reset --hard`、`git clean -xfd` | 主干历史已重写过一次，二次改写代价极高 |
| 6 | 提交 `build/`、`install/`、`3rdparty/`、`.vscode/`（包括 `git add -f`） | 已在 `.gitignore`，属于本机资产 |
| 7 | 重新引入 `src/common/media/` 一类**第二套内存池/框架** | 该试验框架已于 `e30ccb3` 彻底移除，不要再建"新框架" |
| 8 | 做任务范围之外的"顺手修"（批量格式化、调整无关代码风格、改注释排版） | 污染 diff，让 review 与回滚失效 |
| 9 | 在**没有实际执行构建/运行**的情况下声称"已修复/已通过" | 必须给出真实命令与输出 |
| 10 | 擅自改动 `main` 分支或推送其他远程分支 | 只操作 `rk3588`，其他分支需用户明确指定 |

## 2.2 构建与验证规定

1. **默认原生构建**（本机是 aarch64 板端，不需要交叉编译）：
   ```bash
   cmake -S . -B build && cmake --build build -j"$(nproc)"
   ```
2. **不要盲目 `rm -rf build`**：`CMakeCache` 的 `CMAKE_HOME_DIRECTORY` 与本机一致，缓存有效。只有缓存报路径错误或 CMakeLists 结构大改时才清。
3. 改了 `CMakeLists.txt` / `cmake/Find*.cmake` 后，必须**重新执行一次 `cmake -S . -B build`**，不能只 `cmake --build`。
4. **新建 build 目录时 `rtsp` 不会被编译**（源码没有 `option(BUILD_RTSP_MODULE)`，见 §1.4 第 3 条）。若需要编 RTSP 模块，显式传 `-DBUILD_RTSP_MODULE=ON`；修 P1-2 时要把它补成正式 option。
5. 验证顺序固定为：**编译通过 → `./build/run.sh EdgeVisionService` → 相关 `testnode`**。
6. `testnode` 需要真实 `/dev/video0` 与 ZLMediaKit 端口；**无设备时崩溃属于已知 P0-2，不算你改坏**，但必须说明"未做运行验证及原因"。
7. x86 构建当前**必然失败**（P0-1）。若任务涉及 x86，必须先补 `install/x86_64` 或给 `FindFFMPEG.cmake` 加 `pkg-config` fallback，而不是靠改代码绕过。
8. 长跑类验证（`testnode` 现在会一直跑到收信号）一律用 `timeout -s INT 20 ./build/run.sh testnode`，既避免挂死终端，又顺带验证优雅退出（`EXIT=0` + 无残留进程）。
9. **每次验证都要留痕**：跑完按 [TESTLOG.md](TESTLOG.md) 分级记录（L1 一行 / L2 任务小节 / L3 里程碑），必须含命令、结果摘要、结论、未做项；提交代码时同一 commit 带上对应记录。没有记录的"已通过"按 §2.1 第 9 条处理。
10. **修 bug 必须写档案**（用户 2026-11-09 明确要求）：凡是"有原理含量"的 bug —— ① 现象与根因不在同一层 ② 只在特定并发/时序/内存残留下复现 ③ 涉及资源所有权、生命周期、跨组件共享状态 ④ 曾被误判过方向 —— 修完必须在 **[BUG_CASES.md](BUG_CASES.md)** 追加案例（固定顺序：现象 → 位置到函数级 → 根因原理 → 修复 → 为什么难发现 → 面试怎么讲），并更新其 §1 索引表；与代码同一批提交。纯笔误 / 单行空指针且无连带语义的，只在 [STATUS.md](STATUS.md) 标注。


## 2.3 C++ 代码规定

| 项 | 规定 |
| --- | --- |
| 标准 | C++14；不引入 17+ 特性 |
| 缩进 / 风格 | 2 空格缩进，K&R 大括号，与现有文件保持一致 |
| 头文件 guard | **新增文件**用 `EVS_<MODULE>_<FILE>_H`；老文件（`MEM_POOL_H` 等）不批量改，随任务落地 |
| 命名空间 | 内存池相关放 `pool`；新增公共组件放对应模块命名空间，**不要**新建 `evs::core` 之类大改造 |
| 内存管理 | 现状是裸指针 + 手工 new/delete；任务内可局部改善，**不要**把整条链路改成智能指针 |
| 日志 | 现状 `printf`/`std::cout`/`std::cerr` 混用；**新代码不要新增逐帧 `printf` + `fflush`**（P1-14） |
| 注释 | 中文，说明"为什么"；不写无信息量的注释 |
| 文件布局 | 一个 `.cpp` 配同名 `.h`；平台相关实现放 `platform/<backend>/`；对外接口不暴露 FFmpeg/MPP 类型，统一转 `FrameBuffer` |
| 错误处理 | 不使用异常跨模块传播；返回码/空指针约定与现有代码一致 |

## 2.4 流水线与内存池规定

1. **取块必有归还**：`acquire()` / `getBlock()` 之后的所有失败分支（读帧失败、编码打开失败、队列满、异常返回）都必须 `release()`。P0-3、P0-4 都是漏了这一步（均已修复）；`push_input_pipe()` 队列满丢块那条路径仍未处理（P0-5 一起修）。
2. `push_input_pipe()` **只入队**，不做耗时处理；耗时逻辑放节点自己的 `run()` 线程。
3. 链路的**终点节点负责 `m_queue.release(block)`**；中间节点处理后继续 `push_input_pipe()`。
4. **接多下游之前，先把块级引用计数补对**（P0-5）。当前一源多下游会读到被复用的脏块——不要"先接上再说"。
5. `FrameBuffer` 有两个身份：`Allocate()` 出来的**拥有内存**（`owned_mem`，析构 `Release()` 归还），`BindMemory()` 出来的**纯视图**（`owned_mem == nullptr`，析构 no-op）。业务代码**不得**绕过这两个接口自己 `new/delete` `data[]`；也**不得**按值拷贝 `FrameBuffer`（拷贝已删，要共享就传指针）。
6. 不要绕过 `MemPool` 单例自行管理 `free_list_`；不要新增第二个全局池。
7. **写入块之前必须重绑视图**：`FrameBuffer` 的 `width/height/stride/data[1]` 挂在块上、被所有链路共享，别的链路用完会把视图改成自己的分辨率。任何采集 / 生成帧的后端在写块前都要 `BindMemory(frame->data[0], 本链路输出宽, 高, 格式[, 对齐])`（`data[0]` 恒为块首地址，可安全复用；格式按**实际写入的字节布局**取，不按配置期望）。违反即 P0-14 那种「多路并发时某一路下半屏绿」（TESTLOG §5.12）。

## 2.5 CMake 规定

1. 新增源文件必须显式加入对应 `target_sources` / `add_library`。
2. 新模块使用 `MODULE_LIB_TYPE` 变量，跟随顶层 `BUILD_SHARED_MODULE`（`common_api` 写死 `SHARED` 属于历史例外，不要扩散）。
3. 输出目录统一走顶层全局设置（`build/lib`、`build/bin`），不要自定 `LIBRARY_OUTPUT_DIRECTORY`。
4. 新开关必须 `option(... "描述" ON/OFF)` 给默认值并写注释；**改开关要同步 README §3 与 ARCHITECTURE §3**。
5. 依赖查找只放在 `cmake/Find*.cmake`，不在 `CMakeLists.txt` 里硬编码绝对路径。
6. 链接关系尽量用 `PUBLIC`/`PRIVATE` 表达真实依赖，避免依赖可执行文件层"补链"（P1-1 就是这类问题，修复时一次改到位）。

## 2.6 Git 规定

1. **主干只有 `rk3588`**（远程同名，已建立 upstream）。实验性改动先在本地建 `feature/<主题>` 分支，不要直接在主干堆 WIP 提交。
2. **提交信息格式**：`<模块>: <动词> <对象>`，模块取 `build|capture|encoder|decoder|rtsp|mem|pipeline|docs|test`，例：
   - `pipeline: 修正 read_frame 失败未归还 MemBlock`
   - `docs: 补充 HANDOFF 接手流程`
3. **提交前必查** `git status --short`：不得出现 `build/`、`install/`、`3rdparty/`、`.vscode/` 下的文件。
4. **一次提交只做一件事**：代码与文档分开提交（现有两次提交就是这个粒度）。
5. **推送前先 `git fetch origin`** 确认远程未被他人改动；本仓库 `origin` 是 SSH（`git@github.com:DIronPacket/EdgeVisionService.git`），已配 `~/.ssh/id_ed25519`。
6. **禁止 force 推送**。历史已重写过一次；若确实必须重写，前置条件：用户明确同意 + 在本地先打备份 tag。
7. **禁止在有未提交改动时**执行 `git reset --hard`、`git clean -xfd`、`git checkout -- .`。
8. 提交后如需同步，用普通 `git push origin rk3588`；不要推 `main`。

## 2.7 命名与目录规定

- 新目录小写；**新模块用 `module`，不要复制 `moudle` 拼写错误**。
- 公共类型放 `common/`；平台后端放 `platform/<backend>/`；业务节点放 `pipeline/`（未来为 `pipeline/nodes/<name>/`）。
- 配置字段与 schema 以 [ARCHITECTURE.md §4.2](ARCHITECTURE.md) 草案为准，落地时同步该节。
- 对外头文件不暴露 FFmpeg / MPP / ZLM 类型，统一收敛到 `FrameBuffer`、`CodingType` 等内部类型。

## 2.8 环境与资产规定

| 项 | 规定 |
| --- | --- |
| `install/rk3588/lib` | 交付依赖，**不得删除或替换**（尤其 `libmk_api.so`、`librockchip_mpp.so*`、`libavcodec.so.59`） |
| `3rdparty/` | 只放第三方源码包，不要在里面对源码打补丁 |
| `/dev/video0` | 板端独占设备，运行前确认没有其他进程占用；用完释放 |
| 进程管理 | 不 kill 他人进程；自己启动的测试程序用 `timeout` 包裹 |
| 系统层 | **不升级 glibc / 内核 / 驱动**：`libavcodec.so` 最高要求 `GLIBC_2.35`，`libmk_api.so` 依赖 OpenSSL 3，升级即破坏 ABI |
| 端口 | ZLMediaKit 默认 8550（代码里还有 8554 的默认成员，见 P1-11），起服务前确认端口空闲 |

## 2.9 交付前自检清单（每次都要过一遍）

- [ ] 只修改了任务范围内的文件（`git status` / `git diff --stat` 复核）
- [ ] **实际执行**了构建，并保留命令与输出
- [ ] 能运行就运行验证；不能运行则明确说明原因（缺设备 / 缺依赖 / 已知 P0）
- [ ] 无新增编译告警
- [ ] 没有引入 C++17+ 特性、没有引入新第三方库
- [ ] 所有取块失败分支都有 `release`
- [ ] 新文件已加入 CMake、头文件 guard 符合规范
- [ ] 行为/开关变化已同步 README、ARCHITECTURE、STATUS 中受影响的部分
- [ ] 提交信息符合 `<模块>: <动词> <对象>`
- [ ] 若修的是"有原理含量"的 bug：已在 **`docs/BUG_CASES.md`** 追加案例（含根因原理与函数级位置），并更新其 §1 索引表
- [ ] 没有提交被忽略目录的内容

---

# 第三部分　常见任务操作规程

## R1 修一个 P0 / P1 问题（最常做）

1. 在 [STATUS.md §4](STATUS.md) 找到条目，确认**文件:行号**与影响面。
2. 打开对应文件，先读上下文（不要只看那一行），确认现有错误处理约定。
3. **最小改动**：只修这一条，不顺手重构。
4. 构建：`cmake --build build -j8`；如改动涉及 CMake，先 `cmake -S . -B build`。
5. 能验证就验证（板端设备/端口允许时）；不能验证就写清"未验证原因"。
6. 若问题已修复，**同步更新 STATUS.md 对应表格**（删行或改状态），并把行号变化一并核对。
7. 提交：`<模块>: <动词> <对象>`。

## R2 新增一个处理节点（算法 / OSD / 录像都走这套）

1. 在 `src/pipeline/` 下新建 `module_<name>/`（**注意是 `module`**），放 `<name>.h` / `<name>.cpp`。
2. 类继承 `PipelineNode`，实现四个纯虚函数：`create(int channel_id, const std::string& name)`、`start()`、`stop()`、`set_data(const char* config)`。
3. `start()` 起独立线程；`run()` 中 `m_queue.getBlock(100)` → 从 `MemMeta` 取数据 → 处理 → `push_input_pipe(block)`；终点节点改为 `m_queue.release(block)`。
4. 在 `src/pipeline/CMakeLists.txt` 的 `target_sources` 里加 `.cpp`，必要时在 `target_include_directories` 加目录。
5. 装配：**不要手写装配**。要验证一条新链路，改 `config/node.json` 让 `PipelineBuilder` 建（节点类型需先在 `NodeFactory::RegisterBuiltins()` 注册）；临时验证也可以在测试程序里手写 `MemInit` → `create` → `link_to` → `start`，但**不要往 `main_node.cpp` 里加节点名**——入口只剩编排。
6. 若引入新的数据类型，用 `MemMeta::set_ptr<T>()` 挂 slot，并在初始化处声明。
7. 节点自身耗时统计：`profile::NodeProfiler *p = profile::get_profiler(block); p->begin(m_name); ... p->end(m_name);`（判空；`end` 要放在等待下一帧的 sleep 之前）。
8. 多下游扇出前必须完成引用计数改造（见 §2.4 第 4 条）。

## R3 新增平台后端（例如 RGA 预处理）

1. 先读 `src/platform/include/processor_interface.h`，接口是唯一契约。
2. 在 `src/platform/<模块>/<backend>_<模块>.{h,cpp}` 实现，**不暴露 RGA 类型到接口**。
3. 在对应 `CMakeLists.txt` 用 `cmake/FindRGA.cmake` 查依赖，加 `target_link_libraries`。
4. 在工厂里加选择分支（参考 `capture_factory.cpp`）。
5. 同步 ARCHITECTURE §2.5 的接口实现表。

## R4 排障口诀

| 现象 | 先查 |
| --- | --- |
| `cmake -S . -B build` 配置失败 | P0-1：`install/x86_64` 缺失；确认是不是在本机（aarch64）构建 |
| 一启动就段错误（无摄像头 / x86） | P0-2 **已修（T7）**：采集后端不可用或设备/文件打不开时只打印 `[viN] open failed` 并退出该节点线程（不崩、其余链路照跑）；仍崩则查 P0-8（MPP 解码未分配 `FrameBuffer`） |
| 跑几秒后链路停摆、拿不到块 | 失败分支漏 `release` 导致池子耗尽（P0-3、P0-4 已修）；剩余检查 `push_input_pipe()` 队列满丢块（P0-5） |
| 画面撕裂 / 花屏 / 脏数据 | P0-5：多下游共用块被提前复用；**若 Y 正常、只有 UV 大块绿斑/花屏** → P0-13（`MppEncoder` 一次性拷贝，已修 `c557d89`，TESTLOG §5.11）；**若某一路上半屏正常、下半屏全绿（多路并发才出现，单路正常）** → P0-14（池块 `FrameBuffer` 视图被别的链路改写，采集层未重绑，已修 `eee3b01`，TESTLOG §5.12） |
| `rtsp://` 打开 404、服务关不掉 | P0-6（stop 的 key 与注册不一致）、P0-7（参数校验崩溃） |
| 进程 `Ctrl+C` 无反应 / 停不下来 | 已非 P1-16（T6 已把 `while(1)` 换成 `while (g_run)`）：检查 `main_node.cpp` 里 `signal(SIGINT/SIGTERM)` 是否在 `start_all` 之前装上；停止卡住通常是 `stop()` 在 `join()` 等某个节点线程不退出 |
| 进程退出慢 / `stop_all` 之后还打日志 | 正常：`stop_all` 正序（源头先停），下游要把队列里已到的块处理完；`PushNode::stop()` 里还有 `zlm_rtsp_stop`（媒体注销 + 关 8550 端口） |
| 链接期 `undefined reference to zlm_rtsp_*` | P1-1：`pipeline_api` 未链 `common_api` / `rtsp` |

## R5 文档更新规则

四份文档分工固定，不要混写：

| 文档 | 只写 | 不写 |
| --- | --- | --- |
| [README.md](../README.md) | 项目定位、能做什么、快速开始、状态摘要、文档导航 | 细节问题清单 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 稳定的分层、数据流、模型、扩展方式、规范 | 临时问题 |
| [STATUS.md](STATUS.md) | 现状、完成度、P0–P2、优化建议 | 未来排期 |
| [ROADMAP.md](ROADMAP.md) | 目标、WBS、排期、验收、风险 | 现状细节 |
| [TESTLOG.md](TESTLOG.md) | **只有测试证据**：命令、结果摘要、结论、未做项（L1 冒烟 / L2 任务 / L3 版本三级） | 方案、现状盘点、规矩 |
| [BUG_CASES.md](BUG_CASES.md) | **已定案 bug 的原理剖析**：现象 / 函数级位置 / 根因 / 修复 / 定位手法 / 面试说法，附 §1 索引表 | 未修问题的跟踪（在 STATUS）、测试命令与结果（在 TESTLOG） |
| **HANDOFF.md（本文）** | **接手所需的事实快照 + 红线规定 + 操作规程** | 重复上面四份的内容 |
| `.clinerules/00-handoff.md` / `AGENTS.md` | 入口索引：现状卡（与本文 §0 一致）、跳读表、硬约束 7 条 | 任何细节展开 |

改动后同步更新各文档头部"最后更新"日期。**纯文档提交不更新校准点**（见本文开头的说明）。

## R6 结束一轮工作 / 再次切换对话前

1. 更新本文 **§0 事实卡**、**§1.2 关键坐标**（HEAD、分支、远程 ref）与 **附录 B 变更记录**。
2. 同步更新 **`.clinerules/00-handoff.md` 与 `AGENTS.md` 里的现状卡**（两处内容与 §0 保持一致）。
3. 确认工作树干净，或明确记录未提交改动的意图。
4. 若 STATUS.md 中的问题被修复，同步删改对应行。
5. 若新增/删除章节，检查 §0.1 跳读表与 `.clinerules/00-handoff.md` 的章节引用是否仍准确（两处都按章节名引用，不写行号，避免漂移）。

---

# 附录 A　接手自检命令（复制粘贴即可）

```bash
cd /mnt/nvme/workspace/EdgeVisionService

# 1) git 状态：应与 §1.2 一致（分支 rk3588、工作树干净；HEAD 不比校准点旧即正常）
git --no-pager branch -vv
git --no-pager log --oneline -5
git status -sb
git --no-pager remote -v

# 2) 环境：应与 §1.3 一致（aarch64 / cmake 3.22 / g++ 11.4 / 8 核）
uname -m; cmake --version | head -1; g++ --version | head -1; nproc

# 3) 依赖是否在位
ls install/rk3588/lib | grep -E 'libmk_api|librockchip_mpp|libavcodec'
ls install/rk3588/include | grep -E 'zlmediakit|ZLMediaKit|libavcodec'

# 4) 构建（约 1 分钟；失败先对照 §2.2 与 STATUS P0-1）
cmake -S . -B build >/dev/null && cmake --build build -j"$(nproc)" 2>&1 | tail -20

# 5) 冒烟运行：期望输出 "EdgeVisionService start"
./build/run.sh EdgeVisionService

# 6) 真实主链路（有 /dev/video0 时；无设备会按 P0-2 崩溃，属已知）
timeout 20 ./build/run.sh testnode || echo "testnode 退出码 $?（见 STATUS P0-2/P1-16）"
```

# 附录 B　变更记录

| 日期 | 变更 | 基线提交 |
| --- | --- | --- |
| 2026-11-05 | 新建本文：整理接手流程，明确"当前情况 / 必须遵守的规定"两部分 | `e30ccb3` |
| 2026-11-05 | 清理 remote 遗留的 `src/common/media/`、`src/pipeline/moudleRtsp/`，强推 `rk3588`；文档去除 media 试验框架描述 | `e30ccb3` |
| 2026-11-05 | 新增 AI 自动接手入口 `.clinerules/00-handoff.md`、`AGENTS.md`；HANDOFF 增加 §0 事实卡、§0.1 按需跳读表；§1.2 坐标前移 | `15e4438` |
| 2026-11-05 | 新增 `config/push.json` 推流配置（分辨率/编码/协议/端口/fps/码率）并打通到采集与推流节点；HANDOFF 同步 §0/§1.2/§1.4/§1.6/§1.8/§1.9，校准点前移 | `283bab5` |
| 2026-11-05 | 新增节点耗时统计（`common/profiler`，每块一个 `NodeProfiler`）与终点节点 `PrintNode`，链路变为 vi→rtsp→print；顺手修掉 `ViNode`/`PushNode` 的 `std::atomic<bool> running_` 未初始化（P0-12）。HANDOFF 同步 §0/§1.2/§1.4/§1.6/§1.7/§1.8/§1.9/R2，校准点前移 | `9678e42` |
| 2026-11-05 | 报表增加实测 fps（`report(frame_no, fps)` + `PrintNode` 窗口统计）；用 30/15/10/60 fps 实测校正「首尾跨度」与帧率的口径 | `3ee3913` |
| 2026-11-05 | 修复 P0-3：`read_frame` 失败分支未归还 `MemBlock`，fps 配得比采集端高时几十帧后池子耗尽、链路静默停摆（gdb 栈定位到 `MemPool::acquire`）。校准点前移 | `d158eca` |
| 2026-11-08 | 新增预处理框架 `src/common/preprocess/`（`pre::ImagePreprocessor` 门面 + `FrameConverter` / `FrameDrawer` / `OsdRenderer` / `glyph_source`；像素级实现留 TODO，未实现分支返回 `ERR_NOT_IMPL`）；新增开关 `BUILD_FREETYPE_OSD`（默认 OFF，`install/` 无 freetype，OSD 先用内置点阵字体）；同步 ARCHITECTURE §1/§3、README §3.1、本文 §1.4/§1.9。**已入库，见右列 commit** | `e82e39e` |
| 2026-11-08 | 新增中间节点 `pipeline/moudle_vpss/`（`VpssNode`）：生命周期/配置解析/槽位挂载/耗时统计/失败归还等样板代码已实现并实测通过（透传、输出帧、池子不泄漏），`process_frame()` 留空返回 true（等价透传）由使用者填；`pipeline/CMakeLists.txt` 加源文件与 `common/preprocess` 头路径；同步 ARCHITECTURE §1、本文 §1.4/§1.9。**尚未接入 `main_node.cpp`**（装配留到 M1 T5/T6） | `e82e39e` |
| 2026-11-08 | 新增设计文档 `docs/DESIGN_MULTI_PIPELINE.md`（提案，无代码改动）：`push.json` → 多链路 `node.json` + 全局配置对象 `NodeConfigManager` + 进程级资源上提（`MediaServer` 等 16 项）；含行业方案调研、四方案比选（A 轻量装配 → B 内核注入）、M1–M3 分期与人日、风险表、M1 文件级改动清单。同步本文 §1.4/§1.9 | `d9f5ca2` |
| 2026-11-08 | `DESIGN_MULTI_PIPELINE.md` 二次修订（纯文档）：与用户确认 8 项决策 D1–D8（**目标 4 路 × 30fps = 1 路 `/dev/video0` + 3 路 mp4 循环**；RTSP 共用 8550 端口只分 `stream`；不做热加载/鉴权/子码流；保持单进程；**迁移完成后删除 `config/push.json`**）；新增 §10 概念详解（主/子码流必要性、热加载边界、鉴权 vs rtsps）、§11 4 路容量估算与板端验收阈值；任务表加 **T7b**（`FfmpegCapture` 循环 + 缩放，当前 EOF 即停、且不缩放）、**T10a**（池块数 50 / 队列深度 20 配置化，4 路必做），M1 工期 5–8 人日。**无代码改动，仍为提案** | `d9f5ca2` |
| 2026-11-08 | 新增 `docs/TESTLOG.md`（测试与验证记录）：按 **L1 冒烟 / L2 任务 / L3 版本** 三级粒度记录「命令 + 结果摘要 + 结论 + 未做项」，含模板、必填项、维护约定与 token 成本评估；归档既有三项结论（基线冒烟、`VpssNode` 验证、`node.json` 语法校验）。同步本文 §1.4/docs 行、§1.9 跳读表、R5 文档分工表 | `d9f5ca2` |
| 2026-11-08 | 提交入库：`e82e39e`（预处理框架 + `VpssNode`，22 文件；提交前构建 `EXIT=0`）+ `d9f5ca2`（多链路设计 + `config/node.json` + `TESTLOG` 制度，6 文件）。同时回写 §0 事实卡、§1.2 坐标（HEAD / 代码校准点）、`.clinerules/00-handoff.md` 与 `AGENTS.md` 现状卡，并把附录 B 里标「未提交」的行补上实际 commit | `e82e39e` / `d9f5ca2` |
| 2026-11-08 | **T1 修 P0-6 / P0-7**（`src/platform/rtsp/zlm_push_server.{cpp,h}` + 新增 `scripts/tests/`）：流/服务 key 改为 ctx 自带并由 `ServerKey` / `StreamKey` 唯一生成（原 `bind_ip` 从未赋值，stop 拼出 `":8550"` 故停止路径静默失效）；新增 `ReleaseServerRef()` 收敛三处 `ref_cnt` 释放（原重复注册分支 `ref_cnt++` 后直接 return，导致 server 永久滞留、`mk_stop_all_server()` 永不执行）；stop / 送帧改为先按句柄在表内定位再解引用（避免已停句柄 UAF）；`!out_code` 判空移到最前；新增只读自检 `zlm_rtsp_stream_count` / `zlm_rtsp_server_count`。验收：`./scripts/tests/run_t1_zlm_rtsp_key.sh` → `ALL PASS (fails=0)`（18 项），全量构建 `EXIT=0`、冒烟 `EdgeVisionService start`，详见 TESTLOG §5.5 | `87127e0` |
| 2026-11-08 | 内存池归属定案（两次纯文档提交，**无代码改动、无需重新编译**）：`f87211f` 新增 `DESIGN_MULTI_PIPELINE.md` **§11.6**（现状事实：池为进程级单例、块尺寸统一硬编码 1080p、**一块挂所有 profile**、单一 `free_list_`、`acquire(name)` 无归属语义；A/A+/B 三方案对比；共享池三风险；内存账 199MB/92MB/63MB；DeepStream、GStreamer、ZLMediaKit、SRS、NVR 业界对照）+ `node.json` 增补 `global.pool.quota_per_channel`；`b6c639c` 把结论记为 **D9 = A（共享池 + 每路配额）**，A+ 不实施，**T10a 范围与人日扩为 1.5**（块数 + 队列深度 + 配额计数 + 丢帧分类计数 + 健康指标） | `f87211f` / `b6c639c` |
| 2026-11-08 | **T2 新增 `NodeConfigManager`**（新模块 `src/common/configmgr/`：`node_config.h` 数据模型 + `node_config_manager.{h,cpp}`；`src/common/CMakeLists.txt` 注册源文件与头路径）：v1 `node.json` 解析（字段级校验、错误带字段路径如 `pipelines[0].nodes[1].config.port`、`${global.*}` 整值引用保留 JSON 类型、节点未写时注入 `global.media.rtsp.{port,app}`、默认值合并）+ v0 `push.json` 单链路适配（`vi` → `push(name=rtsp)` → `print`，与现 `main_node.cpp` 一致）；单例 + 不可变快照，**校验失败不破坏已生效快照**，`load_with_fallback()` 优先 v1、显式路径不回退。新增验收脚本 `scripts/tests/run_t2_node_config.sh` + 7 份 `fixtures/*.json`（53 项断言 → `ALL PASS (fails=0)`），全量构建 `EXIT=0`、冒烟 `EdgeVisionService start`，详见 TESTLOG §5.6。**尚未接入 `main_node.cpp`**（T5/T6 才用）；`global.pool.*` 仍无人读取（T10a）；`config/node.json` 的本地改动按约定未入库 | `8d87731` |
| 2026-11-08 | **T5 新增 `NodeFactory` + `PipelineBuilder`**（`src/pipeline/node_factory.{h,cpp}`、`pipeline_builder.{h,cpp}`）：`NodeFactory` 为 type→节点单例注册表（内置 `vi`/`vpss`/`push`/`print` 集中注册在 `RegisterBuiltins()`，重复注册被拒不覆盖，未知 type 返回 `nullptr` 并写 `last_error()`，**不抛异常**）；`PipelineBuilder` 消费 `NodeConfigManager` 的 `PipelineDesc`，先 `check_order` 校验拓扑（R1 至少 2 节点 · R2 首节点必须 `vi` · R3 末节点必须 `push`/`print` · R4 `vi` 只能首位 · R5 `print` 只能末位，错误带字段路径 `pipelines[i].nodes[j].…`），再按 `create → node->create → set_data → link_to` 装配连线，`build_all` 单链失败隔离（N3）、`start_all` 倒序 / `stop_all` 正序。`pipeline/CMakeLists.txt` 加 2 源文件并 **`PUBLIC` 链 `common_api`**（同时消除 §1.4 第 4 条那处隐式依赖）；新增 `scripts/tests/run_t5_builder.sh` + `t5_builder.cpp`（64 断言 → `passes=64 fails=0 skips=2`），真实 `config/node.json` 建成 4 条链（ch0 4 节点、ch1~ch3 各 3 节点），T2/T3 回归通过，详见 TESTLOG §5.8。**只 build 不 start**（启停顺序留 T6/T7 板端验）；`main_node.cpp` 仍未接入（T6） | `b461ece` |
| 2026-11-08 | **T6 重写真实入口 `main_node.cpp`（builder 驱动 + SIGINT 优雅退出）**：删除 v0 手写装配（3 个 `make_shared` + 2 次 `link_to` + 自写候选路径读取 `load_push_config()`）、`while(1)` 死循环与永不执行的 `stop()`；新流程 `NodeConfigManager::load_with_fallback(argv[1])` → `MemManager::MemInit()` → `PipelineBuilder::build_all()`（单链失败只告警、全失败退出码 1）→ `std::signal(SIGINT/SIGTERM)` → `start_all()` → `while (g_run) 200ms` → `stop_all()` → `sets.clear()` → `FreeMem()` → `return 0`；信号处理只写 `volatile sig_atomic_t`（异步信号安全）；入口不再出现任何节点名/设备路径，并显式打印「配置里写了但尚未接线」的 `global.pool.*`（T10a）。实测：v0 单链路真拉 `/dev/video0` 12s（30fps 稳定）→ SIGINT → `vi0 → rtsp0 → print0` 正序停止 → ZLM 媒体注销 + 关 8550 → `FreeMem 完成` → `EXIT=0`、无残留；v1 `node.json` 4 条链全部建成并启动、逐链正序停止后同样 `EXIT=0`（ch1~ch3 因 `ViNode` 写死同一 `/dev/video0` 打不开设备，属 P0-2/T7）。T5 `passes=64` / T3 `passes=104` / T2 `ALL PASS` 回归通过，详见 TESTLOG §5.9 | `29ab58b` |
| 2026-11-09 | **T7 `ViNode` 按 `vi.src` 前缀选采集后端 + 修 P0-2**（`pipeline/moudle_vi/moudle_vi.{h,cpp}`）：新增匿名命名空间 `ResolveSrc()`（`dev:`→`CAPTURE_V4L2`；`file:` / `rtsp://` / `rtmp://` / `http://` / `udp://`→`CAPTURE_FFMPEG`；无前缀按老规则兜底。`dev:` 只写设备路径时补成 V4L2 契约的 5 段 `设备:宽:高:格式:fps`，写一半直接报错）；`set_data()` 优先读 `vi` 段、回落 `push` 段，`src` 解析失败只告警、不静默降级；`run()` 改用 `CreateCapture(m_capture_type)`，工厂返回 `nullptr` 时报错并退出该节点线程（**P0-2 不再空指针解引用**），`open()` 失败分支补 `m_queue.release(block)`；`vi.src` 缺省时行为与 T7 前一致（v0 `push.json` 不变）。实测：v0 单链路不变（29.6–30fps）→ `EXIT=0`；v1 四路 = `dev:/dev/video0`（V4L2, 1920×1080）+ 3 个 `file:...mp4`（FFmpeg, 1024×576）全部 open 成功、**四路同时出帧** 29.96/23.96/23.93/23.94fps → SIGINT → 逐链正序停止 → `FreeMem` → `EXIT=0`；设备/文件打不开时只 `[vi0] open failed` 并干净退出（无段错误）；T5 `passes=64` / T3 `passes=104` / T2 `ALL PASS` 回归通过，详见 TESTLOG §5.10 | `eac4bb0` |
| 2026-11-09 | **修 P0-13：`MppEncoder` 按平面拷贝 NV12**（`platform/encoder/rk_mpp_encoder.cpp`，+26/−1）：`encode()` 原先把 `fb->data[0]` 起 `data_size` 字节一次性 `memcpy` 进 MPP buffer，但 `FrameBuffer::BindDataPointers()` 里 `data[1]`（UV）偏移按**池块分辨率**算（`base + stride[0] * 池块高`），本帧比池块小时（1024×576 的 mp4 跑在 1920×1080 池块上）UV 段拷到的是本帧 Y 之后的内存残影 → 播放端 Y 正常、UV 大块绿斑/花屏（T7 让四路出现不同分辨率后暴露）。改为 Y、UV 分别从 `data[0]`/`data[1]` 拷贝（UV 紧接 Y，offset = `hor_stride * ver_stride`），`data_size == 0` 按 `stride * height * 3 / 2` 兜底，`data[0]`/`data[1]` 为空时归还 buffer 并返回 false。实测：四路 45s → 编码错误 0、29.82/23.94/23.93/23.96fps、SIGINT 正序停止 `EXIT=0`；拉流 UV 统计 `U=128.1±10.9 / V=128.0±1.6`（修复前 `105.4±41.1 / 105.5±41.2`，源 `126.6±11.0 / 128.2±1.7`），ch0 1080p 无回归；T5/T3/T2 回归通过，详见 TESTLOG §5.11 | `c557d89` |
| 2026-11-09 | **修 P0-14：两个采集后端写块前按本链路分辨率重绑 `FrameBuffer` 视图**（`platform/capture/v4l2_source/v4l2_capture.cpp` +24/−1、`platform/capture/ffmpeg_source/ffmpeg_capture.cpp` +9）：`MemMeta` 里的 `FrameBuffer` 是**块级共享视图**（`data[0]/data[1]/width/height/stride` 存在块上，四条链路共用同一个全进程池块），`FfmpegCapture::transfer_frame()` 写完自己那帧就把视图改成自己的分辨率（1024×576 / `hw_stride=1024`）再随块归还池子；`V4l2Capture::read_frame()` 取到这种块后**直接沿用** `frame->stride[0]` 去开 `sws_scale`，于是 1080p 路按 `stride=1024` 写 1080 行、Y 平面只覆盖 `1024×1080` 字节（= 576 行 × 1920），剩余部分保持旧数据/0 → 播放端**上半屏正常、下半屏（约 47% 以下）全绿**，且随块复用**间歇出现**（单跑 ch0 时 176 帧全好）。改为两个采集后端在 `sws_scale` 之前各自 `BindMemory(data[0], 本链路宽, 本链路高, FMT_NV12[, 128])`（`data[0]` 恒为块首地址；V4L2 侧固定 NV12 因 `sws_getContext` 输出恒为 `AV_PIX_FMT_NV12`，ffmpeg 侧 `align=128` 与原 `hw_stride` 算法一致，Y/UV 指针改取重绑后的 `data[0]/data[1]`），V4L2 侧另补 `sws_scale` 返回行数校验（异常则 `VIDIOC_QBUF` 归还并丢帧）。实测：四路并发拉流解码 ch0（1920×1080）**坏帧 0/166**（修复前四路 51/64、两路 63/129）、ch1（1024×576）**0/73**；四路 fps 照常（29.9 / 23.9×3）；T5 `passes=64` / T3 `passes=104` / T2 `ALL PASS`；冒烟 `EdgeVisionService start`，详见 TESTLOG §5.12 | `eee3b01` |
| 2026-11-09 | 新增 **`docs/BUG_CASES.md`（bug 档案 · 面试要点）**：收录 9 个已定案缺陷（B1 RTSP key/ref_cnt、B2 出参判空、B3 漏 release 抽干池、B4 atomic 未初始化、B5 工厂 nullptr、B6 UV 平面错位、B7 池块视图被改写、B8 `while(1)` 挡停止、B9 配置值与素材不符），每案含**现象 / 函数级位置 / 根因原理 / 修复 / 为什么难发现 / 面试怎么讲**，另附 10 条排查方法论（M1–M10）。同时把「**修掉有原理含量的 bug 必须追加档案**」写成强制规定：本文 **§2.2 第 10 条**、§2.9 自检清单、R5 文档分工表、§0.1 跳读表，以及 `.clinerules/00-handoff.md` 硬约束第 9 条与 `AGENTS.md` 入口清单（用户 2026-11-09 要求） | 本文纯文档，不改变代码校准点 |
| 2026-11-10 | **修 P0-9：`MemInit()`/`FreeMem()` 每轮净漏 12.44 MB**（`common/frame_buffer.h`、`common/MemManager/MemManager.cpp`，并删除已无引用的 `common/type_registry.h`）：`FrameBuffer` 头注释写「只是视图、不负责释放」，`Allocate()` 却 `new[]` 了一块，`FreeMem()` 只 `delete` `MemMeta`/`MemBlock` → 每块 6.22 MB 像素缓冲无人认领。现 `FrameBuffer` 增 `owned_mem`（`Allocate()` 记录的基址）+ `Release()`（`delete[]` 后清 `data[]`/`native`），`BindMemory()` 视图的 `owned_mem` 恒为 `nullptr`、`Release()` 不碰别人的内存；`MemInit()` 删掉残留的 `new MemBlock`、`Allocate()` 失败时 `delete` + 继续；`FreeMem()` 逐槽 `Release()` → `delete`。实测（`mallinfo2().uordblks + hblkhd` 口径）：修复前每轮 +12,443,648 B，修复后 init 12.52 MB → free 82 kB、6 轮零增长；四路 29.9/23.9fps → SIGINT → `FreeMem 完成`、退出码 0。详见 TESTLOG §5.13 / BUG_CASES §2.10 | 工作树（2026-11-10） |
| 2026-11-10 | **P0-10 收口：`FrameBuffer` 改 RAII + 禁拷贝**（`common/frame_buffer.h` +15/−3、`common/MemManager/MemMeta.h` −12）：P0-9 加的 `Release()` 仍"靠调用方记得调"，且结构体可拷贝（`Allocate()` 来的两份 `owned_mem` → 双释放）。现 `~FrameBuffer()` 调 `Release()`、拷贝构造/赋值 `= delete`，`MemMeta` 里 `slot_free<FrameBuffer*>` 特化随之删除（主模板 `delete t` 一句即归还），槽位侧不再需要认识 `FrameBuffer`。**加了析构必须连拷贝一起禁**——ASAN 反证：只加析构不禁拷贝＝隐式拷贝必双释放（`attempting double-free`，两次 `Release()`）。栈上视图（`BindMemory()`）`owned_mem == nullptr`，析构 no-op，实测外部缓冲不受影响。验证：`BUILD_RC=0`；`/tmp/meta_probe` 与 `/tmp/mem_probe 2 6` 数值与 P0-11 一致（6 轮恒定 82,080 B）；`/tmp/fb_real_probe`（真实头文件 + ASAN）零错误零泄漏；拷贝探针 2 处 `use of deleted function`；四路 `testnode` SIGINT → `释放 50 块 / 100 个槽位` + 退出码 0。详见 TESTLOG §5.15 / BUG_CASES §2.10 后续段 | 工作树（2026-11-10） |
| 2026-11-10 | **修 P0-11：`MemMeta` 槽位从不销毁 + 节点自建槽位漏收**（`common/MemManager/MemMeta.h`、`MemManager.cpp::FreeMem`、`pipeline/moudle_vpss/moudle_vpss.cpp` 注释）：`data_map_` 存 `void*`，而 `void*` 既不能安全 `delete`（UB、不调析构）也调不到 `MemSlot<T>::freeObj()` —— `set_ptr()` 覆盖同名即丢旧槽位（实测 32 B/次）、`FreeMem()` 只能按 `imgMap` 点名、`VpssNode` 懒创建的 `vpss_out` 帧整块 6.22 MB 漏收。现 `MemSlot` 继承 `MemSlotBase`（虚析构）+ `~MemSlot(){ freeObj(); }`，`MemMeta` 存 `MemSlotBase*` 并补 `clear()` / 析构 / 删拷贝，`set_ptr` 覆盖前先释放旧槽位（同一指针重复登记则保留在用对象），`slot_free<FrameBuffer*>` 先 `Release()` 再 `delete`，`data<T>` 改 `dynamic_cast` 校验（同修 P1-10）；`FreeMem()` 统一 `clear()`。实测：`MemMeta` 探针 2 槽位 12.52 MB → 覆盖同名不增帧 → `clear() released=2` → 回落 78 kB；`MemInit(2)+FreeMem()` 6 轮恒定 82,080 B（修复前每轮 +128 B）；四路 `testnode` SIGINT 打印 `FreeMem: 释放 50 块 / 100 个槽位`、退出码 0。详见 TESTLOG §5.14 / BUG_CASES §2.11 | 工作树（2026-11-10） |




[返回 README](../README.md)
