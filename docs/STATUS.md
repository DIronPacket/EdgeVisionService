# EdgeVisionService 项目现状与已知问题

> 记录当前代码“做到哪、哪里会坏、先修什么”，不描述未来计划。
> 架构与扩展方式见 [ARCHITECTURE.md](ARCHITECTURE.md)；未来排期见 [ROADMAP.md](ROADMAP.md)；项目入口见 [README](../README.md)。
> 最后更新：2026-11-08；适用分支：`rk3588`。

## 1. 模块完成度总览

> 完成度为“代码可读 + 主路径已实现”的粗略评估，不代表已在板端验收。

| 模块 | 主要文件 | 完成度 | 说明 |
| --- | --- | --- | --- |
| 构建/平台探测 | 顶层 `CMakeLists.txt`、`cmake/Find*.cmake` | 🟡 60% | 平台探测与查找模块齐全；x86 缺依赖即失败；RTSP 未接入构建；工具链路径待确认 |
| 内存池 | `common/MemManager/*` | 🟡 65% | `MemPool`/`MemBlock`/`MemQueue` 可跑通单消费者链路；存在泄漏、无锁读取、类型不安全等问题 |
| 平台接口 | `platform/include/*` | 🟢 85% | 采集/编码/解码/预处理接口定义清晰；x86 编解码实现未对齐接口 |
| FFmpeg 采集 | `capture/ffmpeg_source/*` | 🟡 75% | 支持本地文件/摄像头/RTSP 输入，软解后 `sws_scale` 转 NV12 写入池；`AVDictionary` 构造后未实际传入 |
| V4L2 采集 | `capture/v4l2_source/*` | 🟡 65% | RK 专用；固定 YUYV 输入、缩放输出尺寸可由 `set_output_size()` 配置（默认 1920×1080）；无 `poll`、无缓冲区校验、失败路径未清理 |
| MPP 硬编码 | `encoder/rk_mpp_encoder.*` | 🟡 70% | H.264/H.265、NV12、FIXQP(QP=33)、GOP=5s、回调输出裸流；每帧 `memcpy`，无 EOS flush/动态重配 |
| FFmpeg 软编码 | `encoder/ffmpeg_encoder.*` | 🔴 15% | `encode()` 主体被注释；`open()` 参数是 `PixelFormat`，与接口的 `CodingType` 不一致；x86 未编译 |
| MPP 解码 | `decoder/rk_mpp_decoder.*` | 🔴 35% | 流程草稿；回调前未分配 `FrameBuffer` 内存即 `memcpy`，会崩溃；未接入流水线 |
| FFmpeg 解码 | `decoder/ffmpeg_decoder.*` | 🔴 15% | 使用已删除的 `avcodec_register_all()`；引用不存在的 `FrameBuffer::linesize/pixel_fmt`；未编译 |
| RTSP 推流封装 | `platform/rtsp/zlm_push_server.*` | 🟡 65% | 一键起服务/建流、送 H.264/H.265、停止释放；存在停止 key 不匹配、空指针解引用等问题 |
| 流水线节点 | `pipeline/pipeline_node.*`、`moudle_vi`、`moudle_push`、`moudle_print` | 🟡 70% | Vi→Push→Print 单链路可演示；推流分辨率/编码/协议/fps/码率已由 `config/push.json` 驱动；各节点耗时已可采集并周期打印；采集源路径与编码 QP 仍硬编码、错误路径丢块、无多分支引用计数 |
| 主程序 | `main/main.cpp` | 🔴 5% | 仅打印；真实入口在 `main/test/main_node.cpp` |
| 节点配置管理 | `common/configmgr/*` | 🟢 90% | v1 `node.json` 解析 + 字段级校验（错误带字段路径）+ `${global.*}` 引用展开 + 默认值合并 + v0 `push.json` 单链路适配；**尚未被 `main_node.cpp` 使用**（T5/T6 接入），`global.pool.blocks/queue_depth/quota_per_channel` 仍未被内存池读取（T10a） |
| 算法/通信/流媒体扩展 | 注释中的 `algorithm/comm/streamer` | ⚪ 0% | 仅目录规划 |
| 测试 | `main/test/*` | 🟡 25% | 有编码/推流/节点/内存池手工测试；`main_testpool.cpp` 已与当前代码脱节；无 CTest/单测框架 |
| 文档/配置/脚本 | `README.md`、`docs/*`、`config/`、`scripts/` | 🟡 35% | README/ARCHITECTURE/STATUS/ROADMAP 已建立；`config/push.json` 已生效；`scripts/` 仍为空 |

## 2. 已实现的主链路能力

1. **采集**
   - `FfmpegCapture`：按 `src` 前缀区分 `/dev/video*`、`rtsp/rtmp/http`、本地文件；解码后统一转 `AV_PIX_FMT_NV12`，按 `((w+127)/128)*128` 行宽写入 `FrameBuffer`。
   - `V4l2Capture`：解析 `设备:宽:高:格式:fps`（如 `/dev/video0:640:480:NV12:30`），`VIDIOC_S_FMT` 强制 YUYV，MMAP 4 个缓冲，`sws_scale` 转 NV12。
2. **内存池**
   - `MemManager::MemInit(50)` 预建 50 个 `MemBlock`，每个块挂一个 1920×1080 NV12 `FrameBuffer`，由 `MemPool` 单例统一管理 `free_list_`。
   - 源头节点通过 `MemQueue(OWNER)` 取块；下游节点通过 `MemQueue(USER)` 收块；队列深度 20，满则丢弃最老帧。
3. **编码**
   - `MppEncoder` 支持 H.264/H.265；`prep:hor_stride` 128 对齐、`MPP_FMT_YUV420SP`；固定 QP 33、GOP 150；编码结果通过 `EncodeCallback` 交给上层。
4. **推流**
   - `zlm_rtsp_onekey_start()` 初始化 ZLMediaKit、启动 RTSP 服务、创建 `mk_media` 并初始化视频参数。
   - `zlm_rtsp_input_encodeData()` 按 H.264/H.265 调用 `mk_media_input_h264/265`。
   - `zlm_rtsp_stop()` 按引用计数停止流与服务。
5. **节点编排**
   - `PipelineNode` 提供 `link_to` / `push_input_pipe` / `push_data`，`ViNode` → `PushNode` → `PrintNode` 的连线与线程启停已可运行。
6. **耗时统计**
   - `MemManager::MemInit()` 给每块挂一个 `profile::NodeProfiler`（`MemMeta` slot `"profiler"`），块回收时由 `MemBlock::reset()` 清空。
   - `ViNode` / `PushNode` 在拿到块后 `begin(节点名)`、处理完传下游前 `end(节点名)`（`steady_clock` 微秒；采集侧的节流 sleep 不计入）。
   - 终点 `PrintNode` 按 `config/push.json` 的 `print.interval`（默认 30 帧）打印单行报表：各节点耗时 + 节点合计 + 首尾跨度 + 实测 fps；打印后归还块。
   - 实测 fps 由 `PrintNode` 自己算（上一行报表到本行的帧数 / 墙钟时长），**不能拿 `1000/首尾跨度` 反推**（跨度是重叠窗口，会偏小）。`fps` 配得比采集端高时（如 60 配 V4L2 30fps），实测会稳定在采集端帧率。

## 3. 占位与未完成

- `ProcessorInterface` / RGA 预处理没有实现文件。
- `main.cpp` 未组装任何流水线，也没有命令行参数、配置文件加载、信号处理。
- `config/push.json` 已驱动推流分辨率/编码/协议/端口/fps/码率；`scripts/` 为空，采集源路径与编码 QP 仍写在代码里；`docs/` 已建立架构/现状/路线图文档。
- 没有统一日志（`printf`/`std::cout` 混用）、没有指标、没有健康检查。
- 没有 CTest/GoogleTest 单测；测试程序均为 `main()` 手工验证。
- `main_testpool.cpp` 仍是旧 API（`GlobalMemManager`、`FrameBuffer*`、`link_to(shared_ptr)`），与当前头文件不匹配，未参与编译。

## 4. 已知问题与风险

### 4.1 P0：阻断构建 / 必现崩溃 / 资源泄漏

| # | 问题 | 位置 | 影响 |
| --- | --- | --- | --- |
| 1 | x86 配置强制从 `install/x86_64` 找 FFmpeg，但仓库只有 `install/rk3588` | `cmake/FindFFMPEG.cmake:11-15` | `cmake -S . -B build` 直接失败，x86 无法开发/调试 |
| 2 | `ViNode` 固定创建 `CAPTURE_V4L2`，x86 下工厂返回 `nullptr` | `pipeline/moudle_vi/moudle_vi.cpp::run` | 随后 `m_capture->open()` 空指针崩溃 |
| 3 | ✅ 已修复（`d158eca`）：`read_frame()` 失败时直接 `continue`，未归还已取的 `MemBlock` | `pipeline/moudle_vi/moudle_vi.cpp::run` | 配置 fps 高于采集帧率时反复失败，池子几十帧内耗尽，整条链路静默停摆 |
| 4 | ✅ 已修复（`283bab5`）：`enc.open()` 失败时 `continue`，未归还 `MemBlock` | `pipeline/moudle_push/moudle_push.cpp::run` | 同上，且编码器会反复重试失败 |
| 5 | `push_input_pipe()` 向多个下游推同一块，但不增加引用计数 | `pipeline/pipeline_node.cpp:28-38` | 多分支时第一个下游释放后块可能被复用，其他分支读到脏数据 |
| 6 | ✅ 已修复（T1）：`zlm_rtsp_stop()` 曾用 `bind_ip + port` 拼流 key（`bind_ip` 从未赋值 → `":8550"` ≠ 注册时的 `"8550"`），服务 key 同样不一致；现由 ctx 自带 `key`，`ServerKey` / `StreamKey` 为唯一生成入口。**同修 ref_cnt 泄漏**：重复注册分支 `ref_cnt++` 后直接 `return`，最后一个流 stop 时计数归不到零、`mk_stop_all_server()` 永不执行 | `platform/rtsp/zlm_push_server.cpp` | 找不到流直接 return，`mk_media` 不释放，服务引用计数无法归零 |
| 7 | ✅ 已修复（T1）：`!out_code` 判断移到函数最前并直接返回，不再"先判空、后解引用" | `platform/rtsp/zlm_push_server.cpp` | 传 `nullptr` 直接崩溃 |
| 8 | `MppDecoder` 回调前未分配 `FrameBuffer` 内存就 `memcpy` | `platform/decoder/rk_mpp_decoder.cpp:104-106` | `fb.data` 为空，必现段错误 |
| 9 | `MemManager::MemInit()` 中 `new MemBlock` 后未使用；`FreeMem()` 只删 `MemMeta`，不删 `FrameBuffer` | `common/MemManager/MemManager.cpp:22-34`、`40-54` | 每次初始化/退出均泄漏内存与对象 |
| 10 | `FrameBuffer` 无析构、可拷贝、`Allocate()` 后再 `BindMemory()` 会覆盖指针 | `common/frame_buffer.h:16-125` | 裸指针生命周期不可控，易双重释放/泄漏 |
| 11 | `MemMeta` 无析构，`set_ptr()` 覆盖同名指针时旧 `MemSlot` 泄漏 | `common/MemManager/MemMeta.h:46-79` | 长时间运行内存持续增长 |
| 12 | ✅ 已修复（`9678e42`）：`ViNode` / `PushNode` 的 `std::atomic<bool> running_` 未初始化（C++14 默认构造不写值） | `moudle_vi.cpp::ViNode`、`moudle_push.cpp::PushNode` | 内存脏值时 `start()` 里 `running_.exchange(true)` 直接返回，节点线程根本不起，整条链路静默堵死 |

### 4.2 P1：功能正确性 / 稳定性 / 性能

| # | 问题 | 位置 | 建议 |
| --- | --- | --- | --- |
| 1 | `pipeline_api` 未链接 `rtsp` / `common_api`，`zlm_rtsp_*` 符号无提供者 | `pipeline/CMakeLists.txt:26-29`、`moudle_push.h:6` | 显式 `target_link_libraries(pipeline_api PUBLIC common_api rtsp)` |
| 2 | `BUILD_RTSP_MODULE` 未定义，`rtsp` 不参与构建 | `platform/CMakeLists.txt:34` | 增加 `option(BUILD_RTSP_MODULE ...)` 并默认 ON（RK 平台） |
| 3 | V4L2 非阻塞 `DQBUF` 未用 `poll/select`，失败仅 `usleep(2ms)` | `v4l2_capture.cpp:163-169` | 使用 `poll()` + 超时，避免忙等与固定延迟 |
| 4 | V4L2 输入固定 YUYV，输出宽高固定 1920×1080，忽略池块实际容量 | `v4l2_capture.cpp:67-90`、`183-192` | 用 `VIDIOC_S_FMT` 返回的实际宽高；输出前校验 `capacity/data_size` |
| 5 | V4L2 `open()` 失败路径直接 return，未 `munmap`/`close` | `v4l2_capture.cpp:60-145` | RAII 或统一 `close()` 清理 |
| 6 | FFmpeg 采集构造了 `AVDictionary options` 与 `ifmt`，但 `avformat_open_input(..., nullptr, nullptr)` 未传入 | `ffmpeg_capture.cpp:15-55` | 传入 `&options`/`ifmt`，并用 `av_dict_free` 释放 |
| 7 | MPP 解码未处理 `info change`，也未把输出接回内存池 | `rk_mpp_decoder.cpp:82-90` | 实现 buffer 重分配；回调输出直接写入池块 `FrameBuffer` |
| 8 | x86 软编/软解与接口不一致，且使用已移除 API | `ffmpeg_encoder.h:16`、`ffmpeg_decoder.cpp:6,67-69` | 对齐 `CodingType`、`data[]/stride[]/fmt`，删除 `avcodec_register_all()` |
| 9 | `MemPool` 在无锁状态下读取 `free_list_.size()` | `MemPool.cpp:158`、`176` | 把 size 判断移入 `free_mtx_` |
| 10 | `MemMeta::data<T>()` 用 `void*` 强转 `MemSlot<T>*`，类型写错即 UB | `MemMeta.h:56-66` | 增加类型标签（`type_index`）或改用 `std::any`（C++17）/统一基类 |
| 11 | ✅ 已修复（`283bab5`）：推流参数硬编码——端口 8550、`live/ch0`、1920×1080、H.265 已改由 `config/push.json` 驱动；`gop` 仍未透传 | `moudle_push.cpp::set_data` / `init_zlm_push` | 已按通道下发；剩余项见 ROADMAP |
| 12 | ✅ 已修复（`283bab5`）：编码 PTS 用文件级全局 `frame_index_`，多实例/多通道共享 | `moudle_push.cpp::sendFrame` | 已改为每实例成员，步长按配置 fps |
| 13 | 首帧只打开编码器、不编码 | `moudle_push.cpp::run` | 打开后同帧继续编码，减少一帧延迟 |
| 14 | ZLMediaKit 发送加全局锁，且逐帧 `printf` + `fflush` | `zlm_push_server.cpp:145-158`、`moudle_push.cpp::sendFrame` | 按流加锁或无锁队列；日志分级并限流 |
| 15 | `mk_media_input_h264/265` 返回值为实际消费长度，封装层始终返回 `ZLM_ERR_OK` | `zlm_push_server.cpp:148-158` | 校验返回值，处理半包/失败 |
| 16 | `main_node.cpp` 在停止逻辑前 `while(1)` 永久阻塞 | `main_node.cpp:46-48` | 删除死循环，补充 SIGINT 优雅退出 |
| 17 | `main_testpool.cpp` 使用不存在的 `GlobalMemManager` 与旧 `FrameBuffer*` | `main_testpool.cpp:62,141-143` | 按当前 `MemBlock`/`MemPool` API 重写或删除 |
| 18 | 软编/软解未注册工厂（`create_decoder()` 只有声明） | `platform/include/decoder_interface.h:45` | 增加 `create_encoder/create_decoder` 实现与平台选择逻辑 |
| 19 | `ffmpeg_capture` 以固定 30fps 覆盖源帧率，`ViNode` 再按配置 fps 节流 | `moudle_vi.cpp::run` | 以源帧率为准，节流只做补偿，避免双重限速 |

### 4.3 P2：工程质量

- `config/`、`scripts/` 为空；`docs/` 已有架构/现状/路线图三份文档，但缺少独立的构建与排错文档。
- 日志混用 `printf`、`std::cout`、`std::cerr`，无时间戳/级别/模块名，难以定位板端问题。
- 无单元测试与 CI；`build/` 产物与源码同仓（虽被 `.gitignore` 忽略但本地残留旧缓存）。
- 命名不统一：`moudle_*`（拼写错误）、`ZlmServerStreamHandle` / `MppEncoder` 风格混用。
- 大量注释掉的配置代码（MPP CBR/VBR/QP、安全发包等）增加阅读成本，建议移到配置或文档。

## 5. 优化建议

### 5.1 架构收敛（最高优先级）

1. **在现有 `MemPool + MemBlock + MemQueue` 上补齐引用计数。** 保留 `common/MemManager` 单例池，把 `user_cnt` 升级为块级原子引用计数，使一个源可以安全扇出到编码、算法、录像多个下游。这样可以获得：
   - 块级 `refcnt`，天然支持一个源多个下游（编码、算法、录像）；
   - 池容量与水位可观测，避免所有通道争抢同一个全局 `free_list_`；
   - 类型化 slot 统一计算图像/结构体大小，新增格式只需注册。
2. **统一 include guard。** 所有头文件加模块前缀（如 `EVS_MEM_BLOCK_H`），目录名与命名空间统一（`evs::core`）。
3. **明确模块边界。** `platform` 只依赖抽象接口，不依赖 `pipeline`；`pipeline` 依赖 `platform_api + common_api`；`main` 只做装配。用 `target_link_libraries(... PUBLIC/PRIVATE)` 表达真实依赖，禁止依赖可执行文件层“补链”。
4. **统一 cmake 配置入口。** 提供 `cmake/options.cmake`，集中定义 `BUILD_PLATFORM_*`、`BUILD_RTSP`、`BUILD_TESTS`、`ENABLE_ASAN` 等开关；为 x86 提供 `install/x86_64` 或允许 `pkg-config` fallback。

### 5.2 内存与零拷贝

- **让 FrameBuffer 成为“视图 + 元数据”**：`data[3]` 指向池块 `MemBlock` 的 slot，禁止在 `FrameBuffer` 内 `new/delete`；由 `MemBlock` 的引用计数决定归还时机。
- **消除 MPP 编码前的 `memcpy`**：优先使用 `mpp_buffer` 导入外部 DMA-BUF（`mpp_buffer_import` / `MPP_BUFFER_TYPE_EXT_DMA`），或让采集直接写入 MPP 可用的 DMA 缓冲；至少应避免“池内存 → 新 MppBuffer”的整帧拷贝。
- **RGA 预处理**：V4L2/MPP 输出后直接用 RGA 做缩放/格式转换（NV12↔RGB、裁剪、旋转），替代 `sws_scale`，降低 CPU。
- **池容量与水位**：按 `帧率 × 最大处理延迟 × 下游分支数` 估算块数；提供 `acquired/free/dropped` 计数与高水位告警。
- **生命周期 RAII**：为 `MemBlock` 设计 `BlockRef`（类似 `shared_ptr` 的轻量句柄），在析构时自动 `release`；所有 `continue`/异常路径都不会漏归还。
- **零拷贝边界清晰**：跨线程只传 `MemBlock*`（带引用计数）；跨进程/网络再序列化；不要在回调里保存裸 `FrameBuffer*`。

### 5.3 流水线与线程

- **类型化队列**：`BlockQueue` 封装 `mutex + condition_variable + 容量 + 丢弃策略`；提供 `push/pop(timeout)/close()`，停止时 `close()` 唤醒所有等待者。
- **背压策略可配置**：`DROP_OLDEST`（实时预览）/`BLOCK`（录像）/`DROP_NEWEST`（告警），并在 `set_data` 中选择。
- **多下游引用计数**：`dispatch_to_children` 先对每个下游 `add_ref`，下游处理完各自 `release`；源节点最后释放自己的引用。
- **节点状态机**：`Created → Initialized → Running → Stopping → Stopped → Error`，`start/stop` 幂等，重复调用安全；析构前必须 `unlink_all`。
- **异常隔离**：节点 `run()` 外层 `try/catch`，单节点异常只标记该节点 `Error`，不拖垮进程；回调中禁止抛异常。
- **时间戳统一**：采集侧写入 `pts_us`（源时间基），编码侧只做换算；不要用“帧序号 × 33ms”假装时间戳，否则断流/重连后音画/播放器会异常。

### 5.4 平台层

- **采集**：V4L2 使用 `poll` + `VIDIOC_QUERYCAP/S_FMT/G_FMT/S_PARM`，把实际宽高/stride/格式写回 `FrameBuffer`；FFmpeg 传入 `AVDictionary`，支持 `rtsp_transport=tcp`、超时、重连、`av_seek_frame` 循环播放。
- **编码**：MPP 增加 `MPP_ENC_SET_HEADER_MODE`、EOS flush、关键帧请求、动态码率/帧率；把 CBR/VBR/FIXQP 做成配置项，而不是注释代码。
- **解码**：完成 `info change` 的 buffer 重分配；输出直接写池块；与编码器共用 `EncoderConfig/DecoderConfig`。
- **工厂**：补齐 `create_capture/create_encoder/create_decoder/create_processor`，按 `TARGET_PLATFORM` 或运行时能力选择实现；对外只暴露接口头。
- **x86 可调试**：用 FFmpeg 软编/软解 + 文件源跑通完整链路，作为 CI 与开发机回归基线；RK 专有代码用条件编译隔离。

### 5.5 推流与协议

- 修复 `zlm_rtsp_stop()` 的 key：统一 `StreamKey(port, app, stream)`，不要混入空 `bind_ip`。
- 每路流独立锁/队列，避免全局 `g_mtx` 串行化所有通道；`mk_media_input_*` 的返回值要检查。
- 缓存 SPS/PPS/VPS，在关键帧前补发；确保播放器中途接入可解码。
- 支持多通道：`port/app/stream` 由配置生成；停止时引用计数归零再 `mk_stop_all_server()`。
- 增加断流检测与自动重连（采集侧）与推流侧超时清理。

### 5.6 配置、日志与可观测性

- **配置**：`config/channels.json` 描述通道（源地址、分辨率、帧率、编码类型、码率、RTSP 路径、启停策略），启动时加载并校验；支持 `SIGHUP` 热加载。
- **日志**：统一 `Logger`（级别、时间、线程、模块、通道），支持输出到文件与环形缓冲；高频日志限流。
- **指标**：每通道采集帧数、编码帧数、发送字节、队列深度、丢帧数、错误码；通过 HTTP `/metrics` 或共享内存暴露。
- **健康检查/看门狗**：节点心跳超时自动重启；进程崩溃写 core/回溯。
- **优雅退出**：`SIGINT/SIGTERM` → 停止采集 → flush 编码器 → 停止推流 → 归还所有块 → 退出。

### 5.7 工程质量

- 引入 CTest + GoogleTest（或 Catch2），优先覆盖：内存池生命周期、队列满/超时、引用计数、ZLM key 逻辑、配置解析。
- CI：x86 构建 + 单测 + ASan/UBSan；RK 交叉构建只编译不运行；产物归档。
- 静态检查：`clang-tidy`、`cppcheck`；编译开启 `-Wall -Wextra -Werror`（分阶段）。
- 压测：`ffmpeg -f lavfi -i testsrc` 模拟源，4 路 1080p30 连续 72 小时，记录内存/RSS/丢帧/延迟。

## 6. 测试与验证现状

- 现有测试均为 `main()` 手工验证：编码、推流、节点、内存池各有独立测试程序。
- 没有 CTest/GoogleTest，也没有 CI；`main_testpool.cpp` 仍使用旧 API，未参与编译。
- x86 侧当前会在 CMake 配置阶段失败，主要缺 `install/x86_64` 依赖；RK 侧 `testnode` 可演示 Vi→Push 单链路，但需先修 P0。
- 建议顺序：先修构建与 P0，再补内存池/队列/RTSP key 单测，最后接 ASan/UBSan 与 4 路压测；具体排期见 [ROADMAP.md](ROADMAP.md)。

[返回 README](../README.md)
