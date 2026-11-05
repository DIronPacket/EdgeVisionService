# EdgeVisionService 架构设计

> 描述当前代码的分层、目录、数据流、内存/线程模型、平台接口与扩展方式。
> 当前现状与已知问题见 [STATUS.md](STATUS.md)；未来计划见 [ROADMAP.md](ROADMAP.md)；项目入口见 [README](../README.md)。
> 最后更新：2026-11-05；适用分支：`rk3588`。

## 1. 目录结构

```text
EdgeVisionService/
├── CMakeLists.txt              # 平台识别、库类型、全局输出目录
├── cmake/
│   ├── FindFFMPEG.cmake        # 从 install/<platform> 查找 FFmpeg
│   ├── FindMPP.cmake           # 查找 Rockchip MPP
│   ├── FindZLMediaKit.cmake    # 查找 libmk_api.so
│   ├── FindRGA.cmake           # 查找 librga（尚未被引用）
│   └── rk3588.toolchain.cmake  # aarch64 交叉编译工具链
├── config/                     # 运行配置（push.json：推流分辨率/编码/协议/端口/fps/码率）
├── docs/                       # 设计文档
│   ├── ARCHITECTURE.md         # 当前架构与扩展方式
│   ├── STATUS.md               # 当前现状与已知问题
│   ├── ROADMAP.md              # 未来扩展计划
│   └── HANDOFF.md              # 接手工作流（现状快照 + 规定 + 操作规程）
├── scripts/                    # 预留：部署/启动脚本（当前为空）
├── install/rk3588/             # 已放置 FFmpeg、MPP、ZLMediaKit、RGA 的头与库
├── src/
│   ├── CMakeLists.txt          # 聚合 common / pipeline / platform / main
│   ├── common/
│   │   ├── frame_buffer.h      # 多平面 FrameBuffer（data[3] + stride[3]）
│   │   ├── type_registry.h     # 旧版注册辅助（RegisterAll）
│   │   ├── json.hpp            # nlohmann/json 单头库
│   │   ├── profiler/           # 耗时统计（header-only）
│   │   │   ├── node_profiler.h # NodeProfiler：节点名 + 开始/结束时间戳、报表
│   │   │   └── block_profiler.h# get_profiler(block) 取用 + NodeCostScope（RAII）
│   │   └── MemManager/         # 内存池实现（正在使用）
│   │       ├── MemMeta.h       # MemMeta + MemSlot<T>
│   │       ├── MemPool.h/.cpp  # MemBlock / MemQueue / MemPool 单例
│   │       └── MemManager.h/.cpp
│   ├── platform/
│   │   ├── include/            # 抽象接口：capture / decoder / encoder / processor
│   │   ├── capture/            # FfmpegCapture + V4l2Capture + 工厂
│   │   ├── encoder/            # MppEncoder（RK）、FfmpegEncoder（x86，未完成）
│   │   ├── decoder/            # MppDecoder（RK，草稿）、FfmpegDecoder（x86，未完成）
│   │   └── rtsp/               # ZLMediaKit C API 封装（RTSP 服务 + 推流）
│   ├── pipeline/
│   │   ├── pipeline_node.h/.cpp# PipelineNode（裸指针、MemQueue）
│   │   ├── moudle_vi/          # 采集源节点 ViNode
│   │   ├── moudle_push/        # 编码推流节点 PushNode
│   │   └── moudle_print/       # 耗时打印节点 PrintNode（流水线终点）
│   └── main/
│       ├── main.cpp            # 目前仅为打印入口
│       └── test/               # testnode / testpool / 旧测试（部分未编译）
└── build/                      # 旧构建产物（CMakeCache 仍指向 /mnt/nvme/...，已失效）
```

## 2. 总体架构

### 2.1 分层与 CMake 依赖

```text
┌──────────────────────────────────────────────┐
│ main / test 可执行文件                        │
│   EdgeVisionService、testnode                 │
└───────────────┬──────────────────────────────┘
                │
┌───────────────▼──────────────────────────────┐
│ pipeline_api（业务节点）                       │
│   PipelineNode / ViNode / PushNode / PrintNode │
└───────────────┬──────────────────────────────┘
                │ PUBLIC platform_api
┌───────────────▼──────────────────────────────┐
│ platform_api（INTERFACE 聚合层）                │
│   capture / decoder / encoder / rtsp          │
└───────┬───────────────┬──────────────┬────────┘
        │               │              │
   FFmpeg/V4L2       MPP            ZLMediaKit
        │
┌───────▼──────────────────────────────────────┐
│ common_api（内存池 MemPool/MemManager）        │
│   pipeline 与 testnode 各自链接                 │
└──────────────────────────────────────────────┘
```

实际依赖关系有两点需要注意：

1. `platform_api` 是 `INTERFACE` 库，本身不产生二进制，只聚合子模块与头文件路径。
2. `pipeline_api` 只链接 `platform_api`，**没有链接 `common_api`**；`common_api` 是在可执行文件层（如 `testnode`）补链的。这会让 `libpipeline_api.so` 自身缺少对 `MemPool` 的 `DT_NEEDED`，属于隐式依赖，建议改为显式链接。

### 2.2 运行时数据流（当前主链路）

```text
V4L2 /dev/video0 ─┐
                  ├─ CaptureInterface::read_frame(FrameBuffer*) ── 写入池内存（NV12）
mp4 / RTSP 文件 ──┘
        │
        ▼
ViNode（独立线程）       getBlock → 取 FrameBuffer → 采集 + 30fps 节流（节流不计入耗时）
        │               profiler: begin("vi0") / end("vi0") 包住 read_frame
        ▼  push_input_pipe(block)（队列深 20，满则丢最老帧）
PushNode（独立线程）     getBlock → 取 FrameBuffer → MppEncoder::encode()
        │               profiler: begin("rtsp0") / end("rtsp0") 包住 encode
        │               编码回调 → zlm_rtsp_input_encodeData() → ZLMediaKit RTSP Server
        ▼  push_input_pipe(block)
PrintNode（独立线程）    getBlock → 取 profiler → 按 print.interval 打印各节点耗时与合计
        │
        ▼  m_queue.release(block) 归还池子（MemBlock::reset() 顺带清空 profiler）
```

报表形如（`print.interval=30`，即每 30 帧一行）：

```text
[print0] frame#60 耗时(ms): vi0=8.160 rtsp0=6.468 | 节点合计=14.628 首尾跨度=39.999
```

- **节点合计**（`busy_us()`）= 各节点 `end - begin` 之和（纯处理耗时）。
- **首尾跨度**（`span_us()`）= 最早 `begin` 到最晚 `end` 的墙钟时间。

两者差值**主要不是**队列排队，而是 `ViNode` 在 `end("vi0")` 之后、`push_input_pipe()` 之前那段**帧率节流 sleep**——为了让采集耗时不被 30fps 节流恒定撑到 33ms，`end` 必须写在 `sleep_for()` 之前，于是这段空闲落在 `begin→end` 之外、却仍在 `begin→end` 的跨度之内。

实测（只改 `config/push.json` 的 `fps`，`print.interval=30`）：

| `fps` | 帧节拍 `1/fps` | `vi0` | `rtsp0` | 节点合计 | 首尾跨度 | 分解（处理 + 节流 + 下游） |
| --- | --- | --- | --- | --- | --- | --- |
| 30 | 33.3 ms | 8.2 | 6.7 | 14.9 | **40.0** | 8.2 + 25.1 + 6.7 |
| 15 | 66.7 ms | 8.2 | 5.0 | 13.1 | **71.7** | 8.2 + 58.5 + 5.0 |
| 10 | 100 ms | 32.8 | 6.3 | 39.1 | **106.5** | 32.8 + 67.2 + 6.3 |

结论：**首尾跨度 ≈ 帧节拍 + 末端节点耗时**；队列排队实测仅 ~0.1ms（下游比上游快，`MemQueue` 不积压），一旦跨度明显超过「帧节拍 + 末端耗时」，就说明下游跟不上、开始排队丢帧（日志出现 `###`）。`fps=10` 那行 `vi0` 涨到 32.8ms 是另一回事：消费变慢后 V4L2 `DQBUF` 要等新帧，属采集侧阻塞，不算节流。

### 2.3 内存/流水线模型

当前只有一套实现：`common/MemManager`（内存池） + `pipeline/`（节点）。

| 维度 | 实现 |
| --- | --- |
| 内存块 | `pool::MemBlock` + `MemMeta`（`void*` + `MemSlot<T>`） |
| 内存池 | `pool::MemPool` 单例，全局 `free_list_` |
| 队列 | `MemQueue`（`OWNER` / `USER`） |
| 生命周期 | 队列名 + `user_cnt` 增减 |
| 节点接口 | `create(int, name)`、`set_data(const char*)`、裸指针 `link_to` |
| 节点耗时 | 每块一个 `profile::NodeProfiler`（`MemMeta` slot `"profiler"`），节点 `begin/end` 写入；块回收时清空 |
| 构建接入 | ✅ 被 `src/common/CMakeLists.txt` 和 `pipeline` 使用 |

已知短板：块没有独立的引用计数，多下游分支时会出现“一个分支归还后块被复用，另一个分支仍在读”的问题，详见 [STATUS.md](STATUS.md) 的 4.1 / 5.2。

### 2.4 线程、队列与背压

- **每个节点一个线程**：`ViNode::run()`、`PushNode::run()`、`PrintNode::run()` 各自 loop，`std::atomic<bool> running_` 控制退出（**必须在构造函数里显式初始化为 `false`**，C++14 的 `std::atomic` 默认构造不写值）。
- **队列容量**：`MemQueue::InitQueue(name, 20)`，队列满时 `push()` 丢弃最老的一帧并归还池子（drop-oldest）。
- **取帧超时**：`getBlock(100)` 毫秒，拿不到就回到循环检查 `running_`，停止延迟最多约 100ms。
- **引用计数**：`acquire()` 时 `addUser(owner)`；每个消费者队列 `pushBlock()` 再 `addUser(queue_name)`；消费者 `release()` 时 `subUser()`，计数归零才 `reset()` 并放回 `free_list_`。当前链路是单消费者，模型可用；多下游分支时会出现“一个分支归还后块被复用，另一个分支仍在读”的问题（见 [STATUS.md](STATUS.md)）。
- **无显式背压**：上游不等待下游，队列满即丢帧。对实时预览可接受，但需要把丢帧计数暴露为指标。

### 2.5 平台抽象接口

| 接口 | 关键方法 | RK3588 实现 | x86_64 实现 | 状态 |
| --- | --- | --- | --- | --- |
| `CaptureInterface` | `open/read_frame/set_capture_callback/close` | `V4l2Capture` | `FfmpegCapture` | 均已实现基本路径 |
| `EncoderInterface` | `open(w,h,CodingType,fps,bitrate)/encode/set_encode_callback` | `MppEncoder` | `FfmpegEncoder` | x86 版本未完成且签名不一致 |
| `DecoderInterface` | `open(is_h265)/decode/set_decode_callback` | `MppDecoder` | `FfmpegDecoder` | 均为草稿，未接入流水线 |
| `ProcessorInterface` | `process(src,dst)` | RGA（未实现） | OpenCV（未实现） | 仅接口 |

> 注：`EncoderInterface::open` 的第三个参数是 `CodingType`，而 `FfmpegEncoder` 与旧测试代码使用的是 `PixelFormat`，两者不兼容；`FfmpegDecoder` 还引用了当前 `FrameBuffer` 中不存在的 `linesize`、`pixel_fmt` 字段。x86 路径即使补齐依赖也需先修接口。

## 3. 构建系统现状

- `BUILD_RTSP_MODULE` 在 `src/platform/CMakeLists.txt` 中被使用，但**从未用 `option()` 定义**，因此 `rtsp` 子目录不会被加入构建。
- `common_api` 固定为 `SHARED`，未遵循顶层 `BUILD_SHARED_MODULE` 开关。
- `pipeline_api` 使用 `zlm_push_server.h`，但其 include 路径未加入 `src/platform/rtsp`，也未链接 `rtsp` 目标；`testnode` 只链接了 `libmk_api.so`，而 `zlm_rtsp_*` 符号由 `zlm_push_server.cpp` 提供，链接时会出现未定义符号（除非手动补链）。
- `main` 只链接 `platform_api`，未链接 `pipeline_api` / `common_api`，因此无法承载真实业务。

## 4. 扩展方式与开发约定

### 4.1 新增一个处理节点

以“算法节点（YOLO 推理）”为例：

1. 继承 `PipelineNode`，实现 `create(int channel_id, const std::string& name)` 设置通道号与节点名。
2. 实现 `start()/stop()/set_data(const char* config)`：
   - `push_input_pipe(block)` 只做入队，不做耗时处理；
   - `run()` 线程内 `m_queue.getBlock(100)` 取块、从 `MemMeta` 取 `FrameBuffer`、调用算法、把结果写入 `YoloRes` slot；
   - 处理完继续 `push_input_pipe(block)` 交给下游，终点节点调用 `m_queue.release(block)` 归还池子。
3. 在 `main` 或配置装配处创建节点并 `link_to()` 上下游。
4. 若新增数据类型，在 `MemMeta` 中用 `set_ptr()` 注册对应 `MemSlot<T>`，并在池初始化时声明。
5. 编写节点级单测：构造假块 → `push_input_pipe` → 校验输出 slot 与 `user_cnt` 归零。

### 4.2 配置草案（目标态：`config/channels.json`）

> 现状：`config/push.json`（提交 `283bab5`）已落地下文的推流子集——`enable/width/height/codec/fps/bitrate_kbps/gop/port/app/stream/protocol/transport`，由 `PushNode::set_data()` 与 `ViNode::set_data()` 解析并下发到采集与编码；`service`、`channels[]`、`source`、`pool`、`queue` 仍是目标态。

```json
{
  "service": {
    "log_level": "info",
    "metrics_port": 9100
  },
  "channels": [
    {
      "id": 0,
      "name": "front_door",
      "source": {
        "type": "v4l2",
        "uri": "/dev/video0",
        "width": 1920,
        "height": 1080,
        "format": "NV12",
        "fps": 30
      },
      "pool": { "blocks": 20, "images": [{ "format": "nv12", "width": 1920, "height": 1080 }] },
      "encoder": { "codec": "h265", "fps": 30, "bitrate_kbps": 2000, "rc_mode": "cbr", "gop": 150 },
      "rtsp": { "port": 8550, "app": "live", "stream": "ch0" },
      "queue": { "depth": 20, "policy": "drop_oldest" }
    }
  ]
}
```

配置加载后应做校验（分辨率与池容量匹配、端口不冲突、app/stream 唯一），失败时拒绝启动并打印明确错误。

### 4.3 命名与目录规范

- 目录统一小写复数：`modules/`、`pipelines/`、`platform/`；修正 `moudle` → `module`。
- 头文件 guard 使用 `EVS_<MODULE>_<FILE>_H`，避免跨目录冲突。
- 公共类型放 `common/`，平台相关实现放 `platform/<backend>/`，业务节点放 `pipeline/nodes/<name>/`。
- 对外接口不暴露第三方类型（FFmpeg/MPP 结构体），统一转换为内部 `FrameBuffer`。

### 4.4 提交与质量门禁

- 提交信息：`<模块>: <动词> <对象>`，例如 `pipeline: fix block leak on capture error`。
- 合并前必须：x86 构建通过、单测通过、无新增编译告警；涉及 RK 的改动需交叉编译通过并在板端跑 30 分钟。
- `main` 分支始终保持可构建；功能开发走 feature 分支，禁止直接提交到 `main`/`rk3588` 主干。

[返回 README](../README.md)
