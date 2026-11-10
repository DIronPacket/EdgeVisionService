# 多链路配置与服务内核设计（node.json + 进程级资源上提）

> 状态：**设计提案，尚未实现**（本文不含代码改动）
> 编写日期：2026-11-08 · 适用分支：`rk3588` · 基线提交：`7a2055b` + 工作树（含 `moudle_vpss`、`common/preprocess`）
> 相关：[ARCHITECTURE.md](ARCHITECTURE.md)（现状架构）· [STATUS.md](STATUS.md)（P0/P1 清单）· [ROADMAP.md](ROADMAP.md)（WBS B「媒体框架收敛」）· [HANDOFF.md](HANDOFF.md)（红线与构建）

---

## 0. 一页结论（TL;DR）

1. **你的判断是对的，但不止媒体服务器一项。** 判据是三条：**① 进程内唯一 ② 跨通道共享 ③ 生命周期长于链路**。满足任意一条就不该由节点持有。按此筛出 **16 项**需上提的职责（见 §3），媒体服务器只是其中最典型的一项。
2. **现状已经很危险**：ZLMediaKit 的 `mk_env_init2`、`g_server_map`（按端口做引用计数）、`mk_media` 注册表、`mk_stop_all_server()` **全都在 `zlm_push_server.cpp` 里是进程级静态态**，但生命周期却由 `PushNode::start()/stop()` 驱动（`moudle_push.cpp:30`、`52-56`）。也就是「进程级资源 + 节点级生命周期」的错配，加上 P0-6 的 key 不一致，导致 **停一路会泄漏一路；而修好后 `mk_stop_all_server()` 又会停掉全进程所有路** —— 两个方向都是错的，必须由 MediaServer 统一管引用计数。
3. **推荐路线：方案 A 落地 + 按方案 B 的形状设计接口**（§6）。方案 A（轻量装配层：`NodeConfigManager` + `MediaServer` + `NodeFactory` + `PipelineBuilder`）**3–5 人日**可跑通双链路；接口留好依赖注入的缝，稳定后平滑演进到方案 B（ServiceKernel 注入，节点不再摸单例），避免二次返工。方案 C（插件化 .so）/ 方案 D（多进程）当前不建议，但 D 是 4 路以上 + AI 场景的正确演进方向。
4. **前置阻塞：P0-6 / P0-7 必须先修**（`zlm_rtsp_stop` 的 key 与注册不一致、`zlm_rtsp_onekey_start` 入参校验顺序错）。MediaServer 上提之后，这两个问题会从「泄漏」升级成「多路互踩」，必须先清掉。
5. 迁移期 `config/push.json` 作为 v0 单链路格式被 `NodeConfigManager` 自动适配；**M1 验收后删除它（D8）**，此后只保留 `config/node.json`（v1，表达多链路与每节点独立配置）。

---

## 1. 需求

### 1.1 功能需求（来自你的描述）

| ID | 需求 | 说明 |
| --- | --- | --- |
| F1 | 配置中心化 | `push.json` 升级为 `node.json`，一份文件描述**所有链路**，每个节点有自己的配置段 |
| F2 | 多链路 | `main` 从配置读到 N 条链路，自动 create / set_data / link / start / stop |
| F3 | 全局配置对象 | 专门写一个全局对象管理该 JSON（加载、校验、查询、默认值合并），`main` 只从它取"链路信息" |
| F4 | 进程级资源独立 | 媒体服务器（RTSP server）等进程级资源不能塞进节点配置，必须由全局对象持有（你的补充） |
| F5 | 类工厂装配 | 配置里写 `type`，装配层按类型建节点，`main` 不出现 `if (type == "vi")` 长链 |

### 1.2 非功能需求（从现有红线与工业约束推导）

| ID | 需求 | 来源 |
| --- | --- | --- |
| N1 | 分层不反向 | `common` 不得依赖 `pipeline`/`platform`；配置对象放 `common`，工厂与装配放 `pipeline` | STATUS §5.1-3 |
| N2 | C++14 | 不用 `std::any` / `if constexpr` / 结构化绑定 | HANDOFF §2.1 |
| N3 | 失败不拖垮全局 | 单节点配置非法 → 该节点告警并降级，不让进程退出；单链路建链失败 → 其他链路照常跑 | 现有 `set_data` 风格 |
| N4 | 队列名唯一 | `MemQueue::InitQueue` 按**队列名**全局注册，重名会让两路节点共用一条队列（数据串扰） | `MemPool.h` / MEMBER 表 HANDOFF §1.7 |
| N5 | 可回滚 | 新装配路径与旧路径可切换（命令行/配置开关），出问题能一分钟退回单链路 | 现场交付惯例 |
| N6 | 兼容 | 迁移期 `config/push.json` 仍可加载（文档、脚本、习惯都指向它）；**D8 决策：M1 验收通过后删除它**，长期只保留 `node.json` | README / 各节点注释 |

### 1.3 明确不做（本次范围外）

- 不做节点热插拔/动态库加载（方案 C）。
- 不做多进程拆分（方案 D），仅在 §6 给出演进条件。
- 不改节点像素处理逻辑（`process_frame()` 等仍是待填 TODO）。
- 不做配置热加载（用户 2026-11-08 确认：暂不需要，见 §1.4-D5）。
- 不做 RTSP 鉴权 / rtsps（用户确认暂不需要，见 §1.4-D6）。
- 不做主/子双码流输出（用户确认当前不需要，架构留口子，见 §1.4-D2 与 §11）。

### 1.4 已确认决策（2026-11-08 与用户逐项确认）

| ID | 问题 | 决策 | 对方案的影响 |
| --- | --- | --- | --- |
| D1 | 目标通道数 / 帧率 | **4 路 × 30fps**。硬件摄像头只有 1 路，另 3 路用**本地 mp4 文件**当源 | 文件源是主要新增能力：**必须支持循环播放 + 分辨率适配**（当前 `read_frame` 到 EOF 直接返回 false，`moudle_vi.cpp:141-155` 会 1ms 空转、不再出帧）。池/队列/CPU 按 4 路重算（§11） |
| D2 | 是否需要"一路源 → 主码流 + 子码流" | **当前不需要**（解释见 §10.1）。架构上留口子：`pipelines[]` 里可以放多个 `push` 节点共用一路源 | 不触发 P0-5 引用计数的紧急需求 → P0-5 保持 M2（若将来做子码流，必须先修 P0-5 + VPSS 缩放） |
| D3 | RTSP 端口策略 | **所有通道共用一个端口**，仅 `stream` 不同（`rtsp://ip:8550/live/ch0..ch3`） | `MediaServer` 单 server + 4 个 stream；`port` 上提到 `global.media.rtsp`，节点不再各自配端口 |
| D4 | 采集设备 | 有硬件就独立；**当前 = 1 路 `/dev/video0` + 3 路文件** | `DeviceRegistry` 降级为 M2 的"防重复打开"；M1 用 `src` 字段区分 `dev:/dev/video0` 与 `file:/data/xx.mp4` |
| D5 | 配置热加载 | **暂时不用**（确认理解正确：指不重启进程改配置生效，见 §10.2） | `NodeConfigManager::load()` 仍设计成可重复调用（快照替换），将来加 SIGHUP 不用改结构 |
| D6 | RTSP 鉴权 / rtsps | **暂时不用**（两者区别见 §10.3） | `MediaServer` 预留 `auth` 字段位置，M1 不实现 |
| D7 | 单进程 or 多进程 | **保持单进程**；将来可能拆出通信/升级业务进程 | 方案 D 保持 M3 评估；进程内分层要干净（为将来拆进程留边界，见 §5.1） |
| D8 | `config/push.json` 去留 | **v1 迁移完成后删除**（不再长期维护两套格式） | M1 保留 v0 适配器用于回归对照；**M1 验收通过后删除 `config/push.json` 与适配代码**（T8 内），文档全量改指 `node.json` |
| D9 | 内存池归属 | **A = 共享池 + 每路配额**（2026-11-08 确认）：全进程一个池，块统一 1080p，`channel → inflight` 配额防一路拖垮全局；**不按链路分池**，不做"块→链路归属 map"；A+（按 profile 分组池）暂不实施 | 详见 §11.6（含现状事实、三方案对比、内存账、业界对照）；**T10a 由 0.5 → 1.5 人日**，验收增加"配额隔离"与"`free + Σinflight == total`"两项 |


---

## 2. 现状事实盘点（决定"哪些必须上提"的证据）

| # | 资源 / 职责 | 现在归属 | 代码位置 | 问题 |
| --- | --- | --- | --- | --- |
| 1 | ZLM 运行环境 `mk_env_init2` | `zlm_push_server.cpp` 内的静态 `g_env_ok`，**第一次 `onekey_start` 时懒初始化** | `zlm_push_server.cpp:73-77` | 进程级初始化藏在"起一路流"的 API 里；日志级别/线程数是硬编码 `mk_env_init2(4, 2, LOG_CONSOLE, ...)`；无显式关闭点 |
| 2 | RTSP server（监听端口） | 静态 `g_server_map`，按端口引用计数 | `zlm_push_server.cpp:79-101` | 生命周期由"第一个/最后一个 PushNode"决定；`bind_ip` 从未赋值却参与 key 拼接 |
| 3 | 流（`mk_media`） | **`PushNode::m_handle`** | `moudle_push.h:53`、`moudle_push.cpp:215-227` | 节点持有进程级资源 → 节点析构顺序决定服务是否泄漏 |
| 4 | 停一路 / 停全部 | `zlm_rtsp_stop()` → 引用计数归零时 `mk_stop_all_server()` | `zlm_push_server.cpp:161-187`（`:184`） | `mk_stop_all_server()` 是**停全进程所有 RTSP 服务**；多路场景下停一路会打断其他路 |
| 5 | 停止 key 不一致（P0-6） | `zlm_rtsp_stop` 用 `bind_ip + port`，注册时只用 `port` | `zlm_push_server.cpp:104` vs `:167`、`:181` | 找不到流直接 return → `mk_media` 不释放、`ref_cnt` 不归零 → 泄漏 |
| 6 | 入参校验顺序（P0-7） | 既判断 `!out_code` 又写 `*out_code` | `zlm_push_server.cpp:66-70` | 传 `nullptr` 必崩 |
| 7 | 编码器实例 | `PushNode::enc`（每节点一个 `MppEncoder`） | `moudle_push.h:56` | 归属正确（每路一个），但 **MPP 实例总数需要全局配额**，否则 4 路以上直接失败 |
| 8 | 内存池 | `MemManager` 单例 + `MemPool` 单例，块数/规格写死在构造函数 | `MemManager.cpp:5-8`（`ImgConfig{1920,1080,NV12,"nv12"}`）、`:13-48` | 多路不同分辨率无法表达；~~`FreeMem()` 不删 `FrameBuffer`（P0-9）→ 反复建链路必泄漏~~ **P0-9 已修（2026-11-10，`FreeMem()` 逐槽 `Release()` + `delete`，TESTLOG §5.13）**；仍剩 `capacity = w*h*3` 的 2× 过分配（STATUS §4.1 第 10 条） |
| 9 | 节点耗时 | 每块的 `MemMeta` 挂 `NodeProfiler` | `MemManager.cpp:35` | 一条链路一份报表是合理的；但**多路共用一个池时，块会被多路串用**（取决于 P0-5 引用计数），报表会互相污染 |
| 10 | 采集设备 | `ViNode::run()` 里硬编码 `/dev/video0:640:480:NV12:30` + `CAPTURE_V4L2` | `moudle_vi.cpp:104-106` | 两路必然抢同一设备（V4L2 独占 open 会 EBUSY）；且 P0-2 明确指出 x86 下工厂返回 `nullptr` |
| 11 | 采集参数来源 | `ViNode::set_data` **只认 `"push"` 段**，从里面读 `width/height/fps` | `moudle_vi.cpp:61-67`、`:73-75` | vi 与 push 靠"读同一段"隐式耦合；多链路必须让 vi 有自己的段 |
| 12 | 节点配置段 | push 认 `"push"`、print 认 `"print"`、vpss 认 `"vpss"` | `moudle_push.cpp:127-131`、`moudle_print.cpp:55-60`、`moudle_vpss.cpp:123-128` | 同一段被多路共享时无法"每路不同端口/码率" |
| 13 | 装配逻辑 | 手写 3 个节点 + 3 行 `set_data` + 2 行 `link_to` + 3 行 `start` | `main_node.cpp:48-85` | 加一条链路要改代码；末端 `while(1)` 死循环（P1-16）导致后面 stop 永不执行 |
| 14 | 配置加载 | `main_node.cpp::load_push_config` 查 3 个候选路径 | `main_node.cpp:19-39` | 只读一份；无校验（写错字段静默用默认值） |
| 15 | 日志 | `printf` / `std::cout` / `std::cerr` 混用，无通道前缀 | 全局 | 多路日志无法区分来源，现场排障基本失效（STATUS §4.3） |
| 16 | 信号与退出 | 无 SIGINT 处理 | `main_node.cpp:94-96` | Ctrl+C 直接杀进程，`FreeMem()`/`zlm stop` 都不执行 |
| 17 | 全局锁性能 | 每次送流都抢一把全局 `g_mtx` | `zlm_push_server.cpp:145`（P1-14） | 多路并发送流会串行化；2 路可接受，4 路要先改 |
| 18 | 扇出安全 | `push_input_pipe()` 本身不加计数，但下游 `MemQueue::push` → `MemPool::pushBlock` 会 `addUser` | `MemPool.cpp:148-152` / `:199-213`（P0-5） | ~~会读到脏数据~~ **P0-5 于 2026-11-10 核实为非缺陷**（计数完整）；真实缺口是**扇出拓扑不可达**（`PipelineBuilder` 只连 `i→i+1`）+ 下游就地改写同一块的语义约束（STATUS §4.2 第 24 条） |

> 结论（证据）：媒体服务器确实不该在节点里；但**内存池规格、设备独占、日志、时钟、MPP 配额**同样属于「节点不该持有」的范畴。

---

## 3. 判定准则：什么该留在节点，什么必须上提

### 3.1 三条判据

| 判据 | 含义 | 命中后的归属 |
| --- | --- | --- |
| **C1 进程内唯一** | 操作系统/第三方库层面只允许存在一份（端口监听、`mk_env`、日志文件句柄、DMA heap） | 进程级单例（Kernel 持有） |
| **C2 跨通道共享** | 多路要用同一个实例，或必须跨路协调（RTSP 端口、磁盘水位、AI 推理上下文、设备分配） | 进程级单例（Kernel 持有） |
| **C3 生命周期长于链路** | 链路重建（改配置/出错重启单路）时不该被销毁（server、池、日志、指标） | 进程级单例（Kernel 持有） |

反过来说，**留在节点里的**必须同时满足：只服务本通道、可随链路生灭、不占用全局命名空间。典型：编码器实例、节点线程、节点自己的解析后的配置、节点耗时打点、每块的输出帧缓存。

### 3.2 分类结果（16 项上提 + 明确保留项）

| 类别 | 项 | 归属 | 优先级 |
| --- | --- | --- | --- |
| 媒体 | ZLM 运行环境（`mk_env_init2`、日志回调、线程数） | `MediaServer` | P0（M1） |
| 媒体 | RTSP 监听端口 / server 引用计数 | `MediaServer` | P0（M1） |
| 媒体 | 流注册表（`app/stream` → `mk_media`）与句柄生命周期 | `MediaServer`（节点只持 `shared_ptr<Stream>`） | P0（M1） |
| 媒体 | 播放端连接数/鉴权/白名单 | `MediaServer` | P2 |
| 内存 | 池块数、图像规格（宽高/格式/名字） | `global.pool` + `MemManager` | P0（M1 只做单规格，M2 多规格） |
| 内存 | 池水位/丢帧/取块超时统计 | `MetricsRegistry` | P1（M2） |
| 资源 | MPP 编码器实例配额（每路 1 个，总数上限） | `ResourceBudget` | P1（M2） |
| 资源 | RGA 通道、DMA heap 可用性 | `ResourceBudget` | P2 |
| 设备 | 采集设备独占（`/dev/videoX` 谁在用） | `DeviceRegistry` | P1（M2） |
| 运行时 | 日志（级别/输出/通道前缀/限流） | `Logger` | P0（M1 最小版） |
| 运行时 | 时钟基准（PTS 单调时钟、各路起始偏移） | `Clock` | P1（M2） |
| 运行时 | 信号（SIGINT/SIGTERM）与有序退出 | `main` / `Supervisor` | P0（M1） |
| 运行时 | 健康检查、节点心跳、单路崩溃重启 | `Supervisor` | P2 |
| 运行时 | 指标（每路 fps/丢帧/耗时窗口） | `MetricsRegistry` | P1（M2） |
| 存储 | 录像目录、磁盘水位、文件命名 | `StorageManager` | P2（录像落地时） |
| 配置 | 加载/校验/默认值/引用展开/v0 兼容 | `NodeConfigManager` | P0（M1） |
| **保留在节点** | 编码器实例、节点线程、节点配置快照、耗时打点、per-block 输出帧 | 节点 | — |

> 一句话记忆：**节点是"消费者/生产者"，不是"所有者"**。节点只应持有"我这一路、我这个实例"的东西。

---

## 4. 行业方案调研（商业 / 工业 / 企业级）

| 体系 | 配置形态 | 进程级资源如何管理 | 对本项目的可借鉴点 |
| --- | --- | --- | --- |
| **海康 / 大华 NVR-DVR** | 设备表 + 通道表 + 流媒体参数表（数据库，非单文件 JSON） | 设备管理（Device）、通道（Channel）、流媒体分发服务分离；一台设备可挂多通道；**流媒体服务进程集中管理所有通道的对外分发**，多客户端订阅同一通道不重复编码 | 设备 ≠ 通道；媒体服务是进程级；每通道独立编码参数 |
| **GStreamer + GstRTSPServer** | 应用里用 `gst_parse_launch` 描述 pipeline（文本 DSL） | 应用持有**一个** `GstRTSPServer`，每路 pipeline 用 `GstRTSPMediaFactory` 注册成 `mount-point`；元素从 `GstElementFactory` 按类型创建 | ① `type` → 工厂建节点；② 一个 server 多个 mount point（= 我们的 `app/stream`） |
| **ZLMediaKit / MediaMTX** | 单文件配置文件（`config.ini` / `mediamtx.yml`），里面是 `paths` 数组 | 一个进程一个 server；`app/stream` 是 path；每 path 可配 `enable`、编码参数、鉴权 | **MediaMTX 的 path 配置就是"节点配置"的扁平版**；我们已经有 ZLM 封装，缺的是把 path 注册表提到进程级 |
| **Live555** | 代码里注册 `ServerMediaSession` | `RTSPServer` 单实例 + 每路一个 `ServerMediaSession` | 与 ZLM 封装完全同构，验证"server 单例 + 多 session"是标准解法 |
| **NVIDIA DeepStream** | 每源一个 `nvurisrcbin`，共享 `nvdsosd` / sink；配置文件 `config_*.txt` 分组 | `nvstreammux` 做多源汇聚；meta 池化；RTSP sink 可多源共享 | ① 源/处理/输出三段分离；② 共享 sink（多路共用一个 server） |
| **AWS Greengrass / Azure IoT Edge** | **声明式部署清单**（模块列表 + 配置 + 依赖 + 重启策略） | Agent（Supervisor）读清单 → 拉起模块 → 健康检查 → 失败重启；模块彼此隔离 | ① `node.json` = 我们的部署清单；② Supervisor 负责生命周期而不是节点自己 |
| **GB28181 视频平台** | 信令（SIP）+ 媒体分离 | 通道注册到平台；媒体由集中式流媒体服务转封装/分发 | 信令与媒体解耦；将来接平台时只需新增一个"注册节点"，媒体服务不动 |

**归纳出的通用四层结构**（本文方案直接对应）：

```text
① 声明式配置层   一份文件描述：全局资源 + 通道列表 + 每通道的节点链   ← node.json
② 服务内核层     进程级单例：媒体服务 / 池 / 日志 / 指标 / 设备分配 / 时钟   ← ServiceKernel
③ 装配层         按配置建节点、连线、启停（工厂 + Builder/Supervisor）   ← NodeFactory + PipelineBuilder
④ 通道实例层     一条链路 = 一组节点实例（可独占设备与编码器）        ← PipelineSet
```

四层中，**本项目当前只有第 ④ 层是完整的，①②③ 全部缺失或错位**（配置是单文件但只有一段；进程级资源被塞在节点里；装配写死在 `main`）。这就是本次改动的本质。

---

## 5. 目标架构（推荐方案 A+）

### 5.1 分层与数据流

```text
config/node.json
      │  load() + 校验 + 默认值合并 + 引用展开
      ▼
┌──────────────────── common 层（无外部依赖，只依赖 json / MemManager）────────────────────┐
│  NodeConfigManager (单例)   global 段 / pipelines[] / 每节点配置段（含 v0 push.json 适配）│
└────────────────────────────────────────────────────────────────────────────────────────┘
      │  const GlobalDesc& / const vector<PipelineDesc>&
      ▼
┌──────────────────── pipeline 层（依赖 common + platform）──────────────────────────────┐
│  NodeFactory        type → shared_ptr<PipelineNode>（"vi"/"vpss"/"push"/"print"）       │
│  PipelineBuilder    for each pipeline: create → set_data → link → PipelineSet           │
│  PipelineSet        name / channel / nodes[] + start_all(逆序) / stop_all(正序)          │
└────────────────────────────────────────────────────────────────────────────────────────┘
      │  节点 start() 时取"本通道需要的进程级服务"（M1 走单例，M3 走注入）
      ▼
┌──────────────────── platform 层（进程级资源的所有者）──────────────────────────────────┐
│  MediaServer (单例)  mk_env_init2 一次 + RTSP 端口引用计数 + 流注册表 + 全局关闭         │
│                     open_stream(app/stream/codec/w/h/fps) → shared_ptr<Stream>          │
│  （M2）DeviceRegistry / ResourceBudget(MPP 配额) / Clock                                 │
└────────────────────────────────────────────────────────────────────────────────────────┘
      │
      ▼
   zlm_push_server.cpp（原有 C 封装，**不改**，由 MediaServer 内部调用）
```

**依赖方向检查（N1）**：`common` ← `pipeline` → `platform`。`MediaServer` 必须放 `platform`：它的实现要 `#include <ZLMediaKit/mk_media.h>`；`pipeline/moudle_push` 只 include `media_server.h`（纯 C++ 接口），不再直接 include `zlm_push_server.h`。

> 反例警告：**不要**把 `MediaServer` 放 `common`。否则 `common_api` 要链接 `rtsp`，所有 `target_link_libraries(* common_api)` 的目标（含 `platform`）都会被 ZLM 的依赖链拖进来，形成环。

### 5.2 新增模块清单

| 模块 | 路径 | 职责 | 依赖 | 里程碑 |
| --- | --- | --- | --- | --- |
| `NodeConfigManager` | `src/common/configmgr/{node_config.h,node_config_manager.h,node_config_manager.cpp}` | 读 `node.json`；校验（缺 `type`/重名/端口冲突/设备冲突）；默认值合并；把节点配置包成 `{"<seg>":cfg}` 后交给 `set_data`；v0 `push.json` 自动适配 | `json.hpp` | M1 |
| `MediaServer` | `src/platform/media_server/{media_server.h,media_server.cpp}` | `mk_env_init2` 只做一次；RTSP 端口引用计数；`open_stream`/关闭；`Stream` RAII；`shutdown()` | `zlm_push_server.h` | M1 |
| `NodeFactory` | `src/pipeline/node_factory.{h,cpp}` | `type` → 节点实例（注册表 `map<string, Creator>`） | 各 `moudle_*` | M1 |
| `PipelineBuilder` | `src/pipeline/pipeline_builder.{h,cpp}` | 遍历配置建链、连线、启停顺序、失败清理 | `NodeFactory` + `NodeConfigManager` | M1 |
| `Logger` | `src/common/log/{logger.h,logger.cpp}` | 级别 + 通道前缀 + 限流 | 无 | M1 最小 / M2 完善 |
| `DeviceRegistry` | `src/platform/device/device_registry.{h,cpp}` | `/dev/videoX` 独占登记，冲突即报错 | 无 | M2 |
| `ResourceBudget` | `src/platform/resource/resource_budget.{h,cpp}` | MPP 实例数、RGA 通道配额 | 无 | M2 |
| `MetricsRegistry` | `src/common/metrics/*` | 每路 fps/丢帧/耗时窗口/池水位 | 无 | M2 |
| `StorageManager` | `src/platform/storage/*` | 录像目录、磁盘水位 | 无 | P2（录像落地时） |

### 5.3 关键接口草案（C++14）

```cpp
// ---------- src/common/configmgr/node_config.h ----------
struct NodeDesc {
  std::string type;         // vi | vpss | push | print
  std::string name;         // 节点名（日志 / 队列名前缀，必须全局唯一）
  int         channel = 0;  // 传给 create(channel_id, name)
  std::string config_json;  // 已包装成 {"push":{...}} / {"vi":{...}} 的字符串
};

struct PipelineDesc {
  std::string name;             // "ch0"
  int         channel = 0;
  bool        enable = true;
  std::vector<NodeDesc> nodes;  // 顺序即连线顺序
};

struct MediaDesc  { int port = 8550; std::string bind_ip = "0.0.0.0"; std::string app = "live"; };
struct LogDesc    { std::string level = "info"; std::string file; };
struct GlobalDesc {
  int  pool_blocks = 50;
  std::vector<ImgConfig> images;   // {"name":"nv12","width":1920,"height":1080,"fmt":"nv12"}
  MediaDesc media;
  LogDesc   log;
};

// ---------- src/common/configmgr/node_config_manager.h ----------
class NodeConfigManager {
public:
  static NodeConfigManager &getInstance();

  bool load(const std::string &path);   // 解析 + 校验 + 合并默认值（启动阶段调用一次）
  bool load_with_fallback();            // config/node.json → config/push.json（v0）
  const GlobalDesc &global() const;
  const std::vector<PipelineDesc> &pipelines() const;
  const PipelineDesc *find(const std::string &name) const;
  std::string last_error() const;       // 校验失败原因（带字段路径，如 pipelines[1].nodes[2].type）

private:
  // 快照发布：C++14 可用 std::atomic_load / std::atomic_store(std::shared_ptr<T>*)
  std::shared_ptr<const Snapshot> snap_;
  mutable std::mutex mtx_;
};
```

```cpp
// ---------- src/platform/media_server/media_server.h ----------
class MediaServer {
public:
  static MediaServer &getInstance();

  bool start(const MediaDesc &cfg);  // mk_env_init2 只做一次 + 预检端口（不建流）
  void shutdown();                   // 关所有流 + 关所有 server（仅 main 末尾调用）

  class Stream {                     // RAII：析构自动注销 mk_media
  public:
    ~Stream();
    bool send(int codec, const uint8_t *data, int len, uint64_t dts, uint64_t pts);
    const std::string &key() const;  // "live/ch0"
  };
  // 同一 app/stream 重复打开 → 返回已有流（引用计数 +1），不再报 ZLM_ERR_STREAM_EXIST
  std::shared_ptr<Stream> open_stream(const StreamDesc &d);
  int  stream_count() const;
  bool healthy() const;
};
```

```cpp
// ---------- src/pipeline/pipeline_builder.h ----------
struct PipelineSet {
  std::string name;
  int channel;
  std::vector<std::shared_ptr<PipelineNode>> nodes;  // 顺序 = 链路顺序
};

class PipelineBuilder {
public:
  explicit PipelineBuilder(const NodeConfigManager &cfg);
  bool build_all(std::vector<PipelineSet> &out);  // 单链失败 → 清理该链，其余照跑（N3）
  bool build_one(const PipelineDesc &d, PipelineSet &out);

  static bool start_all(std::vector<PipelineSet> &sets);  // 逆序：终点先起（同现状 main_node）
  static void stop_all(std::vector<PipelineSet> &sets);   // 正序：源头先停
};
```

```cpp
// ---------- src/pipeline/node_factory.h ----------
typedef std::shared_ptr<PipelineNode> (*NodeCreator)();
class NodeFactory {
public:
  static NodeFactory &getInstance();
  bool register_type(const std::string &type, NodeCreator c);     // M1 在各 moudle 的 cpp 里静态注册
  std::shared_ptr<PipelineNode> create(const std::string &type);  // 未知类型 → nullptr + 告警
};
```

**M1 与 M3 的差别只有"怎么拿到单例"**：M1 节点内部继续 `MediaServer::getInstance()`；M3 引入 `ServiceKernel` 统一持有并在 `set_context()` 里注入。因为接口形状按 B 设计，M3 不需要重写业务逻辑。

### 5.4 `config/node.json`（v1 schema）

```json
{
  "version": 1,
  "_comment": ["v1：全局段 + 多链路。端口统一在 global.media.rtsp，各通道只区分 stream。",
               "ch0 = 硬件摄像头；ch1..ch3 = 本地 mp4 循环播放（当前条件：只有 1 路摄像头）"],

  "global": {
    "log":   { "level": "info", "file": "" },
    "pool":  { "blocks": 64,
               "images": [ { "name": "nv12", "width": 1920, "height": 1080, "fmt": "nv12" } ] },
    "media": { "rtsp": { "port": 8550, "bind_ip": "0.0.0.0", "app": "live" } }
  },

  "pipelines": [
    {
      "name": "ch0", "channel": 0, "enable": true,
      "nodes": [
        { "type": "vi",    "name": "vi",    "config": { "src": "dev:/dev/video0", "width": 1920, "height": 1080, "fps": 30 } },
        { "type": "vpss",  "name": "vpss",  "config": { "enable": false, "dst_fmt": "", "out_slot": "vpss_out" } },
        { "type": "push",  "name": "rtsp",  "config": { "stream": "ch0", "codec": "h265", "bitrate_kbps": 4096, "fps": 30 } },
        { "type": "print", "name": "print", "config": { "interval": 30 } }
      ]
    },
    {
      "name": "ch1", "channel": 1, "enable": true,
      "nodes": [
        { "type": "vi",    "name": "vi",    "config": { "src": "file:/data/sample1.mp4", "loop": true, "fps": 30 } },
        { "type": "push",  "name": "rtsp",  "config": { "stream": "ch1", "codec": "h265", "bitrate_kbps": 3072, "fps": 30 } },
        { "type": "print", "name": "print", "config": { "interval": 30 } }
      ]
    },
    {
      "name": "ch2", "channel": 2, "enable": true,
      "nodes": [
        { "type": "vi",    "name": "vi",    "config": { "src": "file:/data/sample2.mp4", "loop": true, "fps": 30 } },
        { "type": "push",  "name": "rtsp",  "config": { "stream": "ch2", "codec": "h265", "bitrate_kbps": 3072, "fps": 30 } },
        { "type": "print", "name": "print", "config": { "interval": 30 } }
      ]
    },
    {
      "name": "ch3", "channel": 3, "enable": true,
      "nodes": [
        { "type": "vi",    "name": "vi",    "config": { "src": "file:/data/sample3.mp4", "loop": true, "fps": 30 } },
        { "type": "push",  "name": "rtsp",  "config": { "stream": "ch3", "codec": "h265", "bitrate_kbps": 3072, "fps": 30 } },
        { "type": "print", "name": "print", "config": { "interval": 30 } }
      ]
    }
  ]
}
```

播放地址（共用一个端口，只有 stream 不同）：`rtsp://<板端IP>:8550/live/ch0` ~ `ch3`。


**schema 规则（v1）**

| 规则 | 说明 |
| --- | --- |
| `pipelines[].nodes[]` 顺序即连线顺序 | builder 依次 `nodes[i]->link_to(nodes[i+1])`，末端节点负责回收块 |
| `type` 必填 | 未知 type → 该链路构建失败并明确报错（不静默跳过） |
| `name` 可省略，默认 = `type` | **实际队列名 = `name + channel`**（各节点 `create()` 里拼接），builder 需校验拼接后全局唯一，冲突即报错（N4） |
| `channel` 可省略，默认 = 该 pipeline 在数组中的下标 | 多路必须不同，否则队列名/日志前缀撞车 |
| 节点 `config` 内省略的字段 = 继承 global | 例如 push 不写 `port` 就用 `global.media.rtsp.port`，本节点写了优先 |
| 引用语法（可选） | `"${global.media.rtsp.port}"`，由 ConfigManager 展开；解析不出来就报错，不静默 |
| `enable=false` 的 pipeline | 不建链、不占资源；节点级 `enable` 语义仍由各节点解释（如 `push.enable=false` 只表示不推流） |
| `vi.src` 前缀 | `dev:/dev/video0`（V4L2/ffmpeg 设备）、`file:/path/x.mp4`（本地文件）、`rtsp://…`/`rtmp://…`/`http://…`（网络流）；前缀决定用哪个采集实现，避免靠"字符串里有没有 /dev"猜 |
| `vi.loop` | 仅文件源有意义：`true` = 播完从头开始（**当前代码不支持，需新增**）；`false` = 播完该链停止并告警 |
| `vi.width/height` | **注意**：`FfmpegCapture` 目前**不缩放**（`ffmpeg_capture.cpp:161-163` 源尺寸→目标尺寸同名），文件源输出的就是文件本身分辨率；`V4l2Capture` 支持 `set_output_size()`。要做文件源缩放到 `width/height`，必须在 `transfer_frame()` 里按目标尺寸建 sws（新增任务 T7b） |
| `push.port` | 不再逐节点配置：端口只看 `global.media.rtsp.port`（D3 决策），节点里写 `port` 会被忽略并告警 |


**给 `set_data` 的字符串长什么样**：builder 把 `{"type":"push","config":{...}}` 包成 `{"push": {...}}`（段名映射 `vi→"vi"`、`vpss→"vpss"`、`push→"push"`、`print→"print"`）。这样**所有节点的 `set_data` 一行都不用改**——它们本来就在找自己的段。

唯一要动的节点是 `ViNode`：它现在只认 `"push"` 段（`moudle_vi.cpp:61-67`）。改动是"优先读 `vi` 段，没有则退回 `push` 段"，向后兼容，约 6 行。

### 5.5 装配时序（`main_node.cpp` 变成什么样）

```cpp
int main(int argc, char **argv) {
  // 1. 全局对象：配置（不碰任何节点）
  NodeConfigManager &cfg = NodeConfigManager::getInstance();
  if (!cfg.load_with_fallback()) { /* 打印 last_error() 后退出 */ }

  // 2. 进程级资源：日志 / 内存池 / 媒体服务（一次性，与链路数量无关）
  Logger::getInstance().init(cfg.global().log);
  MemManager::getInstance().MemInit(cfg.global().pool_blocks);   // M2 起带 images
  if (!MediaServer::getInstance().start(cfg.global().media)) { /* 失败退出 */ }

  // 3. 按配置建链路（工厂 + 连线，main 里不出现任何节点名）
  PipelineBuilder builder(cfg);
  std::vector<PipelineSet> sets;
  builder.build_all(sets);            // 单链失败只告警，其余照跑

  // 4. 启停顺序统一由 builder 负责
  PipelineBuilder::start_all(sets);   // 逆序：print → push → vpss → vi
  install_signal_handlers();          // SIGINT/SIGTERM → g_run = false
  while (g_run) sleep_for(200ms);     // 取代 while(1)

  // 5. 有序退出（与启动相反，最后关进程级资源）
  PipelineBuilder::stop_all(sets);    // 正序：vi → vpss → push → print
  MediaServer::getInstance().shutdown();
  MemManager::getInstance().FreeMem();
  return 0;
}
```

对比现状：`main_node.cpp:48-85` 那 20 行硬编码装配、`:94-96` 的死循环、永不执行的 `stop()` 全部消失；**加一路只改配置文件**。

### 5.6 兼容与迁移（v0 → v1）

| 场景 | 行为 |
| --- | --- |
| 只有 `config/push.json`（v0） | `load_with_fallback()` 合成一条 pipeline：`vi`（从 `push` 段取 `width/height/fps`）+ `push`（`push` 段）+ `print`（`print` 段）→ 行为与今天一致 |
| 只有 `config/node.json`（v1） | 直接使用 |
| 两者都在 | 优先 `node.json`，日志提示"已忽略 push.json" |
| 显式指定路径 | `argv[1]` 优先（沿用现有候选路径策略，`main_node.cpp:19-39`） |
| 回滚 | 删掉/改名 `node.json` 即回到 v0 单链路；代码里不需要额外开关（N5） |

---

## 6. 方案比选（四选一，含可行性与演进路径）

| 维度 | **A. 轻量装配层**（推荐先做） | **B. ServiceKernel 依赖注入** | **C. 插件化 + 动态库** | **D. 多进程拆分** |
| --- | --- | --- | --- | --- |
| 做法 | `NodeConfigManager` 单例 + `MediaServer` 单例 + `NodeFactory` + `PipelineBuilder`；节点内部继续用单例 | 新增 `ServiceKernel` 持有所有进程级服务；`PipelineNode::set_context(ctx)` 注入；节点不再摸单例 | 节点编译成 `.so`，`type` → `dlopen` + 注册表；配置可增删节点类型 | supervisor 进程 + 每路一个 worker 进程；共享内存/dma-buf 传帧 |
| 满足 F1–F5 | ✅ 全部 | ✅ 全部 | ✅ 全部（且支持第三方扩展） | ✅ 全部 |
| 额外收益 | 改动面最小、可增量上线、旧路径可回滚 | 可单测、可 mock、无隐藏全局态；是工业方案常态 | 现场不重编即可加节点；第三方接入 | 崩溃隔离（MPP/ZLM 崩一路不连坐）；可单独重启某路；多核调度更自然 |
| 主要代价 | 单例多、测试难、静态初始化顺序需注意 | 改所有节点签名（`set_data`/`set_context`），触面大；本轮与"不做范围外重构"红线冲突 | 构建/部署复杂（rpath、ABI、符号可见性）；调试困难；当前节点数量不需要 | 零拷贝断链（需 dma-buf）；内存池每进程一份（RSS 翻倍）；ZLM 服务端需跨进程汇聚或每进程独立端口 |
| 工作量（人日） | **3–5** | 8–12 | 15+ | 12–20 |
| 可行性风险 | 低（都是本仓库已有模式的延续） | 中（触面广，需回归全部节点） | 中高（工具链/部署未就绪） | 高（依赖 DMA-BUF 与帧分发重构，P0-5 必须先解决） |
| 何时选它 | **现在**：先把多链路与媒体服务上提跑通 | A 稳定后（M3），想提升可测性与长期可维护性 | 有第三方/现场定制需求时 | 4 路以上 + 带 AI + 现场要求单路故障隔离时 |

**推荐：A → B 演进，C/D 暂不做但保留接口空间。** 理由：
1. A 的 4 个模块全部是"新增文件 + main 改写 + PushNode 换媒体接口"，**不改任何节点的像素/编码逻辑**，符合 HANDOFF §2.9「不做范围外顺手修」。
2. A 的接口按 B 的形状写（`MediaServer` 对外只有 `Stream` 句柄 + `send`，`NodeConfigManager` 返回 `const&` 快照），M3 只需把"节点内部 `getInstance()`"换成"构造时注入的引用"，业务代码零改动。
3. D 是 4 路以上的正确终点，但它把「同进程零拷贝」这个当前唯一优势打掉，且内存池按进程翻倍（50 块 × 1080p ≈ 150MB/进程）。**在单进程都还没跑稳 2 路之前不要跳 D。**

---

## 7. 开发计划

### 7.1 M1（P0 必做，5–8 人日）——"4 路能跑、媒体服务脱节点"

| ID | 任务 | 产出 | 人日 | 依赖 | 验收 |
| --- | --- | --- | --- | --- | --- |
| T1 | 修 **P0-6**（`zlm_rtsp_stop` 的 stream/server key 与注册一致）与 **P0-7**（`onekey_start` 入参校验顺序） | `zlm_push_server.cpp` 小改 | 0.5 | — | 起一路→停一路，`g_stream_map`/`g_server_map` 均为空；重复 start/stop 10 次无泄漏 |
| T2 | 新增 `common/configmgr`（`node_config.h` + `node_config_manager.{h,cpp}`），含 v0 适配与校验 | 新模块 + `common/CMakeLists.txt` | 1 | — | 给 4 份样例配置（v1 正常 / 缺 type / 重名 / 端口冲突）能正确接受与拒绝，错误信息带字段路径 |
| T3 | 新增 `config/node.json`（4 路：ch0 = `dev:/dev/video0`，ch1~ch3 = 3 个 mp4 + `loop:true`） | 配置文件 | 0.2 | T2 | 用 v1 跑起来与 v0 单路现象一致 |
| T4 | 新增 `platform/media_server`，把 `mk_env_init2`/端口/流注册/关闭收敛进去；`PushNode` 改为持 `shared_ptr<Stream>` | 新模块 + `moudle_push.{h,cpp}` 改 | 1.5 | T1 | PushNode 里不再出现 `zlm_rtsp_onekey_start`/`zlm_rtsp_stop`；两路并跑、停一路另一路不断流 |
| T5 | 新增 `pipeline/node_factory` + `pipeline/pipeline_builder` | 新模块 + CMake | 1 | T2,T4 | 未知 type 报错；节点顺序错（无末端节点）报错 |
| T6 | 重写 `main_node.cpp`：config → 进程级资源 → builder → SIGINT 优雅退出 | main 改写 | 0.5 | T5 | Ctrl+C 后 `stop_all` 执行、`FreeMem` 执行、进程 0 退出 |
| T7 | `ViNode` 支持 `vi` 段（优先 `vi`，退回 `push`）+ `src` 前缀解析（`dev:` / `file:` / 网络流）+ 按前缀选 `CAPTURE_V4L2` / `CAPTURE_FFMPEG`（顺带解决 `CAPTURE_V4L2` 与 `/dev/video0` 双硬编码，P0-2） | `moudle_vi.cpp` 小改 | 0.7 | T2 | 老 `push.json` 行为不变；`dev:/dev/video0` 与 `file:/data/sample1.mp4` 两种源都能出帧 |
| **T7b** | **文件源循环播放 + 缩放适配**（新增，D1 带来）：`FfmpegCapture` 增加 `loop`（EOF → `av_seek_frame(0)` + `avcodec_flush_buffers`）与按 `set_output_size()` 缩放（`transfer_frame` 用目标尺寸建 sws）；`ViNode` 把 `loop` 透传下去 | `ffmpeg_capture.{h,cpp}` + `moudle_vi.cpp` | 1 | T7 | 3 路 mp4 连续跑 30 分钟不断流、不空转；720p 文件能缩放到 1080p 输出 |
| **T10a** | **池块数 + 队列深度 + 每路配额可配**（4 路的硬前提，§11.2 / R13 / §11.6，**D9 = 方案 A**）：`MemInit(size)` 的 50（`MemManager.h:21`）与 `MemQueue::InitQueue(name, 20)` 的 20（`MemPool.h:73`）改从 `global.pool.blocks` / `global.pool.queue_depth` 读，默认 64 / 8；新增 `global.pool.quota_per_channel`（每路最多同时占块数，`channel → inflight` 计数，超限**只丢该路**）；输出 `free/total`、每路 `inflight`、`drop_pool_empty` / `drop_quota` 两类计数 | `MemManager.{h,cpp}` + `MemPool.{h,cpp}`（**不改分配逻辑、不按 profile 分组**）+ configmgr | 1.5 | T2 | 4 路跑 10 分钟池不枯竭、丢帧率 < 1%；`blocks` 故意配成 8 能观察到明确丢帧告警；`quota_per_channel` 故意配成 2 时**只有该路丢帧、其他路正常**；`free + Σinflight == total` 恒成立 |
| T8 | 回归与文档；**删除 `config/push.json` 与 v0 适配代码**（D8），文档全量改指 `node.json` | STATUS/ARCHITECTURE/HANDOFF/README + 删文件 | 0.8 | T1–T7b | 4 路跑 10 分钟无丢块/泄漏；仓库里不再有 `push.json` 引用 |


**M1 验收标准（可量化，按 D1 = 4 路 × 30fps）**
1. `config/node.json`（v1）能启动 4 路：ch0 = `/dev/video0`，ch1~ch3 = 3 个 mp4 循环播放；迁移期 `push.json` 仍可启动（D8 验收后再删）。
2. 4 路同时跑 **30 分钟**：4 个 `rtsp://ip:8550/live/chX` 用 `ffplay` 或 `ffprobe -show_frames` 验证持续出帧（无卡死、无 0 帧率）；3 路文件源到 EOF 后**自动重播**（T7b）。
3. `PrintNode` 4 份报表互不污染（各自 `frame_no`、各自 fps）；`MemPool` 无枯竭、退出后块全部归还。
4. 停掉 ch0 的 push 节点**不影响** ch1~ch3 的 RTSP 服务（P0-6 回归）。
5. `Ctrl+C` 后：`stop_all` → `MediaServer::shutdown()` → `FreeMem()` 顺序正确，`valgrind --leak-check=full` 无 definite leak（P0-9 于 2026-11-10 修复，TESTLOG §5.13；P0-11 `MemMeta` 槽位生命周期同日修复，TESTLOG §5.14 —— 「反复启停」这条 M2 验收已可执行）。
6. 板端资源实测并记录：4 路时 `top -H` 的 CPU 占用、`free -m` 的 RSS、`cat /sys/class/thermal/*/temp`（阈值见 §11.4）。
7. 代码里**没有**任何 `while(1)` 与硬编码节点名/设备路径。
8. 上述每条验收都在 **`docs/TESTLOG.md`** 留有对应记录：每个 Tn 完成时写一条 L2（命令 + 结果摘要 + 结论 + 未做项），M1 收尾写 L3；**没有记录的项不算通过**（记录粒度见该文档 §1）。


**回滚点**：M1 全程只新增文件 + `main_node.cpp` 改写 + `moudle_push`/`moudle_vi` 小改。回滚 = `git revert` main 与 push 的两个 commit，或临时用 `config/push.json` + 旧装配分支。

### 7.2 M2（P1，5–8 人日）——"多路真正稳定"

| ID | 任务 | 说明 | 依赖 |
| --- | --- | --- | --- |
| T9 | **扇出拓扑与多下游用例**（原「P0-5 块级引用计数」已于 2026-11-10 核实为假阳性，无需再补计数） | 一路源 → 多路输出（主/子码流）的前提：`PipelineBuilder` 支持 `next: []` 多下游 + 一条多分支 L2 用例 + 下游只读/拷贝约定 | 无 |
| T10b | 池规格化（原 T10 的后半） | `global.pool.images` 支持多格式 → `MemManager::MemInit(blocks, images)`。块数/队列深度的"可配"已由 M1 的 T10a 解决；P0-9（`FreeMem` 不删 `FrameBuffer`）于 2026-11-10 修掉（TESTLOG §5.13），P0-11（`MemMeta` 覆盖泄漏 + `VpssNode` 自建槽位不回收）同日修掉（TESTLOG §5.14），本任务只剩多格式/配额 | T2 |
| T11 | `Logger` | 级别 + `[ch0][push1]` 前缀 + 限流；替换 `printf/cout/cerr` | 无 |
| T12 | `MetricsRegistry` | 每路 fps/丢帧/节点耗时窗口/池水位；`PrintNode` 退化为"某通道的报表视图" | T11 |
| T13 | `DeviceRegistry` | `/dev/videoX` 独占登记，第二路打开同一设备时明确报错而不是静默花屏 | T7 |
| T14 | `ResourceBudget` | MPP 实例配额；超出时拒绝建链并给出"降分辨率/减通道"的提示 | 无 |
| T15 | 配置校验器增强 | 端口冲突、两路抢同一设备、`app/stream` 重复、`name+channel` 重复 | T2 |

### 7.3 M3（P2，可选）——"可测试、可运维"

| ID | 任务 | 触发条件 |
| --- | --- | --- |
| T16 | `ServiceKernel` 注入（方案 B） | M1/M2 稳定后；要写单测时 |
| T17 | 配置热加载（SIGHUP → 重建受影响 pipeline） | 现场改端口不愿重启时 |
| T18 | `Supervisor` 健康检查/节点心跳/单路自动重启 | 需要 7×24 无人值守时 |
| T19 | 多进程演进（方案 D）评估 | 4 路以上 + AI 推理 + 要求故障隔离时 |

---

## 8. 风险与前置修复

| # | 风险 | 影响 | 对策 |
| --- | --- | --- | --- |
| R1 | **P0-6/P0-7 未修就做 MediaServer** | 停止路径失效会被"上提"后放大成"多路互踩"，排查成本成倍上升 | T1 作为 M1 第一项，独立提交、独立验证 |
| R2 | **扇出（一源多下游）拓扑不可达** | 配置里写成多下游也不生效（`PipelineBuilder` 只连 `i→i+1`），容易误以为已生效 | 引用计数本身完整（P0-5 假阳性，STATUS §4.1 第 5 条）；M1 只支持"每路独立源"，扇出留到 M2 T9，且先补多分支 L2 用例 |
| R3 | 队列名冲突 | `InitQueue(name)` 同名会共用队列，两路数据串扰且难排查 | builder 强制 `name+channel` 唯一（N4），冲突直接拒绝启动该链 |
| R4 | ZLM 全局锁（P1-14） | 多路送流串行化，4 路以上帧率下降 | M1 验收只做 2 路；4 路前先按流加锁/无锁队列（列 M2 附加项） |
| R5 | `MemInit` 固定 1920×1080 | 两路不同分辨率时，小分辨率路浪费内存、大分辨率路装不下 | M1 用同一规格（两路同分辨率）；M2 T10 池规格化 |
| R6 | 内存翻倍 | 50 块 × 1080p NV12 ≈ 3.0MB/块 ≈ 150MB；两路若各自建池翻倍 | M1 共用一个全局池（现状本就是全局池）；按通道独立池留到有实测需求时 |
| R7 | 静态初始化顺序 | 单例在 `main` 前被其他静态对象调用 → 未初始化 | 单例内部用 function-local static（C++11 起线程安全），禁止全局对象持单例引用 |
| R8 | 配置错误静默降级 | 使用者以为改了端口/分辨率，实际跑了默认值 | `NodeConfigManager` 校验失败必须报错退出（不是告警继续）；未知字段也提示 |
| R9 | 双路抢同一 V4L2 设备 | 一路 `open` 成功、另一路 EBUSY 或花屏 | T13 `DeviceRegistry`；M1 期先用不同设备或 `enable=false` |
| R11 | **文件源 EOF 不循环** | 3 路 mp4 播完（数分钟）后该链静默停摆，`read_frame` 返回 false → ViNode 1ms 空转，报表还在跑但没帧 | T7b 实现 `loop`；并给"文件源 EOF 且 `loop=false`"一个明确日志（不是静默） |
| R12 | **`capacity` 边界（1088 高）** | `transfer_frame` 要求 `capacity ≥ stride*h*1.5`，`stride=((w+127)/128)*128`。1920×1080 → 3,110,400 B 刚好等于池块容量；若文件是 **1920×1088**（编码器常见对齐）→ 需 3,133,440 B，**每帧都会因 capacity 不足失败** | 池块按 `h 向上取偶 + 128 对齐` 分配；或 T7b 里对 `capacity` 不足打印一次明确错误（现在每帧都打） |
| R13 | **4 路时池块数与队列深度不匹配** | 队列深度默认 20/路 → 最坏 4×20=80 块需求 > 池 50 块；即使 drop-oldest 生效，突发时会大面积丢帧 | 池块数按 `路数×(队列深度+2)` 给默认值（4×(8+2)=40，取 64 留余量）；`node.json` 里 `pool.blocks` 可调（T10） |
| R14 | **3 路软解 CPU** | mp4 走 `avcodec` 软解 + `sws_scale`，1080p30 单路约 1 核；3 路 + 1 路 V4L2 + 4 路 MPP 编码，RK3588 8 核（4×A76+4×A55）可能接近饱和 | 文件源优先用 **720p**；必要时上 MPP 硬解（但 `rk_mpp_decoder` 当前是草稿且 P0-8 会崩，不要顺手启用） |
| R15 | **每帧 `fflush(stdout)`** | `moudle_push.cpp:200` 每帧 flush；4 路 120 帧/秒 × 4 = 480 次/s，虽不致命但会放大 ZLM 全局锁（P1-14）的抖动 | M2 与 Logger 一起处理（分级 + 限流），M1 不动（红线：不做范围外顺手修） |


---

## 9. 遗留待确认（原 8 项已于 2026-11-08 确认，见 §1.4）

1. **3 个 mp4 的具体规格**：分辨率 / 编码 / 时长 / 存放路径。建议 720p、H.264、1–3 分钟、放 `/data/`（理由见 §11.1/R12/R14）。
2. **`/dev/video0` 实际能力**：需要 `v4l2-ctl --list-formats-ext -d /dev/video0`。YUYV 1080p30 占 USB 带宽很大，可能需要 MJPEG 才跑得满 30fps。
3. **通道命名**：用 `ch0..ch3`，还是业务名（`door`/`gate`/…）？`stream` 是任意字符串，会影响播放地址与后续对接。
4. **池块数上限**：64 块 ≈ 199MB 的常驻内存是否可接受（板端总内存余量）。
5. **是否 4 路都要推流**：若 1–2 路只做本地分析（print/AI），`push.enable=false` 可省掉对应 MPP 编码负载。
6. ~~**池要不要按 profile 分组（方案 A vs A+，见 §11.6.2）**~~ → **已确认（2026-11-08，记为 D9）：走 A = 共享池 + 每路配额**（+0.5 人日，常驻约 199MB）。A+（按 profile 分组池，约 92MB）**不实施**，保留为将来内存吃紧时的备选；若改走 A+ 需先修 `MemInit` 的"一块挂所有 profile"。

---

## 10. 关键概念详解（回答 D2 / D5 / D6 的三个追问）

### 10.1 主码流 / 子码流是什么，是不是主流（对应 D2）

**定义**：同一个视频通道，编码器**同时产出两路（或多路）参数不同的码流**：

| | 主码流（Main Stream） | 子码流（Sub Stream） | 第三流（Third Stream） |
| --- | --- | --- | --- |
| 分辨率 | 1080p / 4K | D1(704×576) / 640×480 / 720p | 704×576 / 1080p |
| 码率 | 4–8 Mbps | 256–1024 kbps | 512–2048 kbps |
| 帧率 | 25 / 30 | 15 或与主码相同 | 15–25 |
| 典型用途 | NVR 存储、大屏、事后取证 | 手机远程看、多画面宫格预览 | 智能分析（AI 只需要低分辨率） |

**为什么是行业主流**：这是安防 IPC / NVR 的标配设计，海康、大华、宇视的每台网络摄像机都有"主码流/子码流/第三流"三档。原因是同一条录像要同时满足两种矛盾需求：
- 存证要清晰 → 主码流；
- 16 宫格预览时客户端要同时解 16 路，若全用 1080p 主码流，客户端 CPU 和网络直接撑爆 → 宫格用子码流，单画面放大时再切主码流。

**技术本质（容易误解的点）**：一条 H.264/H.265 码流的分辨率是**编码时固定**的，不能"把主码流的文件裁出个低分辨率版本"。要得到第二路不同分辨率，只有两条路：
1. **重新编码**（业界 99% 的做法）：一路 YUV → 缩放 → 两个独立编码器实例；
2. 可分级编码（SVC / H.264 分层），需要编码器和播放端同时支持，安防极少用。

所以"主 + 子码流"在我们的流水线里就等于：`vi` 出 1 块 → 两个下游（清晰路、低码路），低码路前面还要一个缩放节点（VPSS）。

**我们现在为什么不做**：
- **必须先补扇出拓扑与多分支用例**：`PipelineBuilder` 现在只连 `i→i+1`，配置写多下游**不生效**；引用计数本身是完整的（P0-5 于 2026-11-10 核实为假阳性），但一块 `MemBlock` 被两个下游共用时，只要有下游**就地改写**它，另一个就读到被污染的数据（`PushNode::onFrame` 只读不写才安全）。
- 编码实例翻倍：4 路 × 2 = 8 个 1080p30 编码实例，RK3588 的 MPP 编码吞吐要重新评估（不是不能，是余量未知）。
- 池块数与队列深度都要按"8 条链路"重算。

**结论**：当前 4 路、每路基本只有一个播放端 → 单码流足够（配 1080p 或 720p 单档即可）。架构上已经留好口子：`pipelines[]` 允许一条链路放多个 `push` 节点（`stream` 不同、`bitrate` 不同），将来要做子码流时，只需 ① 补扇出拓扑 + 多分支用例（原「P0-5 块引用计数」已核实无需修）② 加 VPSS 缩放 ③ 在配置里多写一个 push 节点，**不用改架构**。

### 10.2 热加载指什么（对应 D5）

你的理解是**对的**：进程不退出，改完配置文件（或通过管理接口下发）后新配置自动生效。常见的三种触发方式：`SIGHUP` 信号、后台线程轮询文件 `mtime`、HTTP/MQTT 管理接口。

难点不在"重新读文件"，而在**哪些参数能原地生效**：

| 参数 | 能否不重启进程生效 |
| --- | --- |
| 码率 / 帧率（GOP 不一定） | 可以（MPP 支持运行中调码率，部分参数要重建编码器） |
| 分辨率 / 编码格式（H.264↔H.265） | 不行：必须销毁并重建编码器，下游要重新对齐，播放端会断流重连 |
| 端口 / app 名 | 不行：要重启 RTSP 服务，所有已连接的客户端全断 |
| 新增/删除一路通道 | 可以，但要完整走"建链/停链 + 释放资源" |

而且热加载会把"反复启停"变成常态，一旦销毁路径有泄漏，就会越改越漏。P0-9（`FreeMem` 不 delete `FrameBuffer`）与 P0-11（`MemMeta` 槽位泄漏 + `VpssNode` 自建槽位不回收）均于 2026-11-10 修掉（TESTLOG §5.13 / §5.14），即"反复启停必漏 12 MB/轮"与其残余 128 B/轮都已解除；销毁路径剩下的已知项是**停链未 join**。

**结论**：暂不做（D5）。但 `NodeConfigManager::load()` 仍按"可重复调用、返回不可变快照"设计，将来加 `SIGHUP` 时只需要再写一个"新旧快照 diff → 只重建受影响的链路"的小模块，不必动架构。

### 10.3 RTSP 鉴权 与 rtsps 的区别（对应 D6）

这是**两件不同的事**，常被混为一谈：

| | 鉴权（Authentication） | rtsps（RTSP over TLS） |
| --- | --- | --- |
| 解决什么 | **谁能拉流**：没密码看不了 | **传输是否加密**：防抓包、防中间人 |
| 机制 | RTSP 标准 `401 Unauthorized` + `WWW-Authenticate`（Basic/Digest），地址写成 `rtsp://user:pass@ip:8550/live/ch0` | 在 RTSP 外套一层 TLS，通常用 322 或 8551 端口，需要证书 |
| ZLM 侧 | `rtsp.auth` / 播放鉴权 hook | `rtsp.sslport` + 证书 |
| 现状 | 封装未暴露 → 不做 | 封装未暴露 → 不做 |

你问的"rtsps"属于**加密**，不是鉴权。两者可以单独用，也可以一起用。

**结论**：都不做（D6）。安全提醒：无鉴权时，同网段任何能访问 8550 的机器都能看到画面，生产环境上线前至少要加 Basic 鉴权或网络隔离。

---

## 11. 4 路 30fps 的容量与性能估算（D1 决策的直接后果）

### 11.1 各环节资源估算（RK3588：4×A76 + 4×A55）

| 环节 | 单路成本 | 4 路合计 | 说明 |
| --- | --- | --- | --- |
| 采集 ch0（V4L2 1080p30，`V4l2Capture` 转 NV12 + `set_output_size` 缩放） | 0.5–1 核 | 0.5–1 核 | YUYV1080p30 原始带宽约 124 MB/s，USB3 才稳 |
| 解码 ch1~ch3（ffmpeg 软解 H.264 1080p30 + `sws_scale`） | 0.8–1.2 核 | 2.4–3.6 核 | **改成 720p 约减半**（R14） |
| 编码 4 × MPP H.265 1080p30 | MPP 硬件 | 硬件 | 4 路 1080p30 在 RK3588 编码能力内（余量需实测，见 §11.4） |
| 送流（`mk_media_input` + ZLM 全局锁，P1-14） | 30 次/s | 120 次/s | 每帧 `fflush` 会放大抖动（R15） |
| 内存池 | 1080p NV12 ≈ 3.11 MB/块 | 64 块 ≈ 199 MB | 块数公式见 §11.2 |
| 网络上行 | 3–4 Mbps/路 | 12–16 Mbps | 千兆局域网/有线 OK；4G 上行需降到 1–2 Mbps/路 |

**结论**：软解 2.4–3.6 核 + 采集 1 核 ≈ **4–5 核**，8 核能跑，但余量不大。降低风险的两个开关：① mp4 用 720p；② 只给需要的通道开 `push.enable`。

### 11.2 池块数怎么算（必须改，否则 4 路必丢帧）

同一时刻被占用的块 = Σ每路（采集正在写的 1 块 + 队列里排队的 N 块 + 正在编码的 1 块）：

```
所需块数 ≈ 路数 × (队列深度 + 2)
4 路 × (8 + 2) = 40  → 取 64 留突发余量（≈199 MB）
```

现状对不上：`MemManager.h:21` 的 `MemInit(int size = 50)` 默认 **50 块**（`main_node.cpp:43` 正是无参调用），而队列深度是 `MemPool.h:73` 的 `InitQueue(name, int size = 20, ...)` → **20/路**，4 路最坏需要 `4×20=80` 块。**不改必然大面积丢帧**（R13）。M1 的 T10a 把这两处一起配置化（`global.pool.blocks` / `global.pool.queue_depth`）。


### 11.3 丢帧与延迟表现（4 路时的预期）

- 队列满的策略是 drop-oldest → 表现为画面偶尔跳帧，**不会累积延迟**（这是好事，直播场景正确取舍）。
- 文件源天然"想读多快就读多快"，但 `ViNode` 每帧按 `frame_duration` 睡眠节流（`moudle_vi.cpp` 里的 fps 控制）→ mp4 被自动限制到 30fps，不会一次抽干池子。
- 反例：若 `fps` 配置成了 0 或很大（如 1000），节流失效 → 3 路文件源会瞬间把池抽干并疯狂丢帧。**T2 配置校验必须拒绝 `fps<=0` 和离谱值**。

### 11.4 M1 板端实测项与建议阈值（验收第 6 条用这张表）

| 指标 | 建议阈值 | 采集命令（不用管道，避免和 shell 引号打架） |
| --- | --- | --- |
| 系统负载 | load1 < 8（8 核） | `cat /proc/loadavg` |
| 进程 CPU | 总计 < 70% × 8 核 | `top -b -n 1 -p $(pidof testnode)` |
| 单线程峰值 | < 90% | `top -H -b -n 1 -p $(pidof testnode)` |
| RSS | < 600 MB | `cat /proc/$(pidof testnode)/status` 看 VmRSS |
| 温度 | < 80 ℃ | `cat /sys/class/thermal/thermal_zone0/temp` |
| 4 路出帧 | 30 分钟不中断 | `ffprobe -v error -rtsp_transport tcp -select_streams v -count_frames -show_entries stream=nb_read_frames -of csv rtsp://<IP>:8550/live/ch1` |
| 池健康 | 退出后 `FreeMem` 后剩余块数 = 总块数 | `PrintNode` 报表或 `MemManager` 统计日志 |

### 11.5 一句话结论

**1 路摄像头 + 3 路 mp4、4×30fps 在 RK3588 上可行**，但成立的前提是三件事必须先做：
1. 文件源支持循环播放（T7b，否则几十分钟后 3 路静默停摆）；
2. 池块数 / 队列深度按 `路数×(队列深度+2)` 配置化（T10a，否则必丢帧）；
3. mp4 优先 720p 或只给需要的通道开推流（R14，软解 CPU 是主要瓶颈）。

---

### 11.6 内存池归属：全局共享还是每路一个？（2026-11-08 追问的答复）

#### 11.6.1 现状事实（代码级，改之前必须知道）

| 事实 | 位置 | 含义 |
| --- | --- | --- |
| 池是**进程级单例** | `MemManager.h:14-17`、`MemPool.h:108-112` 双 `getInstance()` | 全进程只有一个池，`main_node.cpp` 调一次 `MemInit` |
| **块尺寸单一且硬编码** | `MemManager.cpp:7` 构造里 `addImgConfig({1920,1080,FMT_NV12,"nv12"})` | 所有块都是 1080p NV12（3.11 MB），`addImgConfig()` 是 public 但**当前无人调用** |
| **一块携带全部 profile** | `MemManager.cpp:20-32`：每块循环 `imgMap` 逐个 `new FrameBuffer` | ⚠️ 若配第 2 个 profile，**每块内存 = Σ各 profile**（加 profile 反而爆内存）。改造时必须改成"一块只挂一个 profile" |
| 空闲块是**单一队列** | `MemPool.h:141` `free_list_` + `free_mtx_` | 没有 profile / channel 维度，谁先取谁用 |
| `acquire(name)` 的 name | `MemPool.cpp:164` | 只用于统计与队列查找，**不决定尺寸、不代表归属** |
| 块是**无状态可复用资源** | `MemPool.h:38-66`：`owner_name` + `user_list` + `user_cnt` + `subUser()` 引用计数 | 一帧可被多个下游共享（分支），引用归零才回池 |
| 每路唯一的软上限 | `MemPool.h:73` `InitQueue(name, size=20)` | 只有队列深度，**不是**池占用上限 → 4 路最坏想吃 80 块 > 池 50 块（R13） |

#### 11.6.2 三个层次的选择

| 方案 | 做法 | 内存利用率 | 故障隔离 | 实现成本 | 结论 |
| --- | --- | --- | --- | --- | --- |
| **A 共享池 + 每路配额**（**已确认 = D9**） | 一个 `free_list_`；`channel → inflight 计数`，超 `quota` 丢帧 | **最高**（空闲路份额自动让给忙路） | 中（靠配额兜底） | 低（+0.5 人日） | ✅ **M1 采用（2026-11-08 确认）** |
| **A+ 按 profile 分组池** | 池内按 `p1080 / p720` 各一组 `free_list_`，链路静态绑定 profile | 高（消除混分辨率浪费） | 中 | 中（+1 人日，含 `MemInit` 改造） | ⭕ **暂不实施**（D9 已定走 A；将来内存吃紧再评估） |
| **B 每链路独立池** | N 个池实例，`channel → pool` 映射 | **低**（每路按峰值独占，总量翻倍） | **最好** | 高（`MemPool` 单例改成多实例，`FreeMem`/统计/Profiler 全改） | ❌ 不建议 |

**为什么不做"块 → 链路归属 map"**：块是无状态资源，帧的生命周期只有几十毫秒。给块绑定归属后，A 路空闲的块无法给 B 路用（碎片化），还要跨路转移所有权；收益为负。要做的是**每路计数（配额）**，不是**归属绑定**。链路与池的关系是"静态绑定 profile"（配置项），不是运行时映射表。

#### 11.6.3 共享池的三个真实风险（必须同时处理，否则别用共享池）

1. **一路拖垮全局**：某路下游卡住 / 泄漏块 → 池空 → `acquire` 全返回 `nullptr` → **所有路一起丢帧**（现在就是这种结构）。
   → 对策：每路 `quota`（超限只丢自己）+ 泄漏看门狗（块本身带 `owner_name`/`user_list`，直接打印"谁没还"）。
2. **混分辨率浪费**：池块统一 1080p（3.11 MB），720p 链路每帧只用 1.32 MB → **浪费 56%**。
   → 对策：要么全 1080p（简单、199 MB），要么按 profile 分组池（省一半，见 §11.6.4）。
3. **软上限缺位**：现在每路上限只有队列深度 20，`4 × 20 = 80 > 池 50/64`，一路突发能让其他路饿死。
   → 对策：`quota_per_channel` 与 `queue_depth` 分开配（配额管"池占用"，队列深度管"延迟"）。

#### 11.6.4 内存账（3×720p + 1×1080p，每路配额 8）

| 方案 | 构成 | 合计 | 相对浪费 |
| --- | --- | --- | --- |
| 现状/统一 1080p 池 | 64 块 × 3.11 MB | **≈ 199 MB** | 720p 链路每帧浪费 1.73 MB（56%） |
| 按 profile 分组 | p1080 16 块（50 MB）+ p720 32 块（42 MB） | **≈ 92 MB** | 无 |
| 全部降到 720p | 48 块 × 1.32 MB | **≈ 63 MB** | 无（但 ch0 掉分辨率） |

**每路配额怎么定**：`quota = queue_depth + 2`（采集正在写 1 + 编码中 1）；`queue_depth` 取 4–8。
例：4 路 × (8 + 2) = 40 → 总池取 48–64 留突发余量（与 §11.2 公式一致）。

> **D9 决策（2026-11-08）：走方案 A** → 统一 1080p 池，M1 实际常驻 **≈ 199 MB**（64 块）。A+ 的分组池（≈ 92 MB）**不实施**，仅作将来内存吃紧时的备选。

#### 11.6.5 主流业务方案对照（结论：共享池 + 每路配额是行业默认）

| 平台 / 项目 | 池模型 | 每路控制手段 |
| --- | --- | --- |
| **NVIDIA DeepStream** | 两层：每 source 解码面池 + `nvstreammux` 后全局 batch 池 | `num-extra-surfaces`（每路）、`buffer-pool-size` / `batched-push-timeout`（全局） |
| **GStreamer** | 每 element 协商 `GstBufferPool`，可共享 | `queue max-size-buffers` + `leaky=downstream`（每路配额，超了丢旧帧） |
| **ZLMediaKit**（本项目推流端） | **无内存池**，帧用引用计数 `Frame::Ptr` | 每路独立 `MediaSource`，GOP 缓存有上限 |
| **SRS** | 每 stream 独立 `SrsLiveSource` + 抖动队列 | `queue_length` 每路可配 |
| **FFmpeg / libav** | `AVBufferPool` 按尺寸缓存 | 无每路概念 |
| **NVR / 工业视觉盒子惯例**（海康、大华一类） | **按分辨率分组的共享池** | 每路 credit/配额 + 超限丢帧（实时优先于完整） |

→ 三条可复用的行业共识：
1. **共享池 + 每路配额**是默认解；"每 channel 独立池"几乎没人用在多路视频上（除非租户级隔离/SLA 强约束）。
2. **按 profile（分辨率为代表）分组**是第二层优化，只在混分辨率且内存紧张时才做。
3. **实时视频宁可丢帧也不积压**：所有方案的队列策略都是 drop-oldest，配额超限就丢，绝不让生产者阻塞。

#### 11.6.6 `node.json` 的对应配置（v1.1 增补，向后兼容）

**已确认走方案 A（D9）**，所以池的配置只写**三处**，完全不需要"每路一个池"：

```jsonc
"global": {
  "pool": {
    "blocks": 64,              // 池总块数（全局一个池）
    "queue_depth": 8,          // 每路队列深度（管延迟/丢帧）
    "quota_per_channel": 10,   // 每路最多同时占用块数（管池占用；建议 = queue_depth + 2）
    "images": [ { "name": "nv12", "width": 1920, "height": 1080, "fmt": "nv12" } ]
  }
}
```

若要按分辨率分组（§11.6.2 方案 A+，M2 再做），扩展成这样——**链路静态绑定 profile，不是运行时映射**：

```jsonc
"global": { "pool": { "profiles": [
  { "name": "p1080", "width": 1920, "height": 1080, "fmt": "nv12", "blocks": 16 },
  { "name": "p720",  "width": 1280, "height": 720,  "fmt": "nv12", "blocks": 32 }
]}},
"pipelines": [ { "channel": 1, "profile": "p720", "nodes": [ /* vi src 用 p720 */ ] } ]
```

> ⚠️ 走 profile 分组前**必须先改 `MemInit`**（见 §11.6.1 第 3 行）：现在是"一块挂所有 profile"，直接加 profile 会让每块内存变成各 profile 之和。

#### 11.6.7 实施要点（并入 M1 的 T10a，0.5 → 1.5 人日）

| # | 改动 | 位置 | 验收 |
| --- | --- | --- | --- |
| 1 | `MemInit(blocks, images)` 从配置注入，删掉构造里的硬编码 profile | `MemManager.{h,cpp}`、`MemManager.cpp:7` | 配 32/64 块都能起来；`PrintNode` 报出总块数 |
| 2 | 每路配额：`channel → inflight` 计数，acquire 前置检查，超限返回 `nullptr` | `MemPool.{h,cpp}`（`acquire` / `release`） | 故意配 `quota=2` 能看到该路丢帧且**其他路不受影响** |
| 3 | 丢帧分类计数：`drop_pool_empty` 与 `drop_quota` 分开统计 | `MemPool` + `PrintNode` 报表 | 报表能区分"池空"和"配额满"（排障关键） |
| 4 | 健康指标：`free/total`、每路 `inflight`、泄漏看门狗（打印卡的块 `owner_name`） | `PrintNode` / `MemManager` | 4 路跑 10 分钟后 `free + Σinflight == total` 恒成立 |
| 5 | ~~按 profile 分组 `free_list_`（`unordered_map<profile, queue<MemBlock*>>`，`acquire(profile, name)`）~~ | `MemPool.{h,cpp}` | **不实施**（D9 已确认方案 A）；仅保留为将来内存吃紧时的备选，预期常驻降到约 92MB |

> 红线复用：**acquire 之后所有失败分支必须 release**（HANDOFF §2.1 第 5 条）；配额计数必须与块的实际归还同源，否则一路泄漏会表现为"该路永久丢帧"——所以第 4 项的健康指标不是可选项。

---

## 12. 变更记录

| 日期 | 变更 |
| --- | --- |
| 2026-11-08 | 初版：需求分析（F1–F5 / N1–N6）、现状 18 项事实盘点、三条判据与 16 项上提清单、7 类行业方案调研、四方案比选、`node.json` v1 schema、接口草案、M1–M3 分期计划与风险表 |
| 2026-11-08 | 二次（用户确认 8 项）：新增 §1.4 决策表 D1–D8（**4 路 × 30fps = 1 路 `/dev/video0` + 3 路 mp4**、共用 8550 端口只分 `stream`、不做热加载/鉴权/子码流、保持单进程、**迁移后删除 `push.json`**）；schema 样例改为 4 路并新增 `vi.src` 前缀与 `vi.loop`；新增 §10 概念详解（主/子码流、热加载、鉴权 vs rtsps）、§11 4 路容量估算与验收阈值；任务表新增 **T7b**（文件源循环+缩放）、**T10a**（池块数/队列深度可配），T10 → T10b；风险新增 R11–R15；M1 工期 3–5 → **5–8 人日** |
| 2026-11-08 | 三次（纯文档）：M1 验收标准新增第 8 条——**每条验收必须在 `docs/TESTLOG.md` 留记录**（每个 Tn 一条 L2，M1 收尾一条 L3，"没有记录的项不算通过"）；配套新建 `docs/TESTLOG.md`（分级 L1/L2/L3 + 模板 + 必填项 + token 成本评估） |
| 2026-11-08 | 四次（纯文档，回答"池要不要按链路分"）：新增 **§11.6 内存池归属**——① 现状事实（进程级单例 / 块尺寸统一硬编码 1080p / **一块挂所有 profile**（加 profile 会爆内存）/ 单一 `free_list_` / `acquire(name)` 无归属语义）；② 三方案对比（**A 共享池+每路配额** 推荐 / A+ 按 profile 分组池 / B 每路独立池 不建议）；③ 共享池三个风险与对策（一路拖垮全局、混分辨率浪费 56%、软上限缺位）；④ 内存账 199MB vs 92MB vs 63MB；⑤ 业界对照（DeepStream / GStreamer / ZLMediaKit / SRS / NVR 惯例 = 共享池 + 每路配额）；⑥ `node.json` v1.1 增补 `quota_per_channel`；⑦ 实施要点并入 T10a（0.5 → 1.5 人日）。§9 新增第 6 项待确认（默认按 A 实施） |
| 2026-11-08 | 五次（纯文档，用户确认池方案 = **A**）：**记为 D9**——§1.4 决策表新增 **D9**（A = 共享池 + 每路配额；不按链路分池、不做"块→链路归属 map"；A+ 不实施）；§9 第 6 项标记已确认；§11.6.2 标注 A 为 M1 采用 / A+ 暂不实施；§11.6.4 注明 M1 实际常驻 ≈199MB；§11.6.6 收敛为方案 A 的"三处配置"；§11.6.7 第 5 项（A+）标记不实施；**T10a 描述与验收同步扩展**（块数 + 队列深度 + `quota_per_channel`；验收新增"配额隔离"与"`free + Σinflight == total`"），人日 0.5 → 1.5 |


> 维护约定：M1 落地后回写 [STATUS.md](STATUS.md)（模块完成度、P0 清单）与 [HANDOFF.md](HANDOFF.md)（§1.2 关键坐标、§1.4 目录地图、附录 B），并把本文"状态"改为"已实现（M1）"。

---

## 附录 A：M1 文件改动清单（精确到路径，供实施时对照）

| 操作 | 路径 | 说明 |
| --- | --- | --- |
| 改 | `src/platform/rtsp/zlm_push_server.cpp` | 仅 T1：stop 的 key 与注册一致、入参校验顺序（P0-6/P0-7） |
| 新增 | `src/common/configmgr/node_config.h` | `NodeDesc` / `PipelineDesc` / `GlobalDesc` |
| 新增 | `src/common/configmgr/node_config_manager.h` / `.cpp` | 单例、加载、校验、v0 适配、段包装 |
| 改 | `src/common/CMakeLists.txt` | 加上述 `.cpp` 与 include 目录 |
| 新增 | `config/node.json` | v1 多链路样例（4 路：1 硬 + 3 文件） |
| 删 | `config/push.json` | T8/D8：M1 验收通过后删除，同时删掉 configmgr 里的 v0 适配分支 |
| 改 | `src/platform/capture/ffmpeg_source/ffmpeg_capture.{h,cpp}` | T7b：`set_loop(true)` 循环播放（EOF → `av_seek_frame` + `avcodec_flush_buffers`）、`set_output_size()` 缩放（`transfer_frame` 用目标尺寸建 sws，现在源尺寸=目标尺寸） |
| 改 | `src/common/MemManager/MemManager.{h,cpp}` + `src/common/MemManager/MemPool.{h,cpp}` | T10a：`MemInit(size)` 与 `InitQueue(name, 20)` 的默认值改由配置传入（不改分配逻辑） |
| 新增 | `src/platform/media_server/media_server.h` / `.cpp` | 进程级 RTSP 服务与流注册表 |
| 改 | `src/platform/CMakeLists.txt` | 加 `.cpp`；确保 `rtsp` 目标被链接（P1-1/P1-2 相关） |
| 新增 | `src/pipeline/node_factory.h` / `.cpp` | `type` → 节点 |
| 新增 | `src/pipeline/pipeline_builder.h` / `.cpp` | 建链/连线/启停 |
| 改 | `src/pipeline/CMakeLists.txt` | 加 `.cpp` 与 include |
| 改 | `src/pipeline/moudle_push/moudle_push.h` / `.cpp` | `ZlmServerStreamHandle` → `std::shared_ptr<MediaServer::Stream>`；删 `init_zlm_push` 里的 onekey_start |
| 改 | `src/pipeline/moudle_vi/moudle_vi.cpp` | 优先 `vi` 段、`src` 从配置读（保留 `push` 段兼容） |
| 改 | `src/main/test/main_node.cpp` | 改成 config → 资源 → builder → 信号 → 有序退出 |
| 改 | `src/main/CMakeLists.txt` | 若 `testnode` 需链 `common_api`/`platform_api`（编译期验证） |
| 改 | `docs/STATUS.md`、`docs/HANDOFF.md`、`docs/ARCHITECTURE.md`、`README.md` | 回写现状、目录地图、配置说明 |

（本文件为设计提案，落地时按附录 A 逐项实施。）






