# 面试问答手册（高频 · 核心 · 深入 · 内核 · C 语言落点）

> 用途：把这个仓库**真实做过什么、为什么这么做、哪里还没做完**讲清楚。
> 分工：[BUG_CASES.md](BUG_CASES.md) 是「单个 bug 的原理档案」，[TESTLOG.md](TESTLOG.md) 是「证据」，本文是「**别人问起来怎么答**」。
> 原则：**只讲代码里能指到行号的东西**。凡本文写「已验证」的，都能在 TESTLOG 找到对应记录；凡是没做完的，§8.2 有一张诚实清单——面试时说「这块我还没做/只知道思路」比编一个实现要安全得多。
> 基线：分支 `rk3588`、代码校准点 `15cf975`、C++14、aarch64 板端本机编译（2026-11-10 快照）。

## 目录

<!-- TOC -->

- [0. 先背这三段话（不同时长）](#0-先背这三段话不同时长)
- [1. 项目速览（答任何问题前先在脑里过这张图）](#1-项目速览答任何问题前先在脑里过这张图)
- [2. 高频问题（20 题 · 一定会被问）](#2-高频问题20-题--一定会被问)
- [3. 核心问题（15 题 · 考的是"你真的想过为什么"）](#3-核心问题15-题--考的是你真的想过为什么)
- [4. 深入问题（12 题 · 决定你是"用过"还是"做过"）](#4-深入问题12-题--决定你是用过还是做过)
- [5. 这个项目到底"碰没碰内核"？（面试最容易被追问的一块）](#5-这个项目到底碰没碰内核面试最容易被追问的一块)
  - [5.1 一句话结论](#51-一句话结论)
  - [5.2 直接碰内核：V4L2 的完整时序（能背下来就赢一半）](#52-直接碰内核v4l2-的完整时序能背下来就赢一半)
  - [5.3 `mmap` 这一层，面试官最爱问什么](#53-mmap-这一层面试官最爱问什么)
  - [5.4 间接碰到的内核子系统（说得出名字就够，别硬编细节）](#54-间接碰到的内核子系统说得出名字就够别硬编细节)
  - [5.5 内核向连环问（10 问 · 每题给"能站住"的答法）](#55-内核向连环问10-问--每题给能站住的答法)
  - [5.6 能主动说的加分点（面试官会眼前一亮）](#56-能主动说的加分点面试官会眼前一亮)
  - [5.7 不能吹的边界（说错就掉分）](#57-不能吹的边界说错就掉分)
- [6. 项目里能练 C 语言熟练度的 8 个落点](#6-项目里能练-c-语言熟练度的-8-个落点)
- [7. 准备路线（4 周 · 每周都能产出可讲的东西）](#7-准备路线4-周--每周都能产出可讲的东西)
- [8. 收尾三件事](#8-收尾三件事)
- [9. 速查表（问题 → 代码位置 → 档案）](#9-速查表问题--代码位置--档案)

<!-- /TOC -->

---

## 0. 先背这三段话（不同时长）

### 0.1 一句话（15 秒）

> 一个跑在 RK3588 上的**多路视频接入与推流服务**：V4L2/FFmpeg 采集 → 自研固定块内存池 → 线程化节点流水线（vi→vpss→push/print）→ MPP 硬编码 → ZLMediaKit 出 RTSP，配置驱动、可多通道并发。

### 0.2 三十秒（面试官说"介绍一下你的项目"）

> 主体是 C++14，但**底层一整条都是 C ABI**：V4L2 用 `ioctl + mmap` 直接跟内核交互，采集/解码走 FFmpeg 的 C API，编码走瑞芯微 MPP 的 C API，出流走 ZLMediaKit 的 C API。
> 架构上我把"帧"和"内存"拆开了：内存是进程级预分配的固定块（默认 50 块，每块 1080p NV12），帧只是块上的一个**视图**，节点之间传的是 `MemBlock*`，靠引用计数决定什么时候归还池子。
> 我做的主要工作是把原来写死在 `main` 里的单链装配改成**配置驱动**（JSON → 工厂 → 构建器），并修掉了一批跨层 bug——最典型的是"四路并发时 1080p 那路下半屏变绿"，根因是池块上的视图被另一条链路改写。这些都留了测试记录和 bug 档案。

### 0.3 三分钟（要展开时按这个顺序，别跳）

1. **数据流**：`ViNode` 取块 → 采集写入 → `push_input_pipe` → 下游 `MemQueue` → `VpssNode`（可选）→ `PushNode` 编码送 ZLM → 终点 `release` 回池。
2. **两个解耦**：①节点之间只认 `MemBlock*`，不认具体实现；②配置只认 `type/name/channel/config`，`main` 里没有任何节点名。
3. **一处硬骨头**：池块是共享的，`FrameBuffer` 的 `width/height/stride` 是**块级状态**——谁都能改，这就是 P0-14 的来源。
4. **怎么证明**：`mallinfo2` 6 轮零增长、ASAN 零泄漏、四路 45s 编码错误 0、SIGINT 后 `FreeMem 释放 50 块 / 100 个槽位`、`EXIT=0`。
5. **边界**：`VpssNode` 的 `process_frame()` 现在是透传（TODO 注释在 `moudle_vpss.cpp:386-429`），preprocess 像素级实现是占位，这些我不装成"已完成"。

---

## 1. 项目速览（答任何问题前先在脑里过这张图）

```
配置 node.json (v1) / push.json (v0)
   │  NodeConfigManager：校验 + ${global.*} 展开 + 不可变快照
   ▼
PipelineBuilder：check_order(R1~R5) → NodeFactory.create → create/set_data/link_to
   ▼
┌──────────── 每条链路一组节点，每个节点一个线程 ────────────┐
│ ViNode ──MemQueue──▶ VpssNode ──MemQueue──▶ PushNode        │
│ (OWNER 队列)          (USER 队列)            (USER 队列)     │
│   取块/采集/缩放        转码/叠加(待实现)       MPP 编码→ZLM   │
└──────────────────────────────────────────────────────────────┘
   │ 终点 release → 引用计数归零 → MemBlock::reset → 回 free_list
   ▼
MemManager::FreeMem()：槽位 clear() → FreeBuffer/Profiler/MemMeta/MemBlock 全回收
```

| 事实 | 值 | 出处 |
| --- | --- | --- |
| 代码规模 | `src/` 下 64 个 `.cpp/.h`，约 8.9k 行（不含 `json.hpp` 25.5k） | `find src` 实测 |
| 池块 | 默认 50 块，每块 `Allocate(1920,1080,NV12)`，`capacity = w*h*3` = 6.22 MB | `MemManager.h` / `frame_buffer.h:101` |
| 队列 | 每节点 `MemQueue(name, 20)`，满则 **drop-oldest** | `MemPool.h:73`、`MemPool.cpp:109-121` |
| 节点 | `vi`（采集）/ `vpss`（预处理，当前透传）/ `push`（编码推流）/ `print`（终点+报表） | `node_factory.cpp:32-37` |
| 通道 | 4 条链路同时跑（1 路 V4L2 + 3 路 mp4），实测 29.9 / 23.9×3 fps | TESTLOG §5.12 |
| 内核接触面 | **直接**：`/dev/video*` 的 V4L2 `ioctl` + `mmap`；**间接**：MPP、ZLM、FFmpeg | §5 |
| 已修 P0 | 14 项中 10 项（含 P0-2/3/4/6/7/9/11/12/13/14；P0-5 核实为假阳性） | STATUS §4.1 |
| 剩余 | P0-1（x86 配置路径）、P0-8（MPP 解码越界）、P0-10 ③（`capacity` 语义） | STATUS §4.1 |

---

## 2. 高频问题（20 题 · 一定会被问）

每题给「**30 秒答**」+「**追问往哪走**」。带 ⚠️ 的是**答错的坑**。

### 2.1 为什么用内存池，不直接 `new` 或 `std::vector`？

**答**：视频帧是**固定尺寸、高频、跨线程**的东西。`new`/`delete` 每帧两次（4 路 ×30fps = 120 次/秒）会带来堆碎片和不可控延迟；`vector` 拷贝等于 memcpy 一整帧（6.2 MB）。所以启动时一次性预分配 50 块，运行期只在链表上摘挂，**零分配、零拷贝**，帧内容原地被下一个节点改写。
⚠️ 别说"为了避免内存泄漏"——池子解决的是**延迟与碎片**，泄漏靠 RAII 和所有权约定。

**追问**：池子耗尽怎么办？→ `acquire()` 返回 `nullptr`，`ViNode::run` 里 `continue` 并 `sleep_for(1ms)` 退避（`moudle_vi.cpp:240-244`），是**背压**而不是崩溃。

### 2.2 讲一下池子的数据结构

`MemPool`（进程级单例）里：`free_list_` 空闲块队列 + `queueMap_` 名字到队列的映射 + `all_blocks_`（只读）；锁只保护 `free_list_`（`MemPool.h:137-146`）。块本身 `MemBlock` 带 `owner_name`、`user_cnt`、`user_list`（`MemPool.h:53-65`）。
关键是**块不认识帧格式**：块里挂一个 `MemMeta`，用 `set_ptr(name, ptr)` 把"帧/统计结构/中间结果"挂上去，按名字取（`MemMeta.h:86-136`）。

### 2.3 引用计数是干什么的？现在真的用到了吗？

一帧可能同时被多个下游消费（比如既推流又录像），谁最后释放谁归还。实现：`acquire→addUser`、`push→addUser`、`release→subUser`，`userCnt()==0` 才 `reset()` 回池（`MemPool.cpp:164-213`）。
**诚实回答**：机制完整，但**当前拓扑用不到**——`PipelineBuilder` 只连 `i → i+1`（`pipeline_builder.cpp:156-159`），扇出没有配置语法，所以多下游分支从未被真实执行。这条风险记在 STATUS §4.2 第 24 条。
⚠️ 别吹"我们支持一帧多路分发"——面试官追问"配置怎么写"就露馅。

### 2.4 队列满了怎么办？为什么不阻塞生产者？

`MemQueue::push` 里 `size >= max_cnt` 就 `pull()` 出最老的一块 `release` 掉（`MemPool.cpp:109-121`）。直播场景**保低延迟、丢旧帧**：阻塞会让采集端积压，延迟越滚越大。代价是画面偶发跳帧（DESIGN §11.3 明确写了这个取舍）。

### 2.5 为什么每个节点一个线程？代价是什么？

采集/编码都是**阻塞式**的（`VIDIOC_DQBUF`、`encode_put_frame`），同线程串行会让采集被编码拖住。代价：线程数 = 节点数 × 链路数（4 路 × 3~4 节点 ≈ 11~16 个线程，实测 `Threads=11`），上下文切换与调试成本上升，且**必须自己定义停机顺序**。

### 2.6 一帧从摄像头到播放器的完整路径？

`ViNode::run` 取块（`m_queue.getBlock(100)`，OWNER 队列走 `MemPool::acquire`）→ `read_frame(fb)` 里 `DQBUF`/`sws_scale` 写进块 → 更新 `width/height/stride/timestamp` → `push_input_pipe(block)` 交给下游队列 → `PushNode::run` 取到后 `enc.encode(fb)` → MPP 回调 `sendFrame` → `zlm_rtsp_input_encodeData` → `mk_media_input_h264/265` → ZLM 的 RTSP 服务推给客户端 → 终点 `m_queue.release(block)` 引用归零回池。

### 2.7 NV12 的内存布局？stride 是什么？为什么 128 对齐？

NV12：`Y` 平面 `w×h` 逐行，紧跟一个 `UV` 交错平面 `w×h/2`（每 2×2 块共用一组 UV）。`stride`（行跨距）≥ 宽度，因为硬件/`sws_scale` 按对齐写：本仓库 FFmpeg 侧 `hw_stride=((w+127)/128)*128`（`ffmpeg_capture.cpp:141`），MPP 侧同样算法（`rk_mpp_encoder.cpp:76`）。**三者必须一致**，否则就是花屏/绿边。
⚠️ `FrameBuffer::BindDataPointers` 里 `data[1] = base + stride[0] * height`（`frame_buffer.h:146-163`）—— 这里的 `height` 是**池块的高**，不是本帧的高，这正是 P0-13/P0-14 的引信。

### 2.8 "后半屏绿"和"UV 绿斑"是两个 bug 还是一个？

**两个，根因不同**，这是本项目最能体现深度的题：
- **UV 绿斑（P0-13）**：编码器把 `data[0]` 起 `data_size` 字节**一次性** `memcpy` 进 MPP buffer，但 `data[1]` 的偏移按池块（1920×1080）算，1024×576 的帧拷到的是 Y 平面之后的内存残影 → UV 全是脏数据。修法：Y、UV 各从自己的平面指针拷（`rk_mpp_encoder.cpp:203-216`）。
- **下半屏绿（P0-14）**：两个采集后端共用池块，`FrameBuffer` 视图被**跨链路改写**（另一个后端把自己的 1024×576/stride=1024 写在块上），1080p 那路沿用 `stride=1024` 写 1080 行 → 只能覆盖 1024×1080 字节，剩下的保持旧值。修法：写之前 `BindMemory(data[0], 本链路宽, 高, NV12[,128])` 重绑视图（`v4l2_capture.cpp:202-209`、`ffmpeg_capture.cpp:175-179`），并校验 `sws_scale` 返回行数。

### 2.9 为什么用 MPP 硬编，而解码用 FFmpeg 软解？

编码在 RK3588 上是**硬**的（MPP/VPU），1080p30 四路在能力内且几乎不占 CPU；解码三路 mp4 走 `avcodec_send_packet/receive_frame` 软解，是本项目 CPU 的主要消耗（DESIGN §11.1 估算 2.4–3.6 核）。所以"软解降 720p"是最有效的降载开关。

### 2.10 RTSP 服务是怎么起来的？为什么有"服务/流"两层引用计数？

ZLMediaKit 提供 C API：`mk_rtsp_server_start(port)` 起服务 → `mk_media_create(vhost, app, stream, ...)` 建媒体 → `mk_media_init_video(...)` 声明编码参数 → 每帧 `mk_media_input_h264/265(...)`。
多路共用同一端口时，服务只能起一次、也只能在**最后一个流停止后**停：所以 `RtspServerCtx.ref_cnt` 计数，`ReleaseServerRef()` 是**唯一**递减入口，归零才 `mk_stop_all_server()` 并从 map 注销（`zlm_push_server.cpp:63-78`）。原实现里"重复注册"分支 `ref_cnt++` 后直接 `return`，泄漏一个引用 → 服务永驻（P0-6/B1）。

### 2.11 怎么优雅退出？为什么不能 `kill -9`？

`SIGINT/SIGTERM` → 处理函数只写 `volatile sig_atomic_t g_run = 0`（异步信号安全：不加锁、不打日志、不分配）→ 主循环退出 → `stop_all()` 逐链**正序**停（源头先停）→ `sets.clear()` → `FreeMem()` → `return 0`（`main_node.cpp:95-115`）。`-9` 会跳过这一切：块不归还、`mk_media` 不释放、端口可能还占着，日志里看就是"泄漏"。

### 2.12 为什么"终点先起、源头先停"？

起点先起：源一开始产帧，下游线程已经在队列上等了，否则第一帧要等消费者创建，且队列容易瞬间打满走丢帧逻辑（`pipeline_builder.cpp:217-230` 逆序 start）。
源头先停：掐断入口后，下游把队列里已有的块处理完再退，不会出现"下游停了、块还在队列里没人还"（`stop_all` 正序，`pipeline_builder.cpp:232-241`）。

### 2.13 配置是怎么驱动整条链路的？

`NodeConfigManager`：读文件 → `nlohmann::json` 解析 → **字段级校验**（错误带路径如 `pipelines[0].nodes[1].config.port`）→ 展开 `${global.*}` 整值引用（解析不出就报错，不静默）→ 发布**不可变快照**（校验失败不破坏已生效快照）。v0 `push.json` 由 `ParseV0` 合成单链（`node_config_manager.cpp:590-684`），显式路径不回退。
`main` 只做四件事：加载配置、`MemInit`、`BuildAll`、信号+启停——**入口里没有任何节点名**（`main_node.cpp`）。

### 2.14 新增一个节点要改哪些地方？

① 继承 `PipelineNode` 实现 `start/stop/create/set_data` + 自己的 `run()`；② 在 `node_factory.cpp` 写 `CreateXxx()` 并 `register_type("xxx", ...)`；③ 加进 `pipeline/CMakeLists.txt`；④ 配置里写 `type: xxx`。红线：`getBlock()` 之后**每条失败分支都要 `release`**，`run()` 里 `push_input_pipe` + `release` 成对。

### 2.15 怎么保证没有内存泄漏？凭什么让人信？

三层证据：①**RAII**：`FrameBuffer` 析构 `Release()`、禁拷贝；`MemMeta` 析构 `clear()`、槽位虚析构；②**结构**：块只有"在池里"和"被引用"两种状态，归还唯一入口是引用计数归零；③**测量**：`mallinfo2().uordblks + hblkhd` 口径下 `MemInit+FreeMem` 6 轮恒定 82,080 B（修复前每轮 +12.44 MB），ASAN 探针零泄漏，四路 SIGINT 打印 `FreeMem: 释放 50 块 / 100 个槽位`。

### 2.16 最难查的 bug 是哪个？怎么定位的？

推荐讲 **P0-14（下半屏绿）**，因为它"只在并发+块复用时出现"：先发现**单跑 ch0 时 176 帧全好、四路时 51/64 坏**，由此把"分辨率/素材"假设推翻（同一份代码同一路只差"有没有别人用池子"）；再顺着"块是共享的 → 谁改了块级状态"找到 `FrameBuffer` 视图被另一后端改写；最后用"写前重绑"验证：坏帧 0/166。完整档案见 BUG_CASES §2.7。

### 2.17 4 路 1080p30 在 RK3588 上跑得动吗？怎么估？

块数公式：`路数 × (队列深度 + 2)`（采集写着 1 + 队列 N + 正在编码 1），4×(8+2)=40，取 64 留余量。CPU 主要花在软解（2.4–3.6 核）+ 采集缩放（0.5–1 核）≈ 4–5 核 / 8 核。**现状对不上**：默认 50 块、队列深度 20/路 → 最坏需要 80 块，必须配置化（DESIGN §11.2，T10a 未完成）。

### 2.18 这个项目有没有做零拷贝？到什么程度？

诚实分级：**池内零拷贝**（节点间只传指针，同一块内存被下游原地改写）；**不是全链路零拷贝**——采集侧 `sws_scale` 必须写一次（YUYV→NV12 + 缩放），编码侧还要再 `memcpy` 进 MPP buffer（`rk_mpp_encoder.cpp:203-216`）。真正零拷贝要 DMA-BUF 导入（V4L2 `VIDIOC_EXPBUF` → RGA/MPP 直接消费），这是 ROADMAP 的方向、当前**没做**。

### 2.19 `print` 节点为什么有用？它不就是打日志吗？

它是**终点 + 汇总点**：唯一知道整条链产出速率的地方（fps 按窗口在 `moudle_print.cpp:120-141` 算），同时负责 `release` 让块回池。耗时统计不做全局单例，而是把 `NodeProfiler` 挂在块上（`MemMeta` 槽位 `profiler`），随帧流动、**无锁**（同一时刻块只属于一个节点）。

### 2.20 你这个项目最大的短板是什么？

三条，主动说比被问出来好：①`VpssNode::process_frame()` 还是透传，预处理像素级实现是框架占位（`preprocess/README.md` §6）；②扇出拓扑没有配置语法，引用计数的多下游路径没被验证过；③x86 上构建配置仍指向不存在的 `install/x86_64`（P0-1），只能板端开发。

---

## 3. 核心问题（15 题 · 考的是"你真的想过为什么"）

### 3.1 `getBlock(timeout)` 里 `condition_variable` 的正确姿势是什么？

`wait_for(lock, ms, pred)` 必须带谓词，否则虚假唤醒会让 `user_queue.front()` 炸掉；本仓库写法正确（`MemPool.cpp:61-97`）：
```cpp
if (!_queue_cond.wait_for(lock, std::chrono::milliseconds(timeout),
        [this]{ return this->user_queue.size() > 0 || this->_stopped; }))
  return nullptr;              // 超时
if (user_queue.size() <= 0) return nullptr;   // 被 _stopped 唤醒
```
**扣分点（本项目真实存在）**：参数是 `uint64_t`，函数里却判 `timeout < 0`——死分支，也无法表达"永久等待"（STATUS §4.2 第 23 条）。正确做法：`int64_t` + `-1` 表示永久，或枚举 `Wait{Forever, NoWait, Ms(n)}`。
**追加**：`_stopped` 目前**没有任何地方置位**，所以停机只能靠 100ms 超时轮询 `running_`——能工作，但不优雅（应该 `stop()` 里置位并 `notify_all`）。

### 3.2 `MemQueue::push` 先无锁读 `size` 再加锁，有问题吗？

有 **TOCTOU**：`unsigned size = this->Size()`（无锁读 `queue.size()`）与后面的 `user_queue.push()` 不在同一临界区（`MemPool.cpp:109-121`）。
后果分两级：①统计判断可能偏差一块（丢帧策略多丢/少丢一块）；②`std::queue::size()` 与 `push()` 并发是**数据竞争**，标准上是 UB（实践中多数实现只是读到旧值）。修法：整个 `push` 在同一把锁内完成"判满 + 弹出 + 压入"，参考 STATUS §4.2 第 20 条。

### 3.3 这个项目里有几把锁？会不会死锁？

四类：`MemPool::free_mtx_`（保护 `free_list_`）、`MemBlock::_mutex`（保护 `user_cnt/user_list`）、`MemQueue::user_mutex`（保护队列）、`MemMeta::mtx_`（保护槽位表），另有各节点的 `cfg_mtx_`、`link_mtx_`。
风险点：`MemQueue::release` 持 `user_mutex` 调 `MemPool::release`，后者持 `free_mtx_` 再调 `block->subUser`（内部 `_mutex`）→ **固定顺序 user → free → block**。只要没有反向路径就不死锁；但这是"隐式锁序"，没有文档化，新增代码容易破坏。面试时能说出"我会把它写成显式锁序注释/统一走一个入口"就是加分。

### 3.4 `MemMeta` 每个操作都加锁，热路径不慢吗？

`MemMeta` 的操作只在**帧级切换点**发生（`data<FrameBuffer*>("nv12")` 每节点每帧 1~2 次，30fps × 4 路 ≈ 240 次/秒），不是像素级，锁开销可忽略。真正热的是像素循环和 `memcpy`。
但要注意 `std::unordered_map<std::string, MemSlotBase*>` 用**字符串键**查：每次构造 `std::string` 可能分配。要再优化就改 `enum` 键或 `const char*` + 自定义哈希（`NodeProfiler` 那条路已经用固定槽位名常量）。

### 3.5 `FrameBuffer` 为什么直接**禁拷贝**，而不做引用计数？

因为它**不是所有权对象，是视图**：真正的内存归池块，`FrameBuffer` 只记 `data[]/stride/width/height`。允许拷贝等于允许两个对象各自 `delete[]` 同一块 `owned_mem` → 双释放（P0-10 ①）。
引用计数会引入每次跨节点传递的原子加减，而"共享"这件事在架构上已经由 `MemBlock` 的引用计数承担了——**一层所有权就够，两层必然打架**。

### 3.6 "内存"和"视图"分离，代价是什么？

好处：一块 6.2 MB 的物理内存可以被任意分辨率/格式复用，`VpssNode` 还能按块懒创建输出帧。
代价（P0-14 的教训）：`width/height/stride/fmt` 是**块级可变状态**，谁写谁生效，跨链路共享时上一个人的视图会污染下一个人。工程上的补救是"**每次写入前按本次布局重绑**"，但更彻底的方案是把布局放进 `MemBlock` 的不可变属性或按 `(w,h,fmt)` 分池——我们选了前者，因为改动面最小且能立即可验证。

### 3.7 stride 到底由谁决定？三处不一致会怎样？

三个"权威"：`FrameBuffer::CalcMemoryLayout`（按 `align` 算，`frame_buffer.h:96-143`）、`sws_scale` 的 `dst_linesize`（采集侧传 `frame->stride[]`）、MPP 的 `prep:hor_stride`（`rk_mpp_encoder.cpp:76`，固定 128 对齐）。
不一致的直接后果：**行错位**（画面斜切/绿边）；UV 偏移错则**整片绿**。所以我们的约定是"采集侧把 `stride` 写进 `FrameBuffer`，编码侧只读它"（`mpp_frame_set_hor_stride(frame, fb->stride[0])`，`rk_mpp_encoder.cpp:173`）。

### 3.8 为什么统计用 `steady_clock`，而帧时间戳用 `system_clock`？

统计要的是**单调**（NTP 校时跳变会让耗时变成负数或巨大值），所以 `NodeProfiler::now_us()` 用 `steady_clock`（`node_profiler.h:22-28`）。
而 `timestamp_ms` 要跟**外部世界**对齐（录制文件、RTSP 播放端的墙钟），所以采集里用 `system_clock`（`v4l2_capture.cpp:231-233`）。这是"两种时间语义不要混用"的典型例子。

### 3.9 编码器什么时候 `open()`？为什么用帧的真实尺寸而不是配置值？

`PushNode::run` 里第一帧到达时用 `fb->width/height` 打开编码器（`moudle_push.cpp:246-281`），配置尺寸不一致只**告警一次**（`warned_size_mismatch_`）。
理由：MPP 的 `prep:width/height` 必须是真实输入尺寸，配错就直接报错；而"配置与上游不符"是配置问题，应该让链路继续跑并留下明确日志，而不是崩掉。这也暴露一个架构事实：**只有 V4L2 采集会按配置缩放**，FFmpeg 采集不读该配置（`ffmpeg_capture.cpp:161-163` 里 `sws` 是 `w,h → w,h`，同尺寸）。

### 3.10 停机为什么靠 100ms 超时轮询，而不是 `notify_all`？

因为 `getBlock` 的阻塞在**队列**上是无条件的（没有 `_stopped` 置位），节点自带的 `cv_` 与队列的 `_queue_cond` 也不是同一个条件变量——`PushNode::stop` 里 `cv_.notify_all()` 实际叫不醒队列等待（`moudle_push.cpp:37-49`）。
现行方案：所有 `getBlock` 都传 `100ms` 超时，`stop()` 把 `running_` 置 false 后最多 100ms 内线程自然退出，再 `join()`。**能工作**，但代价是停机延迟与空转唤醒（30fps 下每秒 10 次空唤醒可忽略）。正确写法是把 `_stopped` 接进 `MemQueue`，`stop()` 里置位 + `notify_all`。

### 3.11 节点之间为什么用**裸指针**连，而不是 `shared_ptr`？

`next_nodes_`/`parent_nodes_` 是 `std::list<PipelineNode*>`（`pipeline_node.h:36-37`）：节点生命周期由 `PipelineSet::nodes`（`vector<shared_ptr>`）统一持有，连线只是**引用关系**，用 `shared_ptr` 会形成环（双向链表必然循环引用）。
⚠️ 既然继承 `enable_shared_from_this`，正确姿势是用 `weak_ptr` 表达"不拥有"，并在 `unlink_all()` 里清空；现在用裸指针 + `unlink_all()`，靠"先 `stop_all` 再 `nodes.clear()`"的顺序保证不悬空。

### 3.12 单例为什么是 `static` 局部变量？

`MemPool::getInstance(){ static MemPool instance; return instance; }`（`MemPool.h:108-112`）、`NodeFactory`、`MemManager` 同理。C++11 起局部静态初始化是**线程安全**的（magic statics，编译器生成 `__cxa_guard_acquire`），比双检锁更简洁且无内存序问题。
析构顺序的坑：单例析构发生在 `main` 返回后，若此时还有节点线程在跑就会用已析构的对象——所以我们把"停线程 → `FreeMem`"放在 `main` 里显式做，**不依赖单例析构**（`MemPool::~MemPool` 里注释也写了"保证线程已全部退出"）。

### 3.13 `dynamic_cast` 做槽位类型校验，代价可接受吗？

`MemMeta::data<T>` 每次取都 `dynamic_cast<MemSlot<T>*>`（`MemMeta.h:128-134`）。这是 RTTI 比较（不是遍历继承树的重操作），每帧每节点几次，可接受；收益是**类型写错时返回空 + 一行明确日志**，而不是原来把 `void*` 硬转成 `MemSlot<T>*` 的 UB（P1-10）。
更快的替代：槽位里存 `const std::type_info*` 比较 `typeid(T)`，或干脆用模板化 `SlotKey<T>` 在编译期区分。当前选 `dynamic_cast` 是"先正确，后性能"。

### 3.14 `NodeProfiler` 为什么不加锁？凭什么说它线程安全？

因为**同一时刻一个块只属于一个节点**：块在 `MemQueue` 里交接时经过 `user_mutex`，这构成 happens-before，前一个节点的写对后一个节点可见，不需要 profiler 自己加锁（`node_profiler.h:45-54`）。
边界：如果将来做扇出（同一块同时给两个下游），这个前提就破了——那时要么每下游一份 profiler，要么加锁。这是"无锁设计的隐含前提"，能讲出来就是深度。

### 3.15 构建系统为什么这么分层？`PUBLIC` 传递依赖解决了什么问题？

`common_api`（池+配置+预处理）← `platform_api`（capture/encoder/decoder/rtsp 四个 `option` 子模块）← `pipeline_api` ← 可执行。
`pipeline_api` 声明 `target_link_libraries(... PUBLIC common_api platform_api)`（`pipeline/CMakeLists.txt:34-38`）——`PUBLIC` 让最终可执行自动继承头文件路径与链接库，避免"符号无提供者"（STATUS §4.2 第 1 条曾是这个毛病）。
平台差异靠 `TARGET_PLATFORM_RK3588` 宏 + `#ifdef`（`capture_factory.cpp:5-18`）：V4L2 后端只在 RK 平台编译，x86 下工厂返回 `nullptr`，调用方必须判空（P0-2 就是没判空）。

---

## 4. 深入问题（12 题 · 决定你是"用过"还是"做过"）

### 4.1 为什么"下半屏绿"只在四路并发时出现？

因为坏的不是代码路径，而是**共享状态的时序**：`FrameBuffer` 挂在池块上，`width/height/stride` 是块级字段。单跑 ch0 时，块回到池子前视图没被别人改过，`stride` 恰好等于自己的 1920；四路混跑时，1024×576 的 mp4 链路用完块会把视图改成 `stride=1024` 再归还，1080p 那条链路拿到块后沿用旧视图 → `sws_scale` 按 1024 字节/行写 1080 行 = 只有 1.06 MB 被覆盖，Y 平面剩余部分与上一帧残影混合，下半屏表现为绿/花。
**证据链**（面试就按这个讲）：单跑 176 帧零坏帧 → 四路 51/64 坏帧 → 修复后 0/166。没有这条对照，任何"我觉得是视图问题"的说法都站不住。

### 4.2 为什么 UV 拷错只是"绿"，不是崩溃？

因为越界发生在**块内部**：`capacity = w*h*3`（6.22 MB）远大于 1080p NV12 实际需要的 3.11 MB，`data[0] + data_size` 之后还有 3 MB 的合法映射内存，`memcpy` 读到的是上一帧/其它链路的残留亮度数据，不会触发 SIGSEGV。
这正好说明"**没崩 ≠ 正确**"：`capacity` 的虚高既是缓冲（救了你），也是掩盖（让你查不出来）。真正的解法是"Y/UV 各拷各的"（`rk_mpp_encoder.cpp:203-216`），而不是靠容量兜。

### 4.3 `void*` 类型擦除为什么会导致每轮漏 12.44 MB？

原实现 `unordered_map<string, void*> data_map_` 存 `MemSlot<T>*`（P0-11）。`void*` 的两宗罪：①`delete (void*)` 是 UB（不调析构），所以谁都不敢删 → 槽位里的 `FrameBuffer` 永远不析构；②即使想删也删不干净——`MemSlot<T>::freeObj()` 根本走不到。
修法：`MemSlot` 继承 `MemSlotBase`（**虚析构**），map 存 `MemSlotBase*`，`MemMeta::clear()` 里 `delete item.second` → 虚析构分派到 `MemSlot<T>::~MemSlot` → `slot_free(t)`。再让 `~FrameBuffer()` 调 `Release()`，一句 `delete` 就能把"结构体 + 堆数据"一起还掉。
数字：修复前 `MemInit(2)+FreeMem()` 每轮 +12.44 MB，修复后 6 轮恒定 82,080 B。

### 4.4 为什么"只加析构、不禁拷贝"会双释放？

`FrameBuffer` 的 `owned_mem` 是它自己 `new[]` 的。加了 `~FrameBuffer(){Release();}` 而保留默认拷贝构造时，`FrameBuffer b = a;` 会让 `b.owned_mem == a.owned_mem`，两个对象析构各 `delete[]` 一次 → **double free**（glibc 直接 `free(): double free detected` 并 abort，ASAN 报 heap-use-after-free）。
所以 P0-10 的两条修复必须**同批**：`~FrameBuffer()` + `FrameBuffer(const FrameBuffer&) = delete`、`operator=` 也 `delete`。要共享就传 `FrameBuffer*`——槽位和池块本来就是这么传的。

### 4.5 `capacity = w*h*3` 错在哪？为什么危险？

`CalcMemoryLayout` 无论格式都写 `capacity = w*h*3`（`frame_buffer.h:101`）：1080p NV12 实际 3.11 MB，却声明 6.22 MB。危险有两层：①**越界保护失效**——上游可以"合法地"往 3.11 MB 的布局里写 6 MB（`ffmpeg_capture.cpp:145` 的容量校验就是拿它比的）；②`BindMemory()` 也会重算 `capacity`，把"真实分配大小"覆盖掉，于是校验永远通过。
正确做法：`capacity` 只由 `Allocate()` 写（真实分配字节数），`BindMemory()` 不改它，另外单独记 `plane_size[]`。这就是 P0-10 ③ 还没收口的部分。

### 4.6 为什么看内存不能只看 `VmRSS`？

`VmRSS` 是**已映射且已触页**的部分，池子刚 `new[]` 完如果没写满，RSS 还没上去；`malloc` 的大块（≥128 KB）走 `mmap`，释放时 `munmap`，RSS 会掉。而泄漏检测要的是"**堆上还有多少活着的字节**"，所以用 `mallinfo2()`：`uordblks`（在用块）+ `hblkhd`（mmap 段）才是"分配器账本"。
本项目实测口径就写死了这两个字段，`mem_probe 2 3` 给出 82,080 B 稳态；如果只看 RSS 会得出"没泄漏"的错觉（因为页被换出/复用）。

### 4.7 "6 轮零增长"这个结论怎么才算可信？

四个条件缺一不可：①**同口径**（每轮都 `MemInit(2) + FreeMem()`，不夹带其它分配）；②**绝对值**（82,080 B 是进程基线，不是 0，要能解释它是什么：libstdc++/locale/静态初始化）；③**逐轮 delta**（打印每轮值，而不是只给首尾）；④**对照**（修复前同样方法得到每轮 +12.44 MB，说明方法能看见泄漏）。
再加两条旁证：ASAN 的 leak report 为空；四路 `SIGINT` 后 `FreeMem: 释放 50 块 / 100 个槽位` 与 `MemInit` 的账对得上。**能用"方法本身被验证过"来支撑结论，比数字本身更有说服力。**

### 4.8 扇出没实现，那引用计数代码是"死代码"吗？

部分是。`push_input_pipe` 遍历所有下游、`MemPool::release` 按 `user_cnt` 判断（`pipeline_node.cpp:26-39`、`MemPool.cpp:199-213`）都写好了，但 `PipelineBuilder` 只连 `i→i+1`，所以从未有"一帧两下游"的真实执行。
风险有两面：①**未被验证的代码**在启用时最容易出事（比如两个下游都 `release` 同一块但名字不同，`subUser` 只减一次计数 → 计数失衡）；②现在把它当"已支持"写进简历，是**过度声明**。我的处理是：STATUS 立项、配置语法（`next: []`）与 L2 用例一起做，再宣称支持。

### 4.9 `MemBlock::reset()` 不重置 `user_cnt`，会怎样？

`reset()` 只清 `user_list`、`owner`、profiler（`MemPool.cpp:30-44`）。`user_cnt` 与 `user_list` 就出现**两套账**：计数走 `userAdd/userSub`，列表走 `push_back/remove`。正常路径下二者同步（都减到 0 才回池），但只要出现一次"少减"（漏 `release`），块就永远回不了池——而且**日志上看不出来**，因为 `user_list` 已经清空。
修法：`reset()` 里 `user_cnt = 0` 并加一行断言/日志（若不为 0 说明有人漏还），把"漏还"从静默变成可观测。

### 4.10 `subUser(name)` 为什么"同名减一次却删光所有同名项"？

`userSub()` 只 `--user_cnt`，但 `user_list.remove(name)` 会删掉**全部**同名元素（`MemPool.cpp:23-28`）。若同名 `addUser` 两次（比如同一节点因重试把块挂了两次），一次 `subUser` 就把计数减 1、列表清空 → 计数虚高、列表为空，后续判断全乱。
正确做法两种：`list` 只删第一个（`find` + `erase`），或改用 `unordered_map<string,int>` 记"每名字几次"。这是 P1-22。

### 4.11 `MppDecoder` 里 `memcpy(fb.data, buf_ptr, total_size)` 为什么是越界？

`FrameBuffer::data` 是 `uint8_t *data[3]`——**3 个指针共 24 字节**的数组。`fb.data` 在表达式里退化成 `uint8_t**`，`memcpy` 就往这 24 字节里灌 `total_size`（1080p 约 3.11 MB）→ 立刻栈溢出/踩烂相邻内存（P0-8，STATUS 未修）。
正确写法：`fb.Allocate(w,h,NV12)`（或 `BindMemory` 到池块），然后按平面 `memcpy(fb.data[0], ...)`、`memcpy(fb.data[1], ...)`。
**延伸考点**：`sizeof(data)` vs `sizeof(data[0])`、数组退化、`if (!out_frame->data)` 这种"对数组判空永远为真"的写法（`ffmpeg_capture.cpp:154` 就有一处，属 P2）。

### 4.12 首帧"只开编码器不编码"和 `gop` 没透传，影响多大？

首帧跳过编码（`moudle_push.cpp:283-287` 的 `else` 分支）意味着每路起步多等一帧（33ms），对直播无所谓，但**录制/抓拍**场景会少一帧；STATUS P1-13 建议改成"打开后同帧继续编码"。
`gop` 未透传更要紧：默认 GOP 决定播放器**首个可解码帧**与 seek 粒度，也影响丢包恢复。配置里写了 `gop` 但不生效，属于"配置说谎"，比缺功能更危险——文档已在 STATUS P1-11 标注剩余项。

---

## 5. 这个项目到底"碰没碰内核"？（面试最容易被追问的一块）

### 5.1 一句话结论

**这是纯用户态程序，没有内核模块、没有写驱动；但它直接使用内核 ABI——V4L2 的 `ioctl` + `mmap` 是"用户态直接跟驱动对话"的典型代码；另外三条链路（MPP / ZLMediaKit / FFmpeg）在库内部大量使用内核子系统。**
所以准确的自我定位是：**"做过用户态与内核的交界层"**，而不是"做过内核开发"。这个区分一定要说清楚，否则一追问 `file_operations` 就崩。

| 层级 | 内容 | 在仓库里能看到吗 |
| --- | --- | --- |
| **直接** | `/dev/video*` 的 V4L2 全流程：`open/ioctl/mmap/munmap/close` | ✅ `v4l2_capture.cpp`（全文 276 行） |
| **直接** | 信号（`SIGINT/SIGTERM`）、线程、`usleep`、`clock_gettime`（`steady_clock`） | ✅ `main_node.cpp`、各节点 |
| **间接** | MPP（`/dev/mpp_service` ioctl + DMA 堆分配）、ZLM（epoll/socket）、FFmpeg（read/mmap/pthread） | ❌ 库内部，仓库里只有 C API 调用 |
| **完全没碰** | 内核模块、字符设备驱动、设备树改动、`io_uring`、eBPF、`/dev/mem` | — |

### 5.2 直接碰内核：V4L2 的完整时序（能背下来就赢一半）

`V4l2Capture::open()` → `read_frame()` → `close()` 这条线就是标准 V4L2 流式 IO（`v4l2_capture.cpp`）：

| 步骤 | 代码 | 内核侧发生了什么 |
| --- | --- | --- |
| 打开设备 | `::open("/dev/video0", O_RDWR \| O_NONBLOCK)`（:73） | V4L2 驱动 `open`，返回文件描述符 |
| 设格式 | `VIDIOC_S_FMT`（:89，强制 `V4L2_PIX_FMT_YUYV`） | 驱动校验/协商格式，写入 `fmt.fmt.pix`（**必须回读**，见 P1-4） |
| 设帧率 | `VIDIOC_S_PARM`（:100） | 驱动调整 `timeperframe` |
| 申请缓冲 | `VIDIOC_REQBUFS`，`count=4`，`V4L2_MEMORY_MMAP`（:112） | **驱动**分配 4 块 DMA 缓冲（videobuf2 的 `vb2_dma_sg_alloc` 之类） |
| 查缓冲 | `VIDIOC_QUERYBUF`（:127） | 得到每块的 `length` 与内核偏移 |
| 映射 | `mmap(NULL, length, PROT_READ\|PROT_WRITE, MAP_SHARED, fd, offset)`（:131） | 把 DMA 缓冲映射进进程地址空间，**不拷贝** |
| 入队 | `VIDIOC_QBUF`（:136） | 把空缓冲还给驱动：`vb2` 队列 |
| 开流 | `VIDIOC_STREAMON`（:142） | 启动 DMA，硬件开始往缓冲里写 |
| 取帧 | `VIDIOC_DQBUF`（:176） | 阻塞/非阻塞地取一块"已填满"的缓冲 |
| 复用 | `VIDIOC_QBUF`（:241） | 用完还回驱动，形成**4 缓冲流水** |
| 释放 | `munmap`（:265）+ `::close`（:271） | 解除映射；缓冲由驱动在 `close` 时回收 |

**要点**：`REQBUFS` 之后缓冲归**驱动**所有，只有 `QBUF` 之后驱动才能往里写；`DQBUF` 拿到的缓冲在 `QBUF` 之前**仍然是你的**——所以中间任何失败分支都必须 `QBUF`（本仓库在 `BindMemory` 失败和 `sws_scale` 行数异常两处都补了，`v4l2_capture.cpp:207`、`:223`），否则 4 块缓冲用完之后 `DQBUF` 永远拿不到帧，表现为"采集卡死"。

### 5.3 `mmap` 这一层，面试官最爱问什么

1. **为什么不用 `read()`？** `read()` 会把每帧从内核缓冲**拷一次**到用户缓冲；`mmap` 是把 DMA 缓冲直接映射进进程，零拷贝。补充一个常被忽略的点：`close(fd)` 之后映射**仍然有效**（映射持有 `struct file` 的引用），但**不能再用这个 fd 发 ioctl**（EBADF）——所以工业写法是先 `munmap` 再 `close`（本仓库 `close()` 就是这个顺序）。
2. **`MAP_SHARED` 与 `MAP_PRIVATE` 的区别？** 共享映射才看得见驱动的写入；`MAP_PRIVATE` 是写时复制，用来采视频会拿到永远不变的旧数据。
3. **`mmap` 之后数据什么时候可见？** 对 `MAP_SHARED` + 非一致 DMA 的平台，驱动在 `DQBUF` 前会做 cache 同步（`dma_sync_single_for_cpu`）；用户态**不需要**手动 `msync`/清 cache——但如果你自己做 DMA-BUF 导入给 RGA，cache 一致性责任就转到你的调用路径上。
4. **`DQBUF` 失败为什么不能 `continue` 空转？** 本仓库 `O_NONBLOCK` + 失败 `usleep(2000)`（:179），属于**忙等 + 固定退避**；正确做法是 `poll(fd, POLLIN, timeout)` 睡在驱动的事件上（P1-3 已记录，未做）。这也是"非阻塞 IO 的正确姿势"标准答案。
5. **为什么是 4 块缓冲？** 驱动在填一块、你这在消费一块、队列里还有备用的；太少会丢帧（`DQBUF` 拿不到），太多增加延迟和内存。4 是延迟/吞吐的折中（V4L2 常见 3~8）。

### 5.4 间接碰到的内核子系统（说得出名字就够，别硬编细节）

| 子系统 | 谁在用 | 与本项目的关系 | 面试可讲的点 |
| --- | --- | --- | --- |
| `/dev/mpp_service`（Rockchip VPU） | `librockchip_mpp` | `MppEncoder` / `MppDecoder` 每帧 `encode_put_frame` / `decode_get_frame` | MPP 是**厂商私有 ioctl**：用户态下发寄存器配置，VPU 中断回填，缓冲来自 DMA 堆 |
| DMA 堆（`/dev/dma_heap/*`，老内核是 ION） | `librockchip_mpp` | `mpp_buffer_get()` 一次 6 MB 的物理连续缓冲 | 为什么必须**物理连续 + 对齐**：VPU/RGA 只认物理地址；这也是"零拷贝要 DMA-BUF"的原因 |
| socket / epoll | ZLMediaKit 静态库 | RTSP 服务端收发 RTP | ZLM 内部 epoll 事件循环，我们的 `mk_media_input_h264` 是线程安全的入队，不是直接 `send()` |
| 文件 IO / page cache | FFmpeg（mp4 文件源） | `av_read_frame` 读本地 mp4 | 文件源"想读多快就多快"，所以**必须靠 `ViNode` 的 fps 节流**，否则 3 路文件源会瞬抽干池子（DESIGN §11.3） |
| 调度器（CFS） | 所有节点线程 | 每节点一个线程，阻塞在 `condvar`/`ioctl` 上 | 30fps 是软实时；`sleep_for` 精度约数十 µs~ms，**不能**当作帧同步的硬保证 |
| procfs / sysfs | 运维与测试 | `/proc/<pid>/status`、`/proc/loadavg`、`/sys/class/thermal/*` | 实测阈值表就是读这三个（DESIGN §11.4） |

### 5.5 内核向连环问（10 问 · 每题给"能站住"的答法）

1. **"你们这个项目要不要写驱动？"**
   不要。RK3588 上摄像头走标准 UVC/V4L2 驱动、编码走 MPP、网络走内核协议栈。我们做的是**用户态把这些能力编排起来**；唯一"贴内核"的代码是 V4L2 的 `ioctl + mmap`。
2. **"`ioctl` 和 `read/write` 有什么区别？"**
   `read/write` 是字节流；`ioctl` 是**命令 + 结构体**的控制通道：`VIDIOC_S_FMT`、`VIDIOC_REQBUFS` 都是"带参数的命令"，返回值和结构体回填共同表达结果。V4L2 是典型的 ioctl 型设备。
3. **"为什么 V4L2 要 `QUERYBUF` 再 `mmap`，不能直接 mmap？"**
   得先知道每块缓冲的 `length` 和内核偏移（`buf.m.offset`），`mmap` 的 `offset` 必须是页对齐的缓冲偏移；`QUERYBUF` 就是拿这两个数。
4. **"`munmap` 和 `close` 的顺序有讲究吗？"**
   先 `munmap` 再 `close` 更清晰（本仓库 `close()` 里就是这个顺序，`v4l2_capture.cpp:263-273`）。技术上先 `close` 映射也还在（映射持有 `struct file` 引用），但**再发 ioctl 会 EBADF**，而且驱动的缓冲要等最后一个引用释放才回收。
5. **"非阻塞 `DQBUF` 拿不到帧，返回什么？你怎么区分'没帧'和'真错'？"**
   返回 `-1` 且 `errno == EAGAIN`（`O_NONBLOCK`）；本项目只 `usleep(2000)` 后返回 `false`，**没有区分 errno**——这是可改进点（`poll()` + 区分 `EAGAIN`/`EIO`）。
6. **"`select`/`poll`/`epoll` 怎么选？"**
   一个 fd：`poll` 足够；本项目只有一路 `/dev/video0`，所以 `poll` 就够，`epoll` 是给"成千上万连接"用的（ZLM 内部就是 epoll）。
7. **"设备树（DT）跟你有什么关系？"**
   关系只在"设备节点是谁"：`/dev/video0` 由 DT 里的 camera/CSI 节点 + 驱动 probe 出来。**改 DT 不在本项目范围**，但要知道"换一路 CSI 摄像头可能换的是 `/dev/videoN`"，所以配置里 `vi.src` 是**可配**的（`dev:/dev/video0`）。
8. **"怎么确认摄像头真的在出 YUYV 1080p30？"**
   `v4l2-ctl --list-devices` 找节点、`v4l2-ctl -d /dev/video0 --list-formats-ext` 看支持项、`v4l2-ctl -d /dev/video0 --get-fmt-video` 看当前格式；`dmesg | tail` 看驱动 probe/带宽报错。这些命令在 `docs/COMMANDS.md` §6 有。
9. **"用户态怎么知道自己用的内存有没有泄漏到内核？"**
   用户态泄漏看 `mallinfo2`；内核侧看 `/proc/meminfo` 的 `Slab`/`SUnreclaim` 与 DMA 预留（`/proc/meminfo` 的 `CmaTotal/CmaFree`）。DMA 缓冲泄漏的典型表现是 `CmaFree` 单调下降——MPP `mpp_buffer_put` 漏调就会这样。
10. **"实时性怎么保证？要不要 `SCHED_FIFO`？"**
    当前 30fps（33ms 预算）+ CFS 足够；要更稳就提升采集/编码线程优先级（`pthread_setschedparam`）并绑核（`taskset`），代价是可能饿死其它线程。本项目**没做**，也不建议在没实测前做。

### 5.6 能主动说的加分点（面试官会眼前一亮）

- "我们的池块**本身就是 MPP 的 DMA 缓冲**（`mpp_buffer_get` 从 DRM 堆分配，`common/frame_mem.*` 是唯一分配边界），所以硬件模块能直接 import：`mpp_buffer_get_fd()` → RGA `importbuffer_fd()` → `wrapbuffer_handle()`，缩放 / 格式转换 / 裁剪 / 粘贴都实测通过，**既不需要物理地址，也不需要 memcpy**。剩下的零拷贝两处还没做：编码器侧 `mpp_frame_set_buffer` 直接吃池块（省掉 4 路 1080p 约 360 MB/s 的整帧拷贝），采集侧 `VIDIOC_EXPBUF` 导出的 DMA-BUF 直接灌池——这是 ROADMAP 明确项。"
- "踩过 RGA 的硬坑：**RGA3 不接受宽度 < 68 的矩形**（64、66 都被驱动拒成 `RGA_BLIT fail: Invalid argument`，68 起才过，高度没这限制）；另外 `imblend()` 没有 rect 参数，会按整块 dst 算缩放比，64×32 贴到 720p 上直接报 `scaling more than 1/16 ~ 16 times`，必须用 `improcess()` 显式给 `dst_rect`。"
- "板端 `/usr/local/include/rockchip/` 下那 17 个 RGA 头是 **0 字节空壳**（只有 MPP 头正常），`find_path` 照样命中并误判 `RGA_FOUND`——所以我们的 CMake 必须校验头文件非空或试编。"
- "V4L2 的非阻塞 `DQBUF` 失败我只做了 `usleep(2ms)` 退避，**这是已知缺陷**（STATUS P1-3）；正确的是 `poll()`，因为它能区分超时与错误，也不会有固定延迟。"
- "`VIDIOC_S_FMT` 之后我没有回读 `fmt.fmt.pix.width/height`——驱动可能协商成别的分辨率（P1-4）。这在 UVC 摄像头上很常见，是典型的'用户态假设 ≠ 硬件实际'。"

### 5.7 不能吹的边界（说错就掉分）

| 别说 | 因为 |
| --- | --- |
| "我写过 V4L2 驱动 / 字符设备" | 仓库里没有内核模块，`file_operations`、`vb2_ops` 都没出现 |
| "我改了设备树" | 没有 `.dts` 文件 |
| "我们做了 DMA-BUF 零拷贝" | 编码前是显式 `memcpy`（`rk_mpp_encoder.cpp:203-216`） |
| "MPP 的 ioctl 细节我很熟" | 仓库只调 C API；`/dev/mpp_service` 的 ioctl 编号、寄存器布局都在库里 |
| "用了 io_uring / eBPF" | 一处都没有 |

---

## 6. 项目里能练 C 语言熟练度的 8 个落点

这个项目主体是 C++14，但**"C 的那一半"恰恰是含金量最高的部分**：系统调用、字节布局、裸指针、C ABI。下面每个落点都标了"练什么 / 怎么练 / 面试怎么问"。

### 6.1 `v4l2_capture.cpp` —— 系统调用层（最纯粹的 C）

- **C 概念**：`open/ioctl/mmap/munmap/close`、`errno` + `perror`、`struct` 零初始化（`memset(&fmt, 0, sizeof(fmt))`）、`O_NONBLOCK` 位掩码、`uint8_t*` 与 `void*` 互转。
- **现成代码**：`parse_src_param`（:21-46）解析 `设备:宽:高:格式:fps` 五段；`read_frame` 里 `(uint8_t *)m_buffers[buf.index].start`（:190）。
- **怎么练**：
  1. 把 `usleep(2000)` 换成 `poll(fd, POLLIN, 100)` 并按 `errno`（`EAGAIN`/`EIO`/`ENODEV`）分支处理 —— 顺手修掉 P1-3；
  2. `close()` 里 `if (m_fd > 0)` 改成 `>= 0`（**fd 可以是 0**，这是典型 C 细节 bug）；
  3. 加 `v4l2-ctl` 对照实验：`--get-fmt-video` 验证 S_FMT 后是否被驱动改写。
- **面试问法**："为什么 `memset` 结构体？不 memset 会怎样？"（未初始化字段被驱动当输入参数 → `VIDIOC_S_FMT` 的 `field`/`bytesperline` 垃圾值）。

### 6.2 `frame_buffer.h` —— 指针算术与字节布局

- **C 概念**：多维数据的**一维化**（`data[0]` + `stride` 表示二维）、对齐向上取整 `(w+align-1)/align*align`、`size_t` 与 `int` 混算、数组 vs 指针。
- **现成代码**：`CalcMemoryLayout`（:96-143）、`BindDataPointers`（:146-163）。
- **怎么练**：
  1. 用纯 C 写一个 `struct fb { uint8_t *data[3]; size_t stride[3]; int w, h; }` + 同样的 `layout/bind` 函数，再用 `mprotect(PROT_NONE)` 做"越界必崩"的测试；
  2. 修 `FMT_YUV422` 的第三平面偏移（`frame_converter.cpp:43-47` 已注明**已知不一致**：用的是 420 的算法）——这是"一个 assert + 20 行测试"就能发现的问题。
- **面试问法**："`data[1] = base + stride[0]*height` 里 `height` 应该是谁的？"（答：必须是**这块内存的**高度，不是本帧的；这正是 P0-13/P0-14 的分水岭）。

### 6.3 `rk_mpp_encoder.cpp` —— `memcpy` 与 `size_t` 算术

- **C 概念**：按平面拷贝、`size_t` 乘除的**溢出与整除截断**、`const` 正确性、`mpp_buffer_get/put` 的配对（C 资源管理）。
- **现成代码**：`frame_size/3*2` 求 Y 大小（:211-214）；`fallback_stride` 兜底（:191-195）。
- **怎么练**：
  1. 把 `frame_size/3*2` 换成显式 `y_size = stride*h; uv_size = stride*(h/2)`，并用 `assert(y_size + uv_size == data_size)` 自证；
  2. 给 `memcpy` 前加"目标容量 ≥ 源长度"的检查（现在只检查了 `data[0]/data[1]` 非空，:203）。
- **面试问法**："`/3*2` 和 `*2/3` 有什么区别？"（整除顺序不同，1080p 下前者恰好对，但换分辨率就可能少 1 字节 → UV 偏移错 → 绿边）。

### 6.4 `zlm_push_server.cpp` —— C ABI 与不透明句柄

- **C 概念**：`extern "C"`、`void*` 作为**不透明句柄**、函数指针 typedef 作回调、`static` 文件作用域全局变量 + 锁。
- **现成代码**：句柄 `ZlmServerStreamHandle`（`zlm_push_server.h`）、回调 `ZlmOutputCb`、`static std::unordered_map` 两张表 + `g_mtx`。
- **怎么练**：
  1. 用纯 C 写一个"注册/注销句柄表"的小模块（`create/find/destroy` + 引用计数），练**句柄失效检测**——本项目的 `zlm_rtsp_input_encodeData` 就是"先查表再解引用"，而不是直接 `static_cast`（:195-205），这就是防 use-after-free 的 C 惯用法；
  2. 把 `ref_cnt` 的"唯一递减入口"写成 C 函数并加断言。
- **面试问法**："为什么句柄不能直接强转回来用？"（客户端可能传已释放/伪造的句柄 → 必须先在表里查存在性）。

### 6.5 `MemMeta.h` —— `void*` 的诱惑与代价

- **C 概念**：`void*` 泛型容器（C 里没有模板）；`delete` 一个 `void*` 是 UB 且不调析构。
- **现成代码**：`slot_free<T>` 特化、`MemSlot<T>` 继承 `MemSlotBase`、map 从 `void*` 改成基类指针（`MemMeta.h:27-37`、`176`）。
- **怎么练**：用纯 C 实现同样功能——`struct slot { void (*free_fn)(void*); void *obj; }`，靠**函数指针存析构**（这是 C 里做"类型擦除 + 自动释放"的标准手法，也是内核里 `kfree`/`release` 回调的思路）。
- **面试问法**："C 里怎么实现 RAII？"（答：`goto cleanup` 统一出口 + 每个资源一条 `free_fn`；或 `__attribute__((cleanup))`）。

### 6.6 `node_profiler.h` —— 定长数组与字符串截断

- **C 概念**：`char name[32]` 定长、`strncpy` + 手工补 `'\0'`（:95-96）、`static const int` 作为编译期常量、结构体数组顺序扫描。
- **怎么练**：
  1. 把 `begin/end` 的线性扫描（最多 8 项）改成"插入时记 `index`"，顺便练"**用返回值表达失败**"的 C 风格；
  2. 加一个 `report()` 的单测，覆盖"节点数超过 `kMaxStages`"的溢出分支。
- **面试问法**："`strncpy` 有什么坑？"（源长度 ≥ n 时不补 `'\0'`；本项目手工补了，很多人会漏）。

### 6.7 `preprocess/frame_converter.cpp` —— 像素级循环（最能练出"手写 C"的手感）

- **C 概念**：逐行 `memcpy`、嵌套循环的下标计算、`size_t`/`int` 混用、边界检查（`check_pair`）、三平面/两平面布局差异。
- **现成代码**：`calc_layout`（:49-109）、`check_pair`（:114-121）、同格式深拷贝（:151-153 的 `rows = plane_size / stride`）。
- **怎么练**（按难度递增）：
  1. 实现 NV12 → YUV420P（UV 解交错），要求"只读一行 UV 写两行 U/V"；
  2. 实现 YUV420P → BGR888，练 BT.601/709 定点数转换（`(y-16)*298 >> 8` 这类，避免浮点）；
  3. 加"越界哨兵"测试：分配时多给 64 字节填 `0xCD`，转换后检查哨兵未被改写。
- **面试问法**："怎么写一个不会越界的像素循环？"（答：先算 `plane_size`，用 `stride` 走行，`assert` + 哨兵测试，不信任上游给的 `width/height`）。

### 6.8 `main_node.cpp` —— 异步信号安全

- **C 概念**：`volatile sig_atomic_t`（:32-38）、信号处理函数里**只能**做异步信号安全操作、`std::signal` vs `sigaction`。
- **怎么练**：换成 `sigaction` + `SA_RESTART` 语义验证；在信号处理里**不要** `printf`/`malloc`/加锁（现在没做，是对的），改为写 `eventfd`/自管道唤醒主循环——这是"信号 + IO 多路复用"的标准工业写法。
- **面试问法**："信号处理函数里为什么不能 `printf`？"（`printf` 内部有锁和缓冲区，可能重入死锁/破坏状态；`write()` 到 `eventfd` 是安全的）。

### 6.9 明确标注：项目里**没有**、但值得单独练的 C 主题

| 主题 | 项目现状 | 建议练习 |
| --- | --- | --- |
| 位域 / `union` | 无 | 手写 RTP 头（12 字节，V/P/X/CC/M/PT/seq）解析与打包 |
| 大小端 | 无（RTSP 由 ZLM 处理） | 手写 `htonl`/`ntohl` 与 NAL 长度前缀（AVCC ↔ Annex-B） |
| `__attribute__((packed))` | 无 | 定义一个"线上格式"结构体并验证 `sizeof` |
| `<stdatomic.h>` | 用的是 C++ `std::atomic` | 用 C11 原子实现无锁单生产者单消费者队列 |
| `container_of` | 无 | 手写一遍，理解"成员指针反推结构体"（内核链表核心手法） |
| `goto cleanup` | 无（用 C++ RAII） | 把一个多资源初始化的 C 函数改写成单出口 |

## 7. 准备路线（4 周 · 每周都能产出可讲的东西）

| 周 | 读什么 | 动手做什么 | 产出（面试可讲） |
| --- | --- | --- | --- |
| 1 | `frame_buffer.h` + `MemManager/MemPool` 全部 + HANDOFF §1.7 | 用纯 C 重写 `layout/bind`，加哨兵越界测试 | "我能手写多平面布局 + 证明不越界" |
| 2 | `v4l2_capture.cpp` + `main_node.cpp` | `poll()` 替换 `usleep`；`sigaction` + `eventfd` 改造退出 | "我改过采集的非阻塞/信号路径，能说清 errno 与异步信号安全" |
| 3 | `rk_mpp_encoder.cpp` + `zlm_push_server.cpp` | 平面拷贝改成显式 `y_size/uv_size` + `assert`；写一个 C 版句柄表 | "我能在 C ABI 边界上做资源与生命周期管理" |
| 4 | `BUG_CASES.md` 全部 + `TESTLOG.md` §5 | 按 §4 的 12 个问题各写 5 行口述稿，录音回听 | "每个坑我都能给出证据链，而不是感觉" |

读的顺序很重要：**先内存池（数据所有权）→ 再采集（内核边界）→ 再编码/推流（C ABI）→ 最后 bug 档案（工程判断）**。反过来读会在 `MemMeta` 的类型擦除上卡住。

---

## 8. 收尾三件事

### 8.1 反问面试官（问这些显得你真做过）

1. "你们这边视频链路是**单进程多线程**还是多进程？多路之间怎么隔离故障？"（对应本项目"单链失败不拖垮其它链"，但仍是同进程）
2. "内存是**池化 + 引用计数**还是每帧 malloc？有没有做 DMA-BUF 零拷贝？"
3. "从采集到出流的延迟目标是多少毫秒？靠什么手段保证（丢帧策略 / 队列深度 / 优先级）？"
4. "线上出问题是靠日志还是指标？有没有类似我们 `NodeProfiler` 那种**逐帧分段耗时**？"
5. "这个岗位需要我补的是内核驱动方向，还是媒体/流媒体方向？"（把边界问清楚，避免入职错配）

### 8.2 诚实清单（被问到"还有什么没做完"就照这张表说）

| 未完成/缺陷 | 一句话说法 | 依据 |
| --- | --- | --- |
| `VpssNode::process_frame()` 透传 | "节点框架通了，像素级处理还是占位，所以我只讲框架不讲算法" | `moudle_vpss.cpp:386-429` |
| preprocess 像素实现占位 | "布局计算、字库解析、配置校验已验证；像素结果未验证" | `preprocess/README.md` §6 |
| 扇出未实现 | "引用计数机制在，但只有单链拓扑，多下游没验证过" | STATUS §4.2 #24 |
| `MppDecoder` 越界 | "解码路径没接进主链路，有个已知越界（P0-8）还没修" | `rk_mpp_decoder.cpp:104-106` |
| x86 构建失败 | "只支持板端 aarch64 开发，x86 配置路径是 P0-1" | STATUS §4.1 #1 |
| `capacity` 语义 | "容量声明虚高，>1080p 时失去越界保护" | STATUS §4.1 #10 ③ |
| `gop` 未透传 | "配置里有但没生效，属于配置说谎" | STATUS §4.2 #11 |
| 池块数/队列深度未配置化 | "默认 50 块 / 队列 20，4 路最坏要 80 块，T10a 才能配" | DESIGN §11.2 |

### 8.3 简历怎么写（三条 bullet，全部可量化可追问）

- 设计并实现**固定块内存池 + 帧视图**模型（50 块 × 1080p NV12，块级引用计数、有界队列 drop-oldest），支撑 4 路并发 30fps；用 `mallinfo2` 6 轮 `MemInit/FreeMem` 证明**零增长**（修复前每轮泄漏 12.44 MB）。
- 直连 V4L2（`ioctl` + 4 缓冲 `mmap` 流式 IO）与 FFmpeg 双采集后端，按配置前缀选路；修复**跨链路共享视图**导致的"1080p 半屏绿"（坏帧 51/64 → 0/166）。
- 把硬编码装配重构为**配置驱动**（JSON → 校验 → 工厂 → 构建器），支持多通道与单链失败隔离；实现 `SIGINT` 有序退出（源头先停 → `FreeMem` → `EXIT=0`）。

---

## 9. 速查表（问题 → 代码位置 → 档案）

| 主题 | 代码位置 | 深入档案 |
| --- | --- | --- |
| 内存池申请/归还 | `MemPool.cpp:164-213` | BUG_CASES §2.3（B3 漏 release） |
| 有界队列 drop-oldest | `MemPool.cpp:109-121` | DESIGN §11.3 |
| 槽位类型擦除 | `MemMeta.h:86-176` | BUG_CASES §2.11（B11） |
| `FrameBuffer` RAII | `frame_buffer.h:23-75` | BUG_CASES §2.10（B10） |
| NV12 平面拷贝 | `rk_mpp_encoder.cpp:203-216` | BUG_CASES §2.6（B6） |
| 采集写前重绑视图 | `v4l2_capture.cpp:202-209`、`ffmpeg_capture.cpp:175-179` | BUG_CASES §2.7（B7） |
| V4L2 全流程 | `v4l2_capture.cpp:61-275` | 本文 §5.2 |
| 采集后端选择 | `moudle_vi.cpp:17-78` | BUG_CASES §2.5（B5） |
| RTSP 引用计数 | `zlm_push_server.cpp:45-78` | BUG_CASES §2.1（B1） |
| 配置校验 + v0 适配 | `node_config_manager.cpp:104-172`、`:590-684` | BUG_CASES §2.9（B9） |
| 拓扑规则 R1~R5 | `pipeline_builder.cpp:40-96` | DESIGN §5.4 |
| 起停顺序 | `pipeline_builder.cpp:217-241` | BUG_CASES §2.8（B8） |
| 有序退出 | `main_node.cpp:95-115` | TESTLOG §5.9 |
| 逐帧耗时 | `node_profiler.h:55-193` | TESTLOG §5.11 |
| 未修的越界 | `rk_mpp_decoder.cpp:41-121` | 本文 §4.11 |

> 本文只做"怎么讲"。要**证据**去 `docs/TESTLOG.md`，要**原理长文**去 `docs/BUG_CASES.md`，要**命令**去 `docs/COMMANDS.md`。

