# BUG 档案（原理剖析 · 面试要点）

> **这份文档解决什么问题**：`TESTLOG.md` 记的是「跑过什么、结果如何」，`STATUS.md` 记的是「现在还有什么问题」。
> 两边都不回答「这个 bug **为什么**会发生、**怎么**一眼看出来、**下次**遇到同类怎么处理」。
> 本文专门收录**值得拿去面试讲**的缺陷：根因原理、精确代码位置、定位过程、以及可复用的排查手法。
>
> **记录规则（强制，用户 2026-11-09 提出）**：从本文建立之后，**每次修掉一个"有原理含量"的 bug 都要在本文追加一个案例**，
> 与代码同一批提交。判断标准：满足任一条即需记录 —— ① 现象与根因不在同一层（如画面坏在采集侧）
> ② 只在特定并发/时序/内存残留下复现 ③ 涉及资源所有权、生命周期、跨组件共享状态 ④ 曾被误判过方向。
> 纯笔误 / 拼写 / 一行空指针且无连带语义的，只在 `STATUS.md` 标注即可，不进本文。

## 1. 索引

| # | 症状（一句话） | 根因（一句话） | 提交 | 详见 |
| --- | --- | --- | --- | --- |
| B1 | RTSP 停止后 `servers` 永远回不到 0，同名流再注册报 `STREAM_EXIST` | 注册与注销**各自拼 key**，且 `bind_ip` 从未赋值；`ref_cnt++` 在判重**之前** | `87127e0`（T1） | §2.1 |
| B2 | 传 `out_code = nullptr` 必崩 | 「先判空、紧接着又写 `*out_code`」——判空写在了使用之后 | `87127e0`（T1） | §2.2 |
| B3 | 跑几十帧后整条链路**静默停摆**（不崩、不报错、报表还在跑） | 失败分支漏 `release()`，池块被逐帧抽干 → `acquire()` 永久阻塞 | `d158eca` / `283bab5` | §2.3 |
| B4 | 节点线程根本不起，链路静默堵死 | `std::atomic<bool> running_` **未初始化**（C++14 默认构造不写值），脏值为 `true` 使 `start()` 直接早退 | `9678e42` | §2.4 |
| B5 | x86 / 无该后端时启动即段错误 | 工厂返回 `nullptr` 未判空就 `open()`；根子是采集**类型与设备路径双硬编码** | `eac4bb0`（T7） | §2.5 |
| B6 | mp4 路**Y 正常、UV 大片绿斑/花屏** | `memcpy(data[0], data_size)` 一次性拷，而 NV12 的 `data[1]` 偏移按**池块**分辨率算 → UV 段拷到的是内存残影 | `c557d89`（P0-13） | §2.6 |
| B7 | 四路并发时 1080p 路**下半屏约 47% 全绿**、上半屏正常，间歇出现 | 池块上的 `FrameBuffer` 视图被**别的链路改写**，采集端沿用过期 `stride` 开 `sws_scale` | `eee3b01`（P0-14） | §2.7 |
| B8 | Ctrl+C 后不退出、`stop()` 永不执行 | 停止逻辑前有 `while(1)` 永久阻塞 | `29ab58b`（T6） | §2.8 |
| B9 | 播放端**加速/抖动**，或小分辨率素材花屏 | 配置的 `fps` / 宽高与素材真实值不符：`PushNode` 用 `1000000/fps` 推 PTS，`FfmpegCapture` 不缩放 | T3 | §2.9 |
| B10 | 每次 `MemInit`/`FreeMem` 都整块漏 12.44 MB，日志全绿、退出码 0 | `FrameBuffer` 注释写「不拥有内存」，`Allocate()` 却在 `new[]`；`FreeMem()` 只删 `MemMeta` → 6.22 MB/块无人认领。**同日收口**：`~FrameBuffer(){Release();}` + 删拷贝（RAII），漏放/双放两条路径一起关 | 工作树（2026-11-10，P0-9 → P0-10 ①②） | §2.10 |
| B11 | 反复启停仍每轮漏 128 B，且 vpss 自建的 `vpss_out` 帧整块 6.22 MB 永不回收 | 容器用 `void*` 存槽位：`void*` 不能安全 `delete`（UB、不调析构）也调不到 `freeObj()` → 槽位只增不减、`FreeMem()` 只能按 `imgMap` 点名 | 工作树（2026-11-10，P0-11） | §2.11 |
| B12 | 崩溃报告写得好好的，**一个 core 都不产生**（`ulimit -c unlimited`、`core_pattern` 都正常） | 信号 handler 执行期间内核自动屏蔽当前信号（无 `SA_NODEFER`）：`tgkill` 只把信号挂成 pending，随后 `_exit()` 把 pending 一起丢掉 → 内核 `do_coredump()` 从未被调用 | 工作树（2026-11-14，崩溃采集） | §2.12 |
| B13 | 崩溃报告里的帧换算出 `?? ??:0`（地址明明在二进制范围内） | `backtrace_symbols` 的 `模块(符号+0x偏移)` 是**符号内偏移**，直接喂 `addr2line` 等于把「符号内」当「模块内」；只有 `模块(+0x偏移)` 才是模块内偏移 | 工作树（2026-11-14，symbolize 脚本） | §2.13 |

**读法建议**：面试前只看 §1 索引 + 每案末尾的「面试怎么讲」；要复盘细节再读中间的定位过程。

---

## 2.1 B1 · RTSP 停止路径静默失效 + 服务引用计数泄漏（T1）

**现象**：`stop` 之后 `zlm_rtsp_server_count()` 仍是 1（`streams=0 / servers=1`）；同一 `app/stream` 再注册永远返回 `ZLM_ERR_STREAM_EXIST`，端口也不释放。**全程没有任何错误日志、没有崩溃**。

**位置**：`src/platform/rtsp/zlm_push_server.cpp` — `zlm_rtsp_onekey_start()` / `zlm_rtsp_stop()` / `ReleaseServerRef()`。

**根因（两处，同源于"多处各自拼字符串 / 计数不配对"）**

1. 注销用的流 key 由 `bind_ip + ":" + port` 拼成，而 `RtspServerCtx::bind_ip` **从未被赋过值**（空串）→ 注销时算出的 key 是 `":8550"`，注册时存的是 `"8550"`。
   `g_stream_map.erase(":8550")` 什么也没删，`mk_media_release()` 也没执行 → **停止路径整体静默跳过**。
2. 重复注册分支里 `ref_cnt++` 写在判重**之后**（先加引用再返回 `STREAM_EXIST`），每重复注册一次就漏一个引用；最后一个流 stop 时 `ref_cnt` 归不到 0 → `mk_stop_all_server()` 永不执行 → server 永久滞留。

**修复要点**
- key 的**唯一生成入口**：`ServerKey(port)` / `StreamKey(port, app, stream)`；ctx 里**存下**注册时用的 key，注销直接读，不再拼第二次。
- `ref_cnt` 的**唯一释放点**：`ReleaseServerRef()`，三条路径共用（正常 stop / 重复注册回滚 / `mk_media_create` 失败）。
- 句柄先**在表内定位**再解引用（`zlm_rtsp_stop`、`zlm_rtsp_input_encodeData`）：stop 真正生效后 ctx 已释放，直接 `static_cast` 就是 UAF。
- 新增只读自检 `zlm_rtsp_stream_count()` / `zlm_rtsp_server_count()` —— 否则「stop 后两表为空」这条验收**无法被断言**。

**为什么难发现 / 踩过的坑**
- 失效是「什么都没发生」型的：没有错误码、没有日志，资源滞留只在**计数指标**上可见。
- **端口是否在监听不能当判据**：ZLM 的监听 socket 由 `TcpServer` 析构在 poller 线程里关闭，`stop` 返回瞬间端口可能仍被占用（实测同端口立刻重启会 `bind: address already in use`，脚本被迫做 ≤3s 退避重试）。
- 修好 A 才暴露 B：key 修对后 stop **真的**会销毁 ctx，原来的「对已停句柄再 stop / 再送帧」立刻变成 UAF —— 这类「修一个洞露出下一个洞」是资源生命周期问题最常见的形态。

**面试怎么讲**：一句话 —— *"我把'同一个键在两处各自生成'改成了单一生成入口，并给引用计数补上唯一释放点，包括失败回滚路径。"* 可延伸：单点真理（single source of truth）、RAII/引用计数必须覆盖**所有**分支（含失败回滚）、以及「没有报错的 bug 要靠可观测指标去断言，先补断言接口再修」。

---


## 2.2 B2 · 出参判空顺序错误（T1）

**现象**：调用方传 `out_code = nullptr`（只想拿句柄、不想看错误码）时进程直接段错误。

**位置**：`zlm_push_server.cpp::zlm_rtsp_onekey_start()` 开头。

**根因**：原实现形如

```cpp
ZlmServerStreamHandle h = nullptr;
if (!out_code) h = nullptr;      // “判了空”，但没有 return
...
*out_code = ZLM_ERR_OK;          // 仍然解引用
```

判空存在，但**没有形成控制流上的早退**，于是「判空」只是装饰。

**修复**：`out_code` 是**唯一**回传错误码的出口 ⇒ 必须最先判空并**立即 return**；其余入参（`app/stream` 空串、`w/h/fps <= 0`）统一返回 `ZLM_ERR_INVALID_PARAM`。

**面试怎么讲**：*"出参比入参更容易被忘——它是唯一出口，所以要在函数第一行就守。"*

---

## 2.3 B3 · 失败分支漏 `release()`：内存池被逐帧抽干（P0-3 / P0-4）

**现象**：跑十几到几十帧后，整条链路**完全静默** —— 没有崩溃、没有错误日志，报表可能还在打印，但再也没有新帧；`Ctrl+C` 之后还能优雅退出（说明线程没死，只是**卡在等块**上）。

**位置**
- `src/pipeline/moudle_vi/moudle_vi.cpp::ViNode::run()` —— `read_frame()` 返回 `false` 时 `continue`（未归还）；T7 又补了 `open()` 失败分支。
- `src/pipeline/moudle_push/moudle_push.cpp::PushNode::run()` —— `enc.open()` 失败时 `continue`（未归还）。

**根因（原理）**
`MemPool` 是**容量固定的池**（默认 50 块），`acquire()` 在无空闲块时**阻塞等待**，不是返回 `nullptr`。所以漏一次 `release()` 就等于**永久少一块**：

```
每帧失败 1 次 → 池块 −1 → 50 帧后 acquire() 永远等不到 → 节点线程挂起 → 下游队列永久空 → 静默停摆
```

触发条件很常见：**配置的 fps 高于采集端实际帧率**（`DQBUF` 拿不到新帧直接返回 false）、编码器打不开、队列满丢块。

**修复**：`getBlock()` 之后**所有**失败分支（读帧失败 / 编码打开失败 / 队列满 / 异常返回）都必须 `m_queue.release(block)`，并在 `continue`/`break` 前把 profiler 的 `end()` 补上（否则耗时统计里该节点永远显示 running）。这条已写成红线：**取块必有归还**。

**为什么这类 bug 很"贵"**
- 症状（静默停摆）与根因（漏一行 release）距离极远，且**不会自己报错**；
- 与 B4 叠加时更难分：一个是"线程没起"，一个是"线程起了但饿死"；
- 长跑才暴露，短跑 5 秒完全正常 —— 验证必须覆盖**稳态**而不只是"能出帧"。

**面试怎么讲**：*"固定容量池 + 阻塞式 acquire，把'漏归还'从内存泄漏变成了'死锁'——内存泄漏还能跑，这个是几十帧后整链停摆。"* 延伸：阻塞式资源获取要配「失败路径统一归还」的编码约定或 RAII guard。

---

## 2.4 B4 · `std::atomic<bool>` 未初始化导致线程起不来（P0-12）

**现象**：链路构建成功、日志打出 `Started`，但**没有一帧数据**，且无任何报错（同一份二进制有时正常、有时不正常）。

**位置**：`moudle_vi.cpp::ViNode::ViNode()`、`moudle_push.cpp::PushNode::PushNode()`。

**根因（原理）**
`std::atomic<bool> running_;` 在 **C++14** 下默认构造**不会**把底层值置为 `false`（C++20 起才 value-initialize）。`start()` 是

```cpp
if (running_.exchange(true)) return;   // 脏值恰好为 true → 直接 return
thread_ = std::thread(&ViNode::run, this);
```

内存里残留 `true` ⇒ `exchange` 返回 `true` ⇒ `start()` **早退，线程根本没创建**。

**修复**：构造函数初始化列表显式 `running_(false)`；不要依赖"堆对象会被清零"的假设（`make_shared` 出来的是未清零的堆内存）。

**面试怎么讲**：*"`std::atomic` 的默认构造在 C++14 与 C++20 语义不同——C++14 是未初始化，C++20 才给零值。"* 延伸：静态存储期与动态分配的区别；为什么这类 bug 偶发且难复现。

---

## 2.5 B5 · 工厂返回 `nullptr` 未判空 + 采集类型硬编码（P0-2 / T7）

**现象**：x86（或未编译 V4L2 后端的平台）启动后**必现段错误**；板端四路配置下 ch1~ch3 全部 `open file failed:/dev/video0...`。

**位置**：`moudle_vi.cpp::run()`。

**根因（两层）**
1. 直接 `CreateCapture(CAPTURE_V4L2)`，而 `capture_factory.cpp` 用 `TARGET_PLATFORM_RK3588` 把 V4L2 后端包住了 ⇒ x86 下工厂**返回 `nullptr`**，紧接着 `m_capture->open(m_src)` 就是空指针解引用。
2. 更底层：节点里**双硬编码** —— 采集类型写死 `CAPTURE_V4L2`、地址写死 `/dev/video0:640:480:NV12:30`，于是"几条链路 = 几个同样的摄像头"这种不可能配置也能被写出来。

**修复**
- `vi.src` 前缀解析（`ResolveSrc()`）：`dev:` → V4L2；`file:` / `rtsp://` / `rtmp://` / `http://` / `udp://` → FFmpeg；无前缀按老规则兜底（`/dev/video*` → V4L2，其余 → 文件）。`dev:` 只写设备路径时补成 V4L2 契约的 5 段 `设备:宽:高:格式:fps`；写一半（`dev:/dev/video0:640`）**直接报错不猜**。
- `run()` 改 `CreateCapture(m_capture_type)`，**判空后报错并退出本节点线程** —— 满足失败隔离（一条链起不来不拖垮其余链），而不是让整个进程崩。

**面试怎么讲**：*"工厂的可空返回值必须在调用点处理；同时我把'用哪个后端'从节点代码搬到配置里 —— 前缀即契约。"* 延伸：解析型配置宁可严格契约 + 「写一半就报错」，也不要"猜默认值"，静默降级会把问题推到下游。

---

## 2.6 B6 · NV12 的 UV 平面被一次性 `memcpy` 拷错（P0-13，T7 暴露）

**现象**：T7 打通四路（ch0 = `dev:` 1920×1080、ch1~ch3 = `file:` 1024×576）后，**所有小分辨率路**画面 Y 正常、UV 出现大片绿斑/花屏；1080p 路正常。

**位置**：`src/platform/encoder/rk_mpp_encoder.cpp::encode()`（与 `src/common/frame_buffer.h::BindDataPointers()` 的耦合）。

**根因（原理，关键在这条偏移公式）**
`FrameBuffer` 是**视图**，平面指针由 `BindDataPointers()` 算：

```cpp
data[0] = base_ptr;
data[1] = base_ptr + stride[0] * height;   // ← height 是「池块」的 height
```

而编码器当时是**一次性连续拷贝**：

```cpp
memcpy(dst_ptr, fb->data[0], fb->data_size);   // data_size = 本帧 Y+UV
```

两者对「UV 在哪」的假设不一致：
- `BindDataPointers` 认为 UV 在 `stride[0] × 池块高` 之后（实测打印 `d1off = 2073600 = 1920×1080`）；
- 连续拷贝隐含「UV 紧接在本帧 Y 之后」（偏移 = 本帧 `stride*height`）。

于是小帧（1024×576）拷进 MPP buffer 的 UV 段，实际取到的是**本帧 Y 之后的内存残影**（上一帧 / 其它链路的亮度数据）→ 亮度正常、色度全乱。
ch0 恰好因为 `d1off == 1920×1080 == 本帧 Y 大小` 而"碰巧正确"——这就是"为什么只有 mp4 路花屏"的完整解释。

**修复**：按平面分别拷贝，UV 紧接在 MPP buffer 的 Y 之后（偏移 = `hor_stride * ver_stride`）：

```cpp
const size_t y_size = frame_size / 3 * 2;               // NV12: Y 占 2/3
memcpy(dst_ptr, fb->data[0], y_size);
memcpy((uint8_t*)dst_ptr + y_size, fb->data[1], frame_size - y_size);
```
`frame_size == 0` 时按 `stride * height * 3 / 2` 兜底；`data[0]/data[1]` 为空时归还 buffer 并返回 false。

**定位手法（客观判据，不靠肉眼）**：拉流解码成 `yuv420p`，统计 U/V 均值±标准差，与**源 mp4** 同分辨率对比：
修复前 ch1 `U=105.4±41.1 / V=105.5±41.2`；源 `U=126.6±11.0 / V=128.2±1.7`（均值偏离 + 方差 4 倍）；修复后 `U=128.1±10.9 / V=128.0±1.6`。

**面试怎么讲**：*"`FrameBuffer` 是块级视图，'平面偏移'按块分辨率算；任何一次性连续拷贝都在赌'本帧尺寸 == 块尺寸'。我改成按平面拷贝，并把'UV 紧接 Y'这个物理布局显式写进偏移。"* 延伸：视图 vs 物理布局分离、stride/宽高不变量。

---

## 2.7 B7 · 池块上的 `FrameBuffer` 视图被跨链路改写（P0-14：下半屏绿）

**现象**：四路并发时 ch0（1920×1080）画面**上半屏正常、下半屏约 47% 以下全绿**，且**间歇出现**（同一路时好时坏）；单跑 ch0 时 176 帧全好。ch1~ch3（1024×576）正常。

**位置**
- `src/platform/capture/v4l2_source/v4l2_capture.cpp::read_frame()`（受害者）
- `src/platform/capture/ffmpeg_source/ffmpeg_capture.cpp::transfer_frame()`（改写着）

**根因（原理）**
`MemMeta` 里的 `FrameBuffer` **不是每帧私有的**，而是挂在共享池块上的**可变视图**：

```
MemBlock ──> MemMeta ──> FrameBuffer{ data[0], data[1], width, height, stride[0..2], fmt }
                          ↑ 四条链路共用同一批块
```

`FfmpegCapture::transfer_frame()` 写完自己那帧，会把视图改成**它的**分辨率（1024×576、`hw_stride=1024`）再随块 `release()` 回池；`V4l2Capture::read_frame()` 取到这种块后**直接沿用** `frame->stride[0]` 去开 `sws_scale`：

```cpp
int dst_linesize[4] = {frame->stride[0], frame->stride[1], 0, 0};   // ← 过期值 1024
sws_scale(ctx, src, src_ls, 0, m_src_height, dst_data, dst_linesize);
```

`stride=1024` 却要写 1080 行 ⇒ 只写满 `1024×1080 = 1 105 920` 字节 ⇒ 按 1920 宽折算**恰为 576 行**，其余部分保持旧数据/0。
数字完全自洽：10 段 Y 均值实测 `… 111.5 | 37.2 | 0 0 0 0`，band5 有数据的行数 `37.2/112×108 ≈ 36` 行 = `576−540` ✓。

**为什么难发现**
- 画面证据只能看出"下半屏绿"，**看不出 stride 是 1024 还是 1920**；
- 损坏是**间歇的**：取决于池块上一次被谁用、以什么分辨率用（单路时没有别的链路改写视图，永远正常）；
- 方向容易被"USB 带宽/丢帧/驱动"带偏（实测 `bytesused` 450 帧 `short=0`，源 YUYV 完全正常）。

**修复**
- 两个采集后端在**写块之前**按自己的输出分辨率重绑视图：
  `BindMemory(frame->data[0], 本链路宽, 高, FMT_NV12[, 128])`（`data[0]` 恒为块首地址，可安全复用；格式按**实际写入的字节布局**取 —— V4L2 侧固定 NV12，因为 `sws_getContext` 输出就是 `AV_PIX_FMT_NV12`）。
- V4L2 侧另加 `sws_scale` 返回行数校验：`ret_h != m_dst_height` ⇒ `VIDIOC_QBUF` 归还并丢帧，**不再静默产出半帧**。
- 已升格为红线（HANDOFF §2.4 第 7 条）：**任何生成/采集帧的后端，写块前必须重绑视图。**

**定位手法（可复用，本例最有价值的部分）**
1. **分层成对探针**：同一循环里同时打印「源」与「目标」（`srcY` 十段 vs `Ybands` 十段），才能一刀切开"源坏"还是"转换/拷贝坏"：
   - UVC 源十段正常（`169.9 152.7 … 109.1`）+ 输出 `… 111.5 | 37.2 | 0 0 0 0` ⇒ 坏在 sws **之后**；
   - MPP buffer（`memcpy` 之后）十段与采集侧**完全一致** ⇒ 排除编码器与传输（`memcpy` 是忠实的）；
   - dump 码流后本地解码同样下半屏 0 ⇒ 排除编码/解码/RTSP。
2. **打印"元数据"而不只是像素**：`stride[0]/width/height` 第 0 帧是 `1920/1920/1080`，第 30 帧起变成 `1024/1024/576`，甚至出现 `w=1920` 但 `st0=1024` 的混合态 —— **这一刻根因就确定了**。
3. 有了 stride 值就能**用算式验证**（1024×1080 / 1920 = 576 行），而不是"看起来像"。

**面试怎么讲**：*"坏的那一半让我先怀疑采集和驱动，但成对探针立刻排除了源——真正错的是写入时的行宽。根因是共享池块上的 FrameBuffer 视图是全局可变状态，别的链路按自己的分辨率改写了它；我在所有写入点前重绑视图，并加行数校验让错误显式化。"*

---

## 2.8 B8 · `while(1)` 挡住停止逻辑（T6）

**现象**：旧 `main_node.cpp` 按 Ctrl+C 进程不退出（只能 `kill -9`），`stop()` 与资源释放代码**从未执行过**。

**位置**：`src/main/test/main_node.cpp`（T6 重写前）。

**根因**：主线程结构是「装配 → `start()` → `while(1);` → `stop()`」——`while(1)` 不是"等待信号"，而是**无条件死循环**：既不判退出标志，也没有信号处理把控制流带出去。写在它后面的 `stop()` / `FreeMem()` 是**不可达代码**。

**修复（T6）**：

```cpp
std::signal(SIGINT, OnSignal);      // 必须在 start_all 之前装：启动瞬间按 Ctrl+C 不能变成默认动作杀进程
std::signal(SIGTERM, OnSignal);
PipelineBuilder::start_all(sets);
while (g_run) std::this_thread::sleep_for(std::chrono::milliseconds(200));
PipelineBuilder::stop_all(sets);    // 正序：源头先停
sets.clear();
MemManager::getInstance().FreeMem();
return 0;
```

信号处理函数只写 `volatile sig_atomic_t g_run = 0`（**异步信号安全**：不加锁、不打日志、不分配内存）。停止顺序与启动相反（终点先起、源头先停），已由日志实测：启动 `print0 → rtsp0 → vi0`，停止 `vi0 → rtsp0 → print0`。

**面试怎么讲**：*"死循环让后面的清理代码成了不可达代码；我用 `sig_atomic_t` + 轮询替代，并把信号注册放在启动之前。"* 延伸：为什么信号处理函数里不能 `printf`/`malloc`/加锁。

---

## 2.9 B9 · 配置值与素材真实参数不符（T3 定稿时发现）

**现象（两种，都不会崩）**
1. 播放端**加速/抖动**：画面像被按 1.25 倍速播放；
2. 小分辨率素材**花屏**：画面被拉伸/裁切后错位。

**位置**：`config/node.json`（ch1~ch3 的 `vi`/`push` 段）+ `moudle_push`（PTS 推算）+ `ffmpeg_capture.cpp::transfer_frame()`（不缩放）。

**根因（两个独立机制）**
1. `PushNode` 用 `dts/pts = 1000000 / fps` **递增推算**时间戳。素材实测 `23.976fps`（`ffprobe`：1024×576，8888s / 213098 帧），配置写 `30` ⇒ 每个时间戳比真实采集节奏**快 20%** ⇒ 播放端按声明节奏播放就表现为加速。
2. `FfmpegCapture::transfer_frame()` **不做缩放**（`vi.width/height` 只是声明），素材 `1024×576` 而配置写 `1280×720` ⇒ 推流声明的尺寸与实际帧数据不符 ⇒ 花屏。

**修复**：配置按 `ffprobe` 实测值定稿（`1024×576` / `fps=24`），并把「与 ffprobe 交叉校验」写成 T3 脚本的一条断言（`run_t3_node_json.sh` E 段），素材缺失时该段 SKIP 而不是失败。

**面试怎么讲**：*"这类问题不会崩，只会'看起来不对'——所以配置的正确性也要被测试断言。我用 ffprobe 的实测值去断言配置，素材缺失时降级为 SKIP。"* 延伸：用"帧序号×固定步长"推时间戳的前提是**配置 fps 等于真实采集节奏**；根治要靠文件循环 + 缩放适配（T7b）与按源帧率取 PTS。

---

## 2.10 B10 · `FrameBuffer` 所有权声明与实现相反：每次 `MemInit`/`FreeMem` 漏 12.44 MB（P0-9）

**现象**：进程**正常启停**——`FreeMem()` 打印"完成"、退出码 0、无段错误、无报错——但内存整块沉淀。4 路 1920×1080 默认池（32 块）一次起停就是 **199 MB**，反复启停几轮必然 OOM。比 B3 更阴：B3 是"跑着跑着静默堵死"，这个是"每次都漏一点、所有指标都绿"。

**位置**：`src/common/frame_buffer.h::Allocate/Release`、`src/common/MemManager/MemManager.cpp::MemInit/FreeMem`。

**根因（三层）**

1. **所有权声明与实现相反**（主因）。`FrameBuffer` 顶部注释写「仅作为 MediaBlock 的视图，**不拥有内存，不负责释放**」，而 `Allocate()` 的实现是 `new (std::nothrow) uint8_t[capacity]` —— 它**真的**分配了堆内存。`FreeMem()` 的作者按注释办事，只 `delete _memData`（`MemMeta`），那块 6.22 MB 从此无人认领。
2. **"绑自己的块"把所有权说了两遍**。`MemInit()` 在 `Allocate()` 之后又跟一句 `BindMemory(frame->data[0], ...)`，而 `BindMemory()` 的语义是"绑定外部内存的视图"。两个接口语义重叠 → 谁都不敢在释放路径上 `delete[]`，因为"看起来这是外部内存"。
3. **附带泄漏**：`new MemBlock(_memMeta)` 后未使用（`MemPool::addBuffer()` 内部自己 `new`），`MemMeta` 无析构 → `MemSlot<T>` 每块漏 2 个（32 B/个）。

**修复**

```cpp
struct FrameBuffer {
  uint8_t *data[3];
  uint8_t *owned_mem = nullptr;  // Allocate() 分配的基址；BindMemory() 的视图恒为 nullptr
  void    *native    = nullptr;  // 平台句柄（MPP MppBuffer），仅视图，不拥有
  void Release() {
    if (owned_mem) { delete[] owned_mem; owned_mem = nullptr; }
    data[0] = data[1] = data[2] = nullptr;
    native = nullptr;
  }
};
// MemInit：只调 Allocate()，失败即 delete frame + continue；不再补 BindMemory()（视图接口只用于外部内存）
// FreeMem：frame->Release() → delete frame，再删 NodeProfiler / MemMeta / MemBlock
```

**后续（同日，P0-11 复查时收口）**：上面的 `Release()` 还是"靠调用方记得调"。复查 `MemMeta::clear()` 时发现更省心的写法是让**析构**负责——

```cpp
struct FrameBuffer {
  FrameBuffer() = default;
  FrameBuffer(const FrameBuffer &) = delete;             // 拷一份 = 两份 owned_mem = 双释放
  FrameBuffer &operator=(const FrameBuffer &) = delete;
  ~FrameBuffer() { Release(); }                          // RAII：delete 即归还
};
// 于是 MemMeta 里的 slot_free<FrameBuffer*> 特化可以删掉，主模板 delete 一句就够
```

这条改动**必须连拷贝一起禁**：加了析构却没有禁拷贝，等于把原来"隐式拷贝 → 泄漏"升级成"隐式拷贝 → 必崩"。ASAN 实测（`/tmp/fb_raii_copy_probe`）刚好抓到这条链：

```
ERROR: AddressSanitizer: attempting double-free on 0x007f85900100
  #1 FrameBuffer::Release()        frame_buffer.h
  #2 FrameBuffer::~FrameBuffer()
  #3 main                          ← 拷贝体出作用域时二次 delete[]
freed by thread T0 here:
  #2 FrameBuffer::~FrameBuffer()
  #3 main                          ← 原对象 delete 时已释放
```

栈上的视图（`ffmpeg_decoder.cpp:62` 那种 `FrameBuffer fb;` + `BindMemory()`）不受影响：`owned_mem == nullptr`，析构是 no-op（ASAN 实测外部缓冲出作用域后仍可写）。详见 TESTLOG §5.15。

**为什么难发现（两个"指标陷阱"，这段最值钱）**

1. **"RSS 涨了才叫泄漏"不成立**：`new[]` 的页不写就不进 RSS。首版探针 6 轮 `VmRSS` 全平在 2364 kB，看着像"没漏"。
2. **换了 `hblkhd` 还会被骗第二次**：改看 `mallinfo2().hblkhd`（mmap 在用字节）后，第一轮 +12,443,648 B 很清楚——但**第二轮 init 显示 0**。原因是 glibc 在 free 掉大 mmap 块后会把 `mmap_threshold` 抬到该块尺寸，后续同尺寸分配改走 **brk 主堆**，于是 mmap 字节数不再增长。
3. 正确口径是 **`uordblks + hblkhd`（在用堆总量）**：未修时每轮净 +12.44 MB 且 free 后一分不还；修复后 init 12.52 MB → free 回落 82 kB、6 轮零累积（残余 128 B/轮 = `MemSlot`，属 P0-11）。
4. 方法论：这类 bug 的证据只能来自**能失败的测量**——先让探针在旧代码上稳定复现增长曲线，再改代码看它归零；"改完是平的"本身不构成证明。

**面试怎么讲**：*"这不是内存'漏'，是**所有权没说清**：结构体注释写着'不拥有内存'，它的 `Allocate()` 却在 `new[]`；初始化时又用'绑定外部内存'的接口绑了自己的块，等于把所有权声明了两遍，释放路径谁都不敢删。我改成 `owned_mem` 显式记基址 + `Release()` 显式释放，并把 `FreeMem()` 补成帧 / profiler / meta / block 全释放。验证上也踩过两个坑：RSS 不反映未触碰的 malloc 页，glibc 的动态 mmap 阈值会让 `hblkhd` 在第二轮假性归零——所以要用 `uordblks + hblkhd` 的在用堆总量，并且先在旧代码上复现出增长。"*

---

## 2.11 B11 · 类型擦除擦掉了「销毁」：`MemMeta` 槽位只建不销 + 节点自建槽位漏收（P0-11）

**现象**：P0-9 修完（B10）后，`MemInit(2)+FreeMem()` 每轮仍残留 **128 B**（2 块 × 2 个槽位），曲线是 `82208 → 82336 → 82464 …` 每轮稳定 +128 B；更隐蔽的一层是 `VpssNode::ensure_dst_frame()` 按块懒创建的输出帧挂在 `vpss_out` 槽位上，**整帧 6.22 MB/块**永远不会被回收——只是因为 `config/node.json` 里 vpss 是 `enable:false`，实测路径不触发，所以"看起来没问题"。

**位置**：`src/common/MemManager/MemMeta.h::MemSlot/MemMeta::set_ptr/data`、`src/common/MemManager/MemManager.cpp::FreeMem`、`src/pipeline/moudle_vpss/moudle_vpss.cpp::ensure_dst_frame`。

**根因原理（关键在"擦除擦过了头"）**

1. `MemMeta` 里存的是 `unordered_map<string, void*> data_map_`，槽位对象是 `new MemSlot<T>(ptr)` —— **容器这一层确实做到了类型无关**（谁都能挂、`data<T>` 取回），这是设计意图，没问题。
2. 但 `void*` 同时把**销毁**这条路擦掉了：`delete (void*)p` 是 **未定义行为**，标准明确它**不调用析构函数**；而唯一会把槽位里的对象还回去的函数 `MemSlot<T>::freeObj()` 是模板成员，调用点必须知道 `T`，运行期手里只有 `void*` 时**根本调不到**。
3. 后果一：`set_ptr()` 只能写成 `data_map_[name] = new MemSlot<T>(ptr)` —— 同名覆盖时旧槽位指针直接被顶掉，槽位结构体（32 B）+ 里面挂的对象（可能是 6 MB 帧）一起失联。
4. 后果二：`MemMeta` 没有析构、也没有 `clear()`，于是 `FreeMem()` 只能"按名字点名"遍历 `imgMap`（`nv12`）逐个 `Release()`+`delete`。名单是 `MemInit` 自己填的 ⇒ **节点后来挂上去的槽位天然不在名单里**（`vpss_out` 整帧漏收）；容器拿走了唯一能回收对象的指针，却不承担所有权。
5. 后果三（顺带）：`data<T>()` 是 `(MemSlot<T>*)it->second` 的 C 风格硬转，槽位名对应错类型时静默 UB（P1-10）。

**修复**（保留原 `MemSlot<T>` 抽象，只补"销毁通道"）

```cpp
// 1) 让擦除后的指针也能被正确析构
class MemSlotBase { public: virtual ~MemSlotBase() {} };

template <typename T>
class MemSlot : public MemSlotBase {      // setObj/freeObj/getObj/getObjPtr 语义不变
public:
  ~MemSlot() { freeObj(); }               // 槽位死 = 对象一起还回去
  void freeObj() { slot_free(t); t = nullptr; }
};

// 2) FrameBuffer* 要先 Release() 再 delete（否则 owned_mem 的几 MB 又漏回 P0-9）
template <typename T> inline void slot_free(T t) { delete t; }
template <> inline void slot_free<FrameBuffer *>(FrameBuffer *t) {
  if (!t) return; t->Release(); delete t;
}
//    ↑ 这段特化在当日稍晚随 `~FrameBuffer(){Release();}` 一并删除（见 §2.10 后续段），
//      现在主模板 `delete t` 一句就足够 —— 保留在此只为说明"当时为什么要它"

// 3) 容器改存基类指针，补 clear()/析构，set_ptr 覆盖前先释放旧槽位
std::unordered_map<std::string, MemSlotBase *> data_map_;
size_t clear() {  // swap 出 map → 锁外逐个 delete → 虚析构 → freeObj() → slot_free()
  std::unordered_map<std::string, MemSlotBase *> old; { std::lock_guard<std::mutex> lk(mtx_); old.swap(data_map_); }
  size_t n = 0; for (auto &kv : old) { delete kv.second; ++n; } return n;
}
// 4) FreeMem() 统一 _memData->clear()，不再按 imgMap 点名
// 5) data<T>() 用 dynamic_cast 校验类型，类型不符返回空 + 一行日志（同修 P1-10）
```

实测：`MemMeta` 探针 2 个槽位占 12.52 MB → 覆盖同名**不再多留一帧**（只 +128 B 记账）→ 同一指针重复登记**不双删**（保留在用对象）→ `clear() released=2` 回落 78 kB；`MemInit(2)+FreeMem()` 6 轮**恒定 82,080 B**；四路 `testnode` SIGINT 打印 `FreeMem: 释放 50 块 / 100 个槽位`、退出码 0。

**为什么难发现**

1. **前面刚修完一个更大的泄漏，128 B/轮看着像"噪声"**：如果不是刻意把残余量级算出来（2 块 × 2 槽位 × 32 B），很容易当成 allocator bookkeeping 放过。
2. **真正的坑被配置挡住了**：`vpss` 的 `enable:false` 让 6.22 MB/块那一条完全不发作；换句话说"没复现"不是因为修好了，而是因为那条代码没跑。
3. **`void*` 的破坏力在编译期完全安静**：`data_map_[name] = memSlot` 从 `MemSlot<T>*` 隐式转 `void*` 合法、无警告；`delete` 也编译得过（只是 UB）。这类"擦除擦过头"的缺陷只有把释放路径完整走一遍才暴露。
4. **`FreeMem()` 的"点名"写法看着很负责**：它确实逐个 `Release()`+`delete` 了，只是名单之外的东西它一无所知——按名单回收永远只是"已知集合"的回收。

**面试怎么讲**：*"我们那个数据池容器用 `void*` 做类型擦除，挂什么东西都行，看着很灵活。但我后来发现它把销毁那条路也擦掉了：`delete` 一个 `void*` 是 UB 而且不调析构，唯一会释放对象的 `freeObj()` 是模板成员，运行期没有 `T` 就调不到。结果槽位只能建不能销——同名覆盖就漏 32 B，节点自己挂上去的 6 MB 输出帧干脆整块漏，因为释放路径只能按初始化的名单点名。修法是给槽位补一个带虚析构的基类、容器改存基类指针并加 `clear()`，`delete` 时通过虚析构触发 `freeObj()`；`FrameBuffer*` 单独走先 `Release()` 再 `delete`。经验是：**类型擦除本身不难，难的是擦掉类型之后还得留下一条能正确销毁的通道**——虚函数或存一份删除器，二选一，别只留 `void*`。另外验证要算到字节级，128 B/轮也是漏。"*

---

## 2.12 B12 · 崩溃报告有了，core 却一个都没有（信号 handler 内的屏蔽语义）

**现象**：装上 `crash_handler` 后触发 SIGSEGV：报告 `.log` 正常写出、退出码 `139`、终端甚至没有 `(core dumped)`；`ulimit -c` 是 `unlimited`、`/proc/sys/kernel/core_pattern` 是 `core`、`PR_SET_DUMPABLE` 也在 handler 里设过 —— **但 `ls core*` 是空的**。

**位置**：`src/common/crash/crash_handler.cpp` — `OnFatalSignal()` 里「报告写完 → 重抛信号 → 产生 core」那段（`SYS_tgkill` 调用前一行）。

**根因原理（Linux 信号递送语义）**：
1. 内核递送信号、调用 handler 时，除非 `sigaction` 带了 `SA_NODEFER`，**会把"正在处理的这个信号"自动加入该线程的阻塞信号集**，handler 返回时才恢复。
2. 因此在 handler 里 `tgkill(getpid(), gettid(), SIGSEGV)` 并不会立刻杀死进程 —— 它只是把该信号放进该线程的 **pending** 集（blocked ∧ pending）。
3. 紧接着执行 `_exit(128 + sig)`：进程**正常退出**，pending 信号随线程销毁一起被丢弃 → 内核根本没有走到「按 `core_pattern` 写 core」的 `do_coredump()` 路径。

> 一句话：**信号"被处理过"不等于"递送完成"**。要自己再触发一次默认动作，必须先解除屏蔽。

**修复**：重抛前显式解除该信号屏蔽：

```cpp
sigset_t one; sigemptyset(&one); sigaddset(&one, sig);
sigprocmask(SIG_UNBLOCK, &one, NULL);          // ← 缺这一行就没有 core
syscall(SYS_tgkill, getpid(), gettid(), sig);  // SA_RESETHAND 已恢复 SIG_DFL
// 正常情况下内核在 tgkill 返回用户态前就终止进程，这里走不到
_exit(128 + sig);                              // 仅兜底 tgkill 失败
```

实测对比（同一探针、同一 `ulimit -c unlimited`）：

| | `ls core*` | 终端 |
| --- | --- | --- |
| 修复前 | 空 | 无 `(core dumped)` |
| 修复后 | `core.75255`（589,824 B） | `bash: ... Segmentation fault (core dumped)` |

**为什么难发现**
- 三个"经典嫌疑"（`RLIMIT_CORE=0`、`core_pattern` 是相对路径、`dumpable=0`）**都排查过且都正常**，很容易下结论「内核不让写」。
- 失败是**静默**的：报告成功、退出码正确、进程确实死了 —— 只有"core 文件不存在"这一个阴性证据，容易归咎于环境。
- 该规则在 `man 7 signal`（"the signal being delivered is added to the mask"）和内核 `signal.c` 里，但**不在 `sigaction(2)` 的 flags 说明里**，只看 API 文档读不出来。
- 顺带解释了一个常见困惑：`SA_RESETHAND` 只重置 **disposition**（处置函数），**不动 signal mask** —— 两件事在 `sigaction` 结构里相邻，语义完全不同。

**面试怎么讲**：*"我要在信号 handler 里重抛信号产生 core，结果报告有、core 没有 —— 因为 handler 执行期间内核会屏蔽当前信号，`tgkill` 只是挂 pending，被我随后的 `_exit` 丢掉了。修法是先 `sigprocmask(SIG_UNBLOCK)` 再 `tgkill`。"*
可延伸：`pending` vs `blocked` 的区别、`SA_NODEFER` 的用途（handler 内允许同信号嵌套）、多线程下必须 `tgkill` 到**崩溃线程**（`gettid()`）而不是 `kill(getpid())`，否则 core 的 `NT_PRSTATUS` 记录的是别的线程。

---

## 2.13 B13 · 栈帧地址换算成 `?? ??:0`（`backtrace_symbols` 的偏移语义）

**现象**：`symbolize_report.sh` 对 `crash_handler` 自身那一帧能解析出 `crash_handler.cpp:297`，但对 `crash_in_worker` / `main` 全部输出 `?? ??:0` —— 地址明明是有效地址、二进制也没被换过。

**位置**：`scripts/coredump/symbolize_report.sh` 的帧解析循环。

**根因**：`backtrace_symbols()` 输出有两种形态，**偏移的参照点不同**：

| 输出形态 | 偏移的含义 | 送 `addr2line` 前要做的事 |
| --- | --- | --- |
| `模块(符号+0x24)` | **符号内**偏移 | 先查符号 vaddr，`vaddr + 0x24` |
| `模块(+0x3618)` | **模块内**偏移（相对加载基址） | 直接可用（PIE 下 = 文件 vaddr） |

原脚本一律 `addr2line 0x<偏移>`，于是：
- 无符号那一帧（`+0x3618`）恰好对了 → 误以为"脚本没问题"；
- 有符号那几帧（`+0x24`）把"符号起点之后 0x24"当成"文件偏移 0x24" → 落在 header/空洞里 → `?? ??:0`。

**修复**：有符号名时用 `nm`（先 `-D`，再退回 `.symtab`）查该符号的 vaddr，加上偏移再送 `addr2line`：

```bash
sym_vaddr="$(nm -D --defined-only "$mod" | awk -v s="$sym" '$3 == s {print $1; exit}')"
[ -n "$sym_vaddr" ] || sym_vaddr="$(nm --defined-only "$mod" | awk -v s="$sym" '$3 == s {print $1; exit}')"
target="$(printf '0x%x' "$(( 16#$sym_vaddr + 16#$off ))")"   # 查不到就退回 0x$off
```

修复后：`crash_in_worker(int, Frame*) /tmp/evs_crash_probe.cpp:17`、`main …:38`。

**另一个同源坑（gdb 侧）**：`gdb exe core -ex "set solib-search-path ..."` 里 core 在**所有 `-ex` 之前**就被读进来，于是 14 个 `.so` 全部 `Could not load shared library symbols`；同时 `solib-search-path` 里的**相对路径**会被 gdb 再按 cwd 拼一次。修法：用绝对路径 + `-ex "file $EXE" -ex "core-file $CORE"` 显式加载，`missing=0`。

**面试怎么讲**：*"符号化失败的根因是偏移的参照点：`模块(符号+0x24)` 是符号内偏移，直接用会落到文件头变成 `??`；要先 `nm` 查符号地址再加。"* 可延伸：PIE/ASLR 下 `dladdr` 的 `dli_fbase` 与文件 vaddr 的关系、`-rdynamic` 只影响 `.dynsym` 不影响 DWARF、以及"为什么必须保存崩溃当时那份二进制"。

---

## 3. 沉淀下来的排查方法论（比单个 bug 更值钱）

| # | 手法 | 来自 | 一句话 |
| --- | --- | --- | --- |
| M1 | **成对探针**：同一循环里打印「源」与「目标」 | B7 | 只有对比才能判断坏在"输入"还是"转换/拷贝" |
| M2 | **打印元数据**（stride / width / height / 指针偏移） | B6、B7 | 画面看不出 stride，但算式能证明 |
| M3 | **分层排除，每层留证据** | B7 | 采集 → sws → memcpy → 编码 → dump 解码，逐层排除而不是猜 |
| M4 | **算一遍数字**：坏区大小 / 行宽 / 偏移是否互相自洽 | B7 | 576 行 × 1920 = 1024 × 1080 —— 一致即根因确定 |
| M5 | **客观统计代替肉眼**：Y/UV 十段均值、U/V 均值±标准差 | B6、B7 | 无 GUI 环境也能给出可复现判据 |
| M6 | **先补可观测接口再断言** | B1 | `zlm_rtsp_stream_count()` 让「资源是否归零」可测 |
| M7 | **回归必须先跑基线对照**（同环境、同轮次） | B6 | 否则容易把环境污染当成代码缺陷 |
| M8 | **不要 `pkill -9` 硬件编码进程** | B6 | rkvenc 来不及释放 → `dmesg` 出现 `soft reset timeout`，之后持续报编码错误；要 SIGINT 优雅退出 |
| M9 | **验证要覆盖稳态**，不只是"能出帧" | B3 | 池类资源问题只在长跑/高并发下暴露 |
| M10 | **修好 A 要复查 B**：停止路径生效后，UAF 才浮出水面 | B1 | 生命周期类修复往往连带暴露下一层 |

## 4. 维护约定

1. **新修一个满足文件开头「记录规则」的 bug ⇒ 在同一次提交里追加本文章节**，同时更新 §1 索引表（#、症状一句话、根因一句话、提交、详见）。
2. 案例小节固定顺序：**现象 → 位置 → 根因（原理）→ 修复 → 为什么难发现 → 面试怎么讲**。缺「根因原理」或「位置不到函数级」的，视为无效记录。
3. 只写**已经实测确认**的根因；推测部分标注「待验证」。与 `TESTLOG.md` 的分工：本文讲**为什么**，TESTLOG 记**跑了什么、结果如何**。
4. 本文不做进度跟踪 —— 未修问题在 `STATUS.md`（P0/P1 表），这里只收**已定案**案例（已修复或已定位待修均可）。


