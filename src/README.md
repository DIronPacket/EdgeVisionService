# src/ —— 源码结构总览

> 一页看懂：**编译出什么、每层负责什么、帧怎么流动、改哪类需求去哪个目录**。
> 规定与红线不在本文件（见 [../docs/HANDOFF.md](../docs/HANDOFF.md) §2），任务→文件索引见其中 §1.9。

## 1. 构建产物（`make` 之后 `out/` 里有什么）

| 产物 | 类型 | 源码入口 | 说明 |
| --- | --- | --- | --- |
| `out/bin/EdgeVisionService` | 可执行 | `src/main/main.cpp` | **唯一真实主链路入口**：装崩溃 handler → 读配置 → `MemInit` → `build_all` → 信号 → `start_all` → 有序停止 → `FreeMem` |
| `out/lib/libcommon_api.so` | SHARED | `src/common/**` | 内存池 + 帧内存分配边界 + 配置模型/管理器 + 预处理 + 崩溃采集 |
| `out/lib/libpipeline_api.so` | SHARED | `src/pipeline/**` | 流水线节点 + 节点工厂 + 装配器 |
| `capture` / `decoder` / `encoder` / `rtsp` | 子库（由 `platform_api` 以 `INTERFACE` 聚合） | `src/platform/{capture,decoder,encoder,rtsp}` | 采集 / 解码 / 编码 / 推流的具体后端实现 |
| `src/main/test/*.cpp` | **当前不参与构建** | `main_node.cpp` / `main_testpool.cpp` / `main_encode.cpp` / `main_pushStream.cpp` | 历史验证程序（`build/bin` 里的 `testnode`/`testpool`/… 是 2026-11 的旧产物）。`src/main/CMakeLists.txt` 末行 `# add_subdirectory(test)` 处于注释状态 |

> ⚠️ **文档滞后提示**：`docs/HANDOFF.md` §1.9 曾写「真实入口是 `main/test/main_node.cpp`、`main/main.cpp` 是空壳」，那是 T6 之前的描述；`src/main/main.cpp` 文件头注释里也还留着「可执行名 testnode」。以代码为准：**`main.cpp` 已是真实入口**（T6），`main/CMakeLists.txt` 里产物名是 `EdgeVisionService`。

## 2. 目录树与职责

```text
src/
├── CMakeLists.txt          add_subdirectory：common → pipeline → platform → main
│                           （algorithm / comm / streamer 三行被注释，见 §7）
├── common/                 → common_api（被所有人依赖，自己不依赖业务层）
│   ├── MemManager/         内存池：MemPool / MemMeta / MemManager / media_define
│   ├── frame_buffer.h      帧视图（数据指针 + 宽高/fmt + stride + 所属池块）
│   ├── frame_mem.{h,cpp}   帧内存的唯一分配边界（rk3588: MPP DRM 缓冲；其他: new[]）
│   ├── configmgr/          node_config.h（数据模型）+ node_config_manager（解析/校验/快照）
│   ├── crash/              信号 → 报告 → 重抛 → core（crash_handler）
│   ├── profiler/           block_profiler / node_profiler（每节点每帧耗时，头文件实现）
│   ├── preprocess/         格式转换 / 画框贴图 / OSD 字形（见其 README.md）
│   └── json.hpp            vendored nlohmann::json 单头（**不要替换版本**）
├── pipeline/               → pipeline_api（编排层，不碰设备细节）
│   ├── pipeline_node.{h,cpp}      节点基类：线程 + 输入/输出队列 + link_to
│   ├── node_factory.{h,cpp}       type 字符串 → 节点实例（注册表，未知 type 返回 nullptr）
│   ├── pipeline_builder.{h,cpp}   配置 → 已连线链路；拓扑校验 R1~R5；start_all 倒序 / stop_all 正序
│   └── moudle_vi / moudle_vpss / moudle_push / moudle_print   四个具体节点（**目录名保留历史拼写 moudle**）
├── platform/               平台层：抽象接口 + 按平台选实现
│   ├── include/            capture/decoder/encoder/processor_interface.h（纯虚接口，上层只认这些）
│   ├── capture/            capture_factory + v4l2_source（dev:）/ ffmpeg_source（file:/rtsp://…）/ desktop_source（空）
│   ├── decoder/            ffmpeg_decoder / rk_mpp_decoder
│   ├── encoder/            ffmpeg_encoder / rk_mpp_encoder
│   └── rtsp/               zlm_push_server（ZLMediaKit：RTSP 服务 + 推流）
├── main/                   → EdgeVisionService
│   ├── main.cpp            真实入口（编排，不出现任何节点名）
│   └── test/               历史验证程序（当前未构建）
├── algorithm/              空目录（预留：算法模块）
└── comm/                   空目录（预留：通信模块，见 docs/design/DESIGN_PROCESS_SPLIT.md）
```

## 3. 依赖关系（谁可以 include 谁）

```text
        ┌────────────────────────────────────────────┐
        │ main (EdgeVisionService)                   │
        │   只依赖 configmgr / pipeline_builder /     │
        │   MemManager / crash_handler               │
        └───────────────┬────────────────────────────┘
                        │
        ┌───────────────▼──────────────┐   ┌─────────────────────────┐
        │ pipeline_api                 │──▶│ platform_api (INTERFACE) │
        │ (节点 + 工厂 + 装配)          │   │  capture/decoder/encoder │
        └───────────────┬──────────────┘   │  /rtsp 具体实现           │
                        │                  └───────────┬─────────────┘
                        └──────────┬───────────────────┘
                                   ▼
                          ┌─────────────────┐
                          │ common_api      │  ← 被上面两层依赖，自己**不**反向依赖
                          │ 池/帧/配置/预处理 │
                          └─────────────────┘
```

三条不可违反的方向：① `common` 不许 include `pipeline`/`platform`；② `platform` 只暴露 `include/*_interface.h` 的纯虚接口给上层，`moudle_*` 里不直接 `#include "v4l2_capture.h"`；③ 节点之间不直接调用，只通过队列传 `MemBlock`。

## 4. 一帧数据的流动（最短路径）

```text
[ViNode 线程]  CaptureInterface::capture() → FrameBuffer 视图
     ↓ 从池 acquire("nv12") 取块，把数据写进块内存（rk3588 上是 MPP DRM 缓冲）
     ↓ queue.push(block)   （节点名 + channel = 队列名）
[VpssNode]（可选，默认 enable=false）格式转换 / 缩放（preprocess）
     ↓
[PushNode 线程]  EncoderInterface::encode() → ZLMediaKit 推流（RTSP）
     ↓
[PrintNode]  打印各节点耗时 / 实测 fps → **release 回池**（链路终点负责回收）
```

## 5. 关键类型速查

| 需求 | 类型 / 文件 | 要点 |
| --- | --- | --- |
| 取/还内存块 | `MemManager::getInstance().acquire(name)` / `MemPool::release()` | 池是进程级单例（D9 方案 A：共享池 + 每路配额，配额尚未接线 T10a） |
| 描述一块数据 | `FrameBuffer`（`common/frame_buffer.h`） | 非拥有视图；`Allocate()/Release()` 走 `frame_mem.*`，RAII 于 `15cf975` 收口（拷贝/赋值 `= delete`） |
| 写一个节点 | `PipelineNode`（`pipeline/pipeline_node.h`） | 覆写 `create/set_data/run/stop`；`run` 里循环 `pop` → 处理 → `push` |
| 加一种节点 | `NodeFactory::RegisterBuiltins()` + `moudle_xxx/` | type 字符串 → creator；未知 type 必须返回 `nullptr` 并写 `last_error()` |
| 读配置 | `NodeConfigManager::getInstance()` | 单例 + 不可变快照；失败带字段路径；`load_with_fallback()` 支持 v0 |
| 采集后端 | `CaptureInterface` + `CreateCapture(type)` | `vi.src` 前缀决定后端（`dev:`→V4L2；`file:`/`rtsp://`/…→FFmpeg），映射在 `moudle_vi.cpp::ResolveSrc()` |
| 崩溃取证 | `common/crash/crash_handler.h` | `InstallCrashHandler()` 要早于任何线程；`SetCrashTag()` 把业务信息带进报告 |
| 看每帧耗时 | `common/profiler/{block,node}_profiler.h` | 头文件实现，无 .cpp；`print` 节点按 `interval` 输出 |

## 6. 常见改动落点

| 我要改 | 去哪 | 配套必做 |
| --- | --- | --- |
| 推流参数（分辨率/编码/协议/端口） | `config/*.json`（不用改代码） | 跑 `scripts/tests/run_t3_node_json.sh` |
| 采集行为（新增源类型 / 打开参数） | `platform/capture/**` + `moudle_vi.cpp::ResolveSrc()` | `capture_interface.h` 契约不能破 |
| 编码器参数 | `platform/encoder/rk_mpp_encoder.*` | `encoder_interface.h` |
| 新增节点类型 | `pipeline/moudle_xxx/` + `node_factory.cpp` 注册 | 拓扑规则 R1~R5（`pipeline_builder.cpp`） |
| 配置字段 / 校验规则 | `common/configmgr/node_config_manager.cpp` | `run_t2_node_config.sh`（53 断言）必跑 |
| 内存池 / 块生命周期 | `common/MemManager/**`、`common/frame_mem.*` | 红线 5；改完按 `docs/history/TESTLOG.md` 记 L2 |

## 7. 两个"看起来能改其实是空的"地方

1. `src/algorithm/`、`src/comm/` 是**空目录**（`src/CMakeLists.txt` 里对应 `add_subdirectory` 被注释）。`comm` 的规划见 [../docs/design/DESIGN_PROCESS_SPLIT.md](../docs/design/DESIGN_PROCESS_SPLIT.md)，尚未落地。
2. `src/main/test/` 的 4 个 `main_*.cpp` 不参与构建（`build/bin` 里的同名可执行是旧产物）。想让 `testnode` 回来：取消 `src/main/CMakeLists.txt` 末行注释 → `make reconfigure && make`。

## 8. 命名与风格约定（照抄，别创新）

- 目录 `moudle_*` 是历史拼写，**不重命名**（HANDOFF §2 红线 2）。
- 库名：`common_api` / `pipeline_api` / `platform_api` + 子库 `capture` `decoder` `encoder` `rtsp`。
- 头文件保护：`EVS_<模块>_<文件>_H`（如 `EVS_NODE_CONFIG_H`）。
- 命名空间：`evs::common` / `evs::config`，节点类在全局（历史遗留，新增代码沿用现有文件的风格）。
- **C++14**：不用 `std::any` / `if constexpr` / 结构化绑定 / `std::optional`。
- 每个 `.h` 只放声明与少量内联；实现进 `.cpp`；`profiler` 是唯一"头文件实现"的例外（无状态模板化统计）。
