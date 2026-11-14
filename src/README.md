# src/ —— 源码结构总览

> 一页看懂：**编译出什么、每层负责什么、帧怎么流动、改哪类需求去哪个目录**。
> 规定与红线不在本文件（见 [../docs/HANDOFF.md](../docs/HANDOFF.md) §2），任务→文件索引见其中 §1.9。

## 1. 构建产物（`make` 之后 `out/` 里有什么）

| 产物 | 类型 | 源码入口 | 说明 |
| --- | --- | --- | --- |
| `out/bin/evs_media` | 可执行 | `src/main/media/{main_media.cpp,media_service.cpp}` | **媒体进程（进程 1）**：装崩溃 handler → 读配置 → `MemInit` → `build_all` → 信号 → `start_all` → 有序停止 → `FreeMem`。编排真源是 `media_service.cpp` |
| `out/bin/evs_comm` | 可执行 | `src/main/comm/main_comm.cpp` | **通信进程（进程 2）**：读 `config/comm.json` → `CommManager` 建通道（UDP/MQTT）→ 统一收包/状态回调打印 → 信号 → `StopAll`+`Clear`。只链 `common_api`+`comm_api` |
| `out/bin/EdgeVisionService` | 可执行 | `src/main/main.cpp` | **历史兼容壳**：与 `evs_media` 调同一个 `evs::media::Run()`，只差崩溃报告里的程序名（`make run` 仍可用） |
| `out/lib/libcommon_api.so` | SHARED | `src/common/**` | 内存池 + 帧内存分配边界 + 配置模型/管理器 + 预处理 + 崩溃采集 |
| `out/lib/libpipeline_api.so` | SHARED | `src/pipeline/**` | 流水线节点 + 节点工厂 + 装配器 |
| `out/lib/libcomm_api.so` | SHARED | `src/comm/**` | 通信层：`CommBase` 抽象基类 + `UdpComm`/`MqttComm` 两种后端 + 全局单例 `CommManager`（依赖 `install/<平台>/lib/libmosquitto.so`） |
| `out/bin/test_comm` | 可执行 | `scripts/tests/t6_comm.cpp` | T6 通信层验收（43 项断言；MQTT 无 broker 时 SKIP）。**`evs_media` 已链 `comm_api` 但尚未调用**（不建通道、不上报 `media/evt/*`） |
| `capture` / `decoder` / `encoder` / `rtsp` | 子库（由 `platform_api` 以 `INTERFACE` 聚合） | `src/platform/{capture,decoder,encoder,rtsp}` | 采集 / 解码 / 编码 / 推流的具体后端实现 |
| `src/main/test/*.cpp` | **当前不参与构建** | `main_node.cpp` / `main_testpool.cpp` / `main_encode.cpp` / `main_pushStream.cpp` | 历史验证程序（`build/bin` 里的 `testnode`/`testpool`/… 是 2026-11 的旧产物）。`src/main/CMakeLists.txt` 末尾 `# add_subdirectory(test)` 处于注释状态 |

**编哪个进程由 `EVS_PROCESS` 决定（2026-11-14 起）**：不指定 = `all`（`evs_media` + `evs_comm` + 兼容壳）；`-DEVS_PROCESS=media|comm|media,comm` 只编子集；`make media` / `make comm` 只编单个 target；`make proc PROC=comm` 改默认集并写进 `out/CMakeCache.txt`。未选中的 target 仍存在（`EXCLUDE_FROM_ALL`），随时可单独构建。

> 2026-11-14 起 `src/main/` 里是三个 target：`media/main_media.cpp` → **`evs_media`**（媒体进程；编排真源是 `media/media_service.cpp`）、`comm/main_comm.cpp` → **`evs_comm`**（通信进程）、`main.cpp` → **`EdgeVisionService`**（历史兼容壳，调同一个 `evs::media::Run()`）。默认三个都编；`EVS_PROCESS` 可只选其一（见上一段）。

## 2. 目录树与职责

```text
src/
├── CMakeLists.txt          add_subdirectory：common → pipeline → platform → comm → main
│                           （algorithm / streamer 两行仍被注释，见 §7）
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
├── main/                   → 进程入口（三个 target，默认编哪些由 EVS_PROCESS 决定）
│   ├── CMakeLists.txt      解析 EVS_PROCESS → EVS_WITH_MEDIA / EVS_WITH_COMM；未选中的 target 走 EXCLUDE_FROM_ALL
│   ├── media/              → evs_media（进程 1）
│   │   ├── main_media.cpp          入口：argv → app_name="evs_media" → evs::media::Run()
│   │   ├── media_service.{h,cpp}   编排真源（配置 → MemInit → build_all → 信号 → stop_all → FreeMem，日志前缀 [main]）
│   │   └── CMakeLists.txt          链 common_api + comm_api + pipeline_api + platform_api，-rdynamic
│   ├── comm/               → evs_comm（进程 2）
│   │   ├── main_comm.cpp           读 config/comm.json → CommManager 建通道 → 收包/状态打印 → 信号 → StopAll+Clear
│   │   └── CMakeLists.txt          只链 common_api + comm_api（不碰 pipeline/platform）
│   ├── main.cpp            历史兼容壳 → EdgeVisionService（同样调 evs::media::Run()）
│   └── test/               历史验证程序（当前未构建）
├── algorithm/              空目录（预留：算法模块）
└── comm/                   → comm_api（通信层）：comm_types / comm_base / udp_comm / mqtt_comm / comm_manager
                            抽象基类 + UDP/MQTT 两种后端 + 全局单例管理器；说明见 comm/README.md
```

## 3. 依赖关系（谁可以 include 谁）

```text
        ┌────────────────────────────────────────────┐
        │ main/media → evs_media（+ 兼容壳同名逻辑）  │
        │   只依赖 configmgr / pipeline_builder /     │
        │   MemManager / crash_handler               │
        └───────────────┬────────────────────────────┘
                        │
        main/comm → evs_comm ──▶ comm_api + common_api
                                 （不 include pipeline / platform）
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

三条不可违反的方向：① `common` 不许 include `pipeline`/`platform`；② `platform` 只暴露 `include/*_interface.h` 的纯虚接口给上层，`moudle_*` 里不直接 `#include "v4l2_capture.h"`；③ 节点之间不直接调用，只通过队列传 `MemBlock`。④ `comm` 是**独立旁路层**：只依赖 `Threads` + `mosquitto`，不 include `pipeline`/`platform`/`common`；唯一使用者是通信进程 `src/main/comm/`（`evs_comm`）。媒体进程 `evs_media` 只**链**了 `comm_api`，尚未真正收发（`media_service.cpp` 不建通道）。

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
| 发/收通信消息（UDP / MQTT） | `CommManager::getInstance()`（`comm/comm_manager.h`） | `Create(CommConfig)` → `Start()` → `Send*()`；收包走 `SetReceiveCallback`（带通道名）。**不要在回调里 Stop/Destroy**（自 join 卡死）。语义见 `comm/README.md` §3：MQTT 的"server"是订阅端角色，broker 是独立进程 |

## 6. 常见改动落点

| 我要改 | 去哪 | 配套必做 |
| --- | --- | --- |
| 推流参数（分辨率/编码/协议/端口） | `config/*.json`（不用改代码） | 跑 `scripts/tests/run_t3_node_json.sh` |
| 采集行为（新增源类型 / 打开参数） | `platform/capture/**` + `moudle_vi.cpp::ResolveSrc()` | `capture_interface.h` 契约不能破 |
| 编码器参数 | `platform/encoder/rk_mpp_encoder.*` | `encoder_interface.h` |
| 新增节点类型 | `pipeline/moudle_xxx/` + `node_factory.cpp` 注册 | 拓扑规则 R1~R5（`pipeline_builder.cpp`） |
| 配置字段 / 校验规则 | `common/configmgr/node_config_manager.cpp` | `run_t2_node_config.sh`（53 断言）必跑 |
| 内存池 / 块生命周期 | `common/MemManager/**`、`common/frame_mem.*` | 红线 5；改完按 `docs/history/TESTLOG.md` 记 L2 |
| 加通信后端 / 改通信行为 | `src/comm/{comm_base,udp_comm,mqtt_comm,comm_manager}.cpp` | 新后端：`CommType` 加枚举 + `CommManager::Create` 加分支 + 实现 5 个钩子；`run_t6_comm.sh` 必跑 |
| 加 / 拆进程、改进程编排 | 媒体：`main/media/media_service.cpp`；通信：`main/comm/main_comm.cpp` | `main/CMakeLists.txt` 的 `EVS_PROCESS`、`Makefile` 的 `media`/`comm`/`proc`/`run-media`/`run-comm`、TESTLOG §5.22 |
| 通信进程的通道配置（不动代码） | `config/comm.json` 的 `channels[]` | 字段语义见 `comm/comm_types.h::CommConfig`；`make run-comm` 冒烟 |

## 7. 两个"看起来能改其实是空的"地方

1. `src/algorithm/` 仍是**空目录**（`src/CMakeLists.txt` 里对应 `add_subdirectory` 被注释）。`src/comm/` 已于 **T6 落地**（`comm_api` + `test_comm`，说明见 [comm/README.md](comm/README.md)），现由通信进程 `src/main/comm/`（`evs_comm`）使用；**媒体进程还没接**（`media_service.cpp` 不建通道）。
2. `src/main/test/` 的 4 个 `main_*.cpp` 不参与构建（`build/bin` 里的同名可执行是旧产物）。想让 `testnode` 回来：取消 `src/main/CMakeLists.txt` 末行注释 → `make reconfigure && make`。

## 8. 命名与风格约定（照抄，别创新）

- 目录 `moudle_*` 是历史拼写，**不重命名**（HANDOFF §2 红线 2）。
- 库名：`common_api` / `pipeline_api` / `platform_api` + 子库 `capture` `decoder` `encoder` `rtsp`。
- 头文件保护：`EVS_<模块>_<文件>_H`（如 `EVS_NODE_CONFIG_H`）。
- 命名空间：`evs::common` / `evs::config`，节点类在全局（历史遗留，新增代码沿用现有文件的风格）。
- **C++14**：不用 `std::any` / `if constexpr` / 结构化绑定 / `std::optional`。
- 每个 `.h` 只放声明与少量内联；实现进 `.cpp`；`profiler` 是唯一"头文件实现"的例外（无状态模板化统计）。
