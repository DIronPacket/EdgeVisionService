# 三进程通信设计（MQTT / UDP / 共享内存的分工与协议）

> 状态：**设计稿（2026-11-15）**，尚未落代码。已落地的事实只有 `src/comm/` 的 `comm_api`（UDP + MQTT 两种后端，见 [src/comm/README.md](../../src/comm/README.md)）。
> 本文解决的问题：三个进程（`evs_media` / `evs_comm` / `evs_upgrade`）之间、以及它们与云端/其他 sensor 之间，**每条数据走哪条通道、协议长什么样**。
> 相关：进程边界见 [DESIGN_PROCESS_SPLIT.md](DESIGN_PROCESS_SPLIT.md) §4，升级 topic 见 [DESIGN_UPGRADE.md](DESIGN_UPGRADE.md) §9。

## 1. 结论先行

**不是「UDP 还是 MQTT 还是共享内存」，而是按数据性质分三条通道。**

| 通道 | 承载什么 | 频率 / 大小 | 用什么 | 为什么 |
| --- | --- | --- | --- | --- |
| **控制面** | 命令、事件、状态、心跳、升级进度、告警 | < 10 Hz，< 4 KB | **MQTT**（`comm_api` 已有） | 要可靠、要重连、要跨机/上云、要一对多；`evs_comm` 已经能发 |
| **遥测快通道** | IMU / 超声 / 电量 / 温度 / 帧率等**同机**高频小包 | 10 Hz – 1 kHz，16 – 1 KB | **共享内存环**（首选）或 **UDP 回环**（简化版） | 每包一次 syscall 很贵；丢一帧不影响业务，但不能阻塞生产者 |
| **大块数据** | 视频帧、点云、音频块 | 10 – 60 Hz，100 KB – 8 MB | **共享内存 + fd 传递**（DMA-BUF） | 拷贝一次就是几十 MB/s 的浪费；MQTT 与 UDP 都不该碰它 |

一句话判据：

```text
需要跨机器 且 能容忍秒级重连  → MQTT
同机 且 单包 ≤ 1 KB 且 高频      → 共享内存环（或 UDP 回环）
同机 且 单包 > 1 KB 或 > 1 MB/s  → 共享内存 / fd 传递，绝不进 MQTT
```

> 现在项目里**媒体进程还没接任何总线**（`media_service.cpp` 不建通道），所以上表是目标态，不是现状。

## 2. 事实前提（全部来自仓库已有结论）

1. `comm_api` 的 UDP 是 POSIX socket + poll，**零第三方依赖**；`Send` 发的是裸字节，**没有分帧/CRC/分片**（[src/comm/README.md](../../src/comm/README.md) §9.2）。
2. `comm_api` 的 MQTT 是 libmosquitto **客户端**，broker 是独立进程（`install/rk3588/sbin/mosquitto`）；`kServer` = 通配订阅端，不是 broker。
3. MQTT 单条建议 **≤ 4 KB**，大块数据只放路径/句柄（[DESIGN_PROCESS_SPLIT.md](DESIGN_PROCESS_SPLIT.md) §5.2）。
4. UDP **不重传、不排序、无背压**：回调慢了内核缓冲满就丢（`src/comm/README.md` §9.7）。
5. 帧内存已经是 **MPP DRM 缓冲**（`src/common/frame_mem.{h,cpp}` 是唯一分配边界，`mpp_buffer_get` + `mmap`）。
6. 硬约束：C++14、不引入 `install/` 里没有的第三方库（HANDOFF §2.1/§2.2）。所以协议**不能**用 protobuf / flatbuffers / msgpack。

## 3. 为什么 MQTT 不能承载高频遥测

按量级算（1 kHz IMU × 3 个 sensor + 4 路帧率）：

| 项 | MQTT 的代价 | 共享内存的代价 |
| --- | --- | --- |
| 序列化 | 每包一次 JSON（`src/common/json.hpp`）→ 几百 ns~几 µs，外加内存分配 | 直接写二进制槽，无分配 |
| 传输 | 进程 → 内核 socket → broker → 内核 socket → 进程（**两次拷贝**） | 一次 `memcpy` 进 shm（若传 fd 则零拷贝） |
| 协议头 | topic 字符串每条重复（`evs/v1/sensor/evt/imu` ≈ 24 B） | 32 B 定长头 |
| 线程模型 | libmosquitto 单 loop 线程，QoS1 还有 inflight 队列 | 生产者直接写，消费者自己读 |
| 背压 | broker 队列涨 → 内存涨 / 断连 | 环满覆盖最旧 + 丢帧计数（确定行为） |

结论：**1 kHz 级别不要走 MQTT**；MQTT 只发**聚合统计**（1 Hz 上报"过去 1 s 收到 1000 包、丢 2 包、最大延迟 1.8 ms"）。

## 4. broker 放哪（三进程 + broker 的拓扑）

```text
                     ┌──────────────── 云端 / 上位机 ────────────────┐
                     │            MQTT over TCP 1883/8883            │
                     └───────────────┬──────────────────────────────┘
                                     │
                        ┌────────────▼────────────┐
                        │ mosquitto（独立进程）     │   install/rk3588/sbin/mosquitto
                        └───┬──────────┬──────────┘
             evs/v1/#       │          │       evs/v1/upgrade/#
        ┌───────────────────┘          └────────────────────┐
┌───────▼────────┐        ┌────────────────┐        ┌───────▼────────┐
│ evs_comm       │        │ evs_media      │        │ evs_upgrade    │
│ 路由/汇聚/心跳  │        │ 采集→编码→推流  │        │ 清单/验签/切换  │
└───────┬────────┘        └───────┬────────┘        └───────┬────────┘
        │  evs/v1/sys/evt/ipc_stats（1 Hz，聚合）
        └──────────────┬──────────┴─────────────────────────┘
                       │
        ┌──────────────▼───────────────┐
        │ 共享内存环 /dev/shm/evs_v1_*  │  ← 高频遥测 + 大块数据（同机）
        └──────────────────────────────┘
```

- broker 的两种形态（`evs_comm` fork/exec 拉起 vs systemd 单独拉起）仍是 [DESIGN_PROCESS_SPLIT §4.1](DESIGN_PROCESS_SPLIT.md) 的待决项。
- 若 broker 挂了：控制面断，但**共享内存通道不受影响**（媒体继续采集，遥测在环里累积由本地消费者读），这是分通道的主要收益。

## 5. MQTT 协议（控制面）

### 5.1 Topic 表

命名沿用 `evs/<协议版本>/<域>/<方向>/<动作>`（`DESIGN_PROCESS_SPLIT` §5.1），`evt` 上报、`cmd` 下发、`ack` 回执。

| Topic | 方向 | 谁发 | 谁收 | QoS | retain | 说明 |
| --- | --- | --- | --- | --- | --- | --- |
| `evs/v1/media/cmd/{start,stop,set_osd}` | cmd | 云 / `evs_comm` | `evs_media` | 1 | 否 | 不 retain：避免重启后重放旧命令 |
| `evs/v1/media/evt/{started,stopped,error}` | evt | `evs_media` | 云 / `evs_comm` | 1 | 否 | 状态迁移 + 原因码 |
| `evs/v1/media/evt/heartbeat` | evt | `evs_media` | `evs_comm` | 0 | **是** | 1 Hz；retain 让云端一连上就知在线 |
| `evs/v1/media/evt/offline` | evt | broker（LWT） | 云 | 1 | **是** | 遗嘱：连接异常断开时 broker 代发 |
| `evs/v1/media/evt/stats` | evt | `evs_media` | 云 | 0 | 否 | 1 Hz 聚合：fps/码率/丢帧/编码耗时 |
| `evs/v1/upgrade/cmd/{check,download,apply}` | cmd | 云 | `evs_upgrade` | 1 | 否 | 见 `DESIGN_UPGRADE` §9 |
| `evs/v1/upgrade/evt/{progress,done,failed}` | evt | `evs_upgrade` | 云 | 1（progress 可 0） | 否 | 同上 |
| `evs/v1/comm/evt/{peer_online,peer_offline}` | evt | `evs_comm` | 云 | 1 | 否 | 通道状态 |
| `evs/v1/comm/cmd/health` | cmd | 云 | `evs_comm` | 1 | 否 | 立即回一条 health |
| `evs/v1/sys/evt/{boot,shutdown,ipc_stats}` | evt | 任一进程 | 云 | 1 / 0 | 否 | `boot` 带 `boot_id`；`ipc_stats` 见 §6.5 |
| `evs/v1/sensor/evt/{health,stats}` | evt | 采集端 | 云 | 0 | 否 | **只放聚合**，原始波形走 §7 |

`ack` 约定（留位，首期不强制）：`evs/v1/<域>/ack/<msg_id>`，payload `{"v":1,"ts":…,"src":…,"ok":true}`。

### 5.2 Payload 通用字段

```json
{ "v": 1, "ts": 1763000000123, "src": "media", "msg_id": "m-1763000000-17" }
```

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `v` | int | 是 | 协议版本；不认识就丢弃并记日志 |
| `ts` | int64 | 是 | **毫秒** Unix 时间（跨机可对时；同机高精度用 `ts_us`） |
| `src` | string | 是 | `media` / `comm` / `upgrade` / `sensor:<名>` |
| `msg_id` | string | cmd 必填 | `<src>-<ns>-<seq>`；用于幂等去重与 ack 关联 |
| `boot_id` | string | 建议 | 每次进程启动生成（`/proc/sys/kernel/random/boot_id` 或随机 hex），用来识别"重启后 seq 归零" |

规则：陌生字段**必须忽略**（向前兼容）；缺必填 → 丢包 + 记日志；单条 ≤ 4 KB。

### 5.3 示例

```json
// ① 云 → 媒体：改 OSD（cmd，QoS1）  topic: evs/v1/media/cmd/set_osd
{ "v":1, "ts":1763000000123, "src":"cloud", "msg_id":"c-1-42",
  "channel":"ch0", "osd":{"text":"Gate-A 2026-11-15","x":16,"y":16,"font_size":28} }

// ② 媒体 → 云：心跳（1 Hz，QoS0，retain=1）  topic: evs/v1/media/evt/heartbeat
{ "v":1, "ts":1763000001000, "src":"media", "boot_id":"9f2c...",
  "uptime_s":8642, "streams":4, "fps_total":96.1, "rss_mb":212, "state":"running" }

// ③ 媒体 → 云：1 Hz 聚合统计  topic: evs/v1/media/evt/stats
{ "v":1, "ts":1763000002000, "src":"media",
  "channels":[{"name":"ch0","fps":25.0,"kbps":2048,"drop":0,"enc_cost_ms":6.2},
              {"name":"ch1","fps":23.9,"kbps":1536,"drop":1,"enc_cost_ms":5.8}] }

// ④ 采集端 → 云：sensor 健康（原始数据不走这里）  topic: evs/v1/sensor/evt/stats
{ "v":1, "ts":1763000002000, "src":"sensor:imu0",
  "hz_nominal":1000, "hz_actual":999.4, "lost":2, "max_latency_us":1800, "ring_drop":0 }
```

### 5.4 连接参数与三条硬约束

| 参数 | 建议值 | 理由 |
| --- | --- | --- |
| keepalive | 30 s | 与 `CommConfig.keepalive_sec` 默认一致 |
| `clean_session` | false（`client_id` 固定时） | 断线期间 QoS1 消息由 broker 暂存；否则会丢命令 |
| `client_id` | `evs-<proc>-<boot_id 前 8 位>` | 固定 client_id + clean_session=true 会互踢，必须二选一 |
| LWT | `evs/v1/<域>/evt/offline`，retain=1 | 云端不用等心跳超时就知道掉线 |
| 重连退避 | 1 s → 2 s → 4 s … 上限 30 s | 板端断网时不要打爆 CPU |

三条硬约束（来自 `src/comm/README.md` §5，必须遵守）：

1. **回调里不做重活**：MQTT 回调在 loop 线程，阻塞就断连；只入队。
2. **回调里不调 `Stop()`**：会自锁（已实测）。
3. **`Send` 非线程安全**：多线程发要加锁，或用 `CommManager` 的通道串行化。
## 6. UDP 协议（同机快通道 / 局域网遥测）

适用：**同机或同网段**、单包 ≤ 1 KB、10 Hz–1 kHz、可容忍丢包。`comm_api` 的 `UdpComm::Send` 直接发裸字节，所以协议头要自己带（这正是 `src/comm/README.md` §9.2 留的口子）。

### 6.1 帧格式（32 B 定长头 + 变长 payload）

| 偏移 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- |
| 0 | `magic` | u32 | `0x31535645`（小端即字节 `E V S 1`）；不匹配直接丢 |
| 4 | `ver` | u16 | 协议版本 = 1 |
| 6 | `type` | u16 | `1`=telemetry `2`=event `3`=ack `4`=heartbeat |
| 8 | `src` | u8 | `1`=media `2`=comm `3`=upgrade `16+n`=sensor n |
| 9 | `flags` | u8 | bit0 = `is_fragment`，bit1 = `is_last_fragment`，其余保留 0 |
| 10 | `len` | u16 | payload 字节数（不含头，**≤ 1200**） |
| 12 | `seq` | u32 | 每 `(src,type)` 独立单调递增；回绕按 u32 处理 |
| 16 | `ts_us` | u64 | `CLOCK_MONOTONIC` 微秒（同机统一时钟，见 §8） |
| 24 | `crc16` | u16 | CRC-16/CCITT-FALSE，覆盖 payload（头本身不校验，magic+len 已能挡大部分坏包） |
| 26 | `topic_id` | u16 | 小整数代替字符串（`1`=imu `2`=radar `3`=audio `4`=fps…）；映射表见 §6.4 |
| 28 | `frag_idx` | u8 | 分片序号，从 0 开始 |
| 29 | `frag_cnt` | u8 | 分片总数（不分成 1） |
| 30 | `reserved` | u16 | 必须写 0，接收端忽略（预留对齐） |

> 上表共 32 B（含 reserved）。**手工逐字段小端序列化**（`put_u32(buf, off, v)`），**不要** `memcpy(struct)`：结构体 padding / 对齐 / 大小端在跨编译器与跨架构时都会变，这是协议层最常见的坑。

### 6.2 示例：IMU 1 kHz

```text
payload = 6 × f32（ax, ay, az, gx, gy, gz）= 24 B，整包 56 B
1000 Hz × 56 B ≈ 56 KB/s ← 回环完全无压力，瓶颈只在 syscall（每包 ~1–2 µs）
字段与量级示意（真代码按 §6.1 逐字段序列化，**不要** `memcpy` 结构体）：
{ ax: 0.012, ay: -0.003, az: 9.807, gx: 0.001, gy: 0.0, gz: -0.002 }   ← 6 × f32
```

### 6.3 分片规则（什么时候该换通道）

| 单包 payload | 做法 |
| --- | --- |
| ≤ 1200 B | 单包发（以太网 MTU 1500 − IP/UDP 头 28 − 余量） |
| 1201 – 4 KB | 按 1200 B 切片，`frag_idx/frag_cnt` 标号，接收端 100 ms 超时丢弃整组 |
| > 4 KB 或 > 1 MB/s | **不要用 UDP**：换 §7 共享内存（或 fd 传递） |

UDP 单包理论上限 65507 B，但一旦 IP 分片，**丢一个分片 = 整包废**，且分片丢包率与包长成正比。1200 B 是保守的工程值。

### 6.4 `topic_id` 映射（写进 `src/comm/ipc_protocol.h` 的常量表）

```text
1 imu     2 radar    3 lidar   4 depth    5 audio
6 battery 7 thermal  8 fps     9 link_rtt 10 media_event
```

新增类型只允许**追加编号**，禁止复用旧号（否则老固件会误解析）。

### 6.5 丢包与延迟的可观测性（必须做，否则 UDP 等于"黑盒"）

接收端每 1 s 统计并上报一次 `evs/v1/sys/evt/ipc_stats`（走 MQTT，QoS0）：

```json
{ "v":1, "ts":1763000010000, "src":"media", "boot_id":"9f2c...",
  "link":"udp", "peer":"127.0.0.1:51000",
  "expect":1000, "recv":998, "lost":2, "lost_pct":0.2,
  "reorder":0, "crc_err":0, "max_gap_us":4200, "p99_latency_us":900 }
```

判据：`lost_pct > 0.1%` 或 `reorder > 0` 就要查——要么接收端回调阻塞（§5.4 第 1 条），要么环/回调处理慢。

## 7. 共享内存（高频 + 大块，同机唯一正确的选择）

### 7.1 为什么是环形缓冲，不是"一块大数组"

- 生产者不能等消费者（采集线程被阻塞 = 掉帧，比丢一包严重）。
- 环满时的行为必须**确定**：覆盖最旧（实时优先）或拒绝写入（可靠优先），二选一写进配置，不能"看运气"。
- 每槽自带 `seq`，读者据此判断"我读到的是不是被覆盖过的旧帧"（seqlock 思想，无锁）。

### 7.2 布局

```text
/dev/shm/evs_v1_telemetry         ← shm_open("/evs_v1_telemetry")，运行时数据，掉电即失（符合语义）
┌──────────────────────────────────────────────────────────────┐
│ Header（1 个 cache line 对齐）                                │
│  magic u32 | version u16 | slot_size u32 | slot_count u32     │
│  write_seq(u64, atomic) | drop_count(u64, atomic)             │
│  reader_seq[u64 × 4]  ← 每个读者自己的游标（最多 4 个读者）      │
├──────────────────────────────────────────────────────────────┤
│ Slot 0: { seq u64 | ts_us u64 | len u16 | type u16 | data[N] }│
│ Slot 1: …                                                     │
│ …                                                             │
└──────────────────────────────────────────────────────────────┘
首期参数：slot_size = 4096, slot_count = 1024 → 4 MB（IMU 1 kHz 可缓 1 s）
        对齐：Header 与每个 Slot 按 64 B 对齐（避免伪共享）
```

### 7.3 写读算法（伪代码，C++14 可直接落地）

```cpp
// 写者（采集线程，唯一）
uint64_t s = hdr->write_seq.load(std::memory_order_relaxed) + 1;
Slot* p = &slots[s % hdr->slot_count];
p->seq   = 0;                       // 先作废：读者看到 0 就知道正在写
std::atomic_thread_fence(std::memory_order_release);
p->ts_us = now_us(); p->len = n; p->type = t;
memcpy(p->data, src, n);
std::atomic_thread_fence(std::memory_order_release);
p->seq   = s;                       // 最后发布
hdr->write_seq.store(s, std::memory_order_release);
hdr->futex_or_eventfd_wake();        // 见 §7.4

// 读者（每进程一个）
uint64_t w = hdr->write_seq.load(std::memory_order_acquire);
uint64_t& r = hdr->reader_seq[my_idx];
if (w - r > hdr->slot_count) {       // 慢读者：跳过被覆盖的
    hdr->drop_count.fetch_add((w - r) - hdr->slot_count);
    r = w - hdr->slot_count;
}
while (r < w) {
    Slot* p = &slots[r % hdr->slot_count];
    if (p->seq != r) { /* 被覆盖，跳过 */ r++; continue; }   // 撕裂保护
    handle(p->data, p->len, p->ts_us);
    r++;
}
```

- **不要**把 `std::atomic<uint64_t>` 塞进 shm 而不检查 `is_lock_free()`：板端 aarch64 上 8 B 原子是 lock-free 的，但这是**前提必须断言**（`static_assert(std::atomic<uint64_t>::is_always_lock_free)` 是 C++17；C++14 用运行时 `assert(a.is_lock_free())`）。
- 内存序只用 `release`/`acquire`，不用 `seq_cst`（无必要且更慢）。

### 7.4 通知机制（三选一，首期选 B）

| 方案 | 做法 | 优点 | 缺点 |
| --- | --- | --- | --- |
| A 轮询 | 读者 1 ms 睡醒查 `write_seq` | 零依赖 | 空转 / 延迟 1 ms |
| **B eventfd + UDS 传 fd** | 创建者 `eventfd()`，通过 UNIX socket `sendmsg(SCM_RIGHTS)` 把 fd 传给读者；写者 `write(efd,1)`，读者 `poll()` | 延迟 ~10 µs，零第三方库 | 要写 fd 传递（约 40 行） |
| C `sem_t`（pshared） | `sem_init(&sem, 1, 0)` 放在 shm 里 | 实现最简 | aarch64 上进程共享 sem 有 glibc 版本差异，且不能多读者广播 |

首期 B；若嫌麻烦可先用 A（1 ms 轮询）打通逻辑，再换 B（接口不变）。

### 7.5 生命周期与权限（四个必须回答的问题）

| 问题 | 答案 |
| --- | --- |
| 谁创建 | `evs_media`（最先起）；用 `O_CREAT|O_EXCL` 创建，已存在则 `EEXIST` → 说明上次没清理，检查 `magic/version` 后复用（版本不符则销毁重建） |
| 谁清理 | 创建者在正常退出时 `shm_unlink`；异常退出（kill -9）残留 → 下次启动按 `magic` 复用；**`/dev/shm` 掉电清空，不需要持久化** |
| 权限 | `shm_open(..., 0660)` + `fchown(uid, evs 组)`；三个进程同组 |
| 多读者 | `reader_seq[4]`，每个读者占一格；超过 4 个读者直接报错（首期不需要更多） |

### 7.6 大块数据：帧 / 点云的 fd 传递

视频帧已经是 DMA-BUF（`mpp_buffer_get` + `mmap`，见 `src/common/frame_mem.*`）。跨进程正确做法**不是** `memcpy` 到 shm，而是：

```text
生产者：mpp_buffer_get_fd(block.buf) → sendmsg(UDS, SCM_RIGHTS, fd) + 元数据{seq,ts,size,fmt,w,h}
消费者：recvmsg → fd → ① mmap（CPU 访问）或 ② RGA importbuffer_fd（硬件色彩转换）
        用完 close(fd) + 通知生产者 release（引用计数在生产者侧）
```

★ **待实测**：仓库里验证过的是**同进程**内 `mpp_buffer_get_fd → RGA importbuffer_fd`；跨进程 import 需要 P4 实验确认（含 MPP 版本限制与 `MPP_BUFFER_TYPE_DRM` 前提）。**不要**在没有实测前声称"零拷贝跨进程可用"——若 import 失败，退路是"shm 里放一份 NV12 拷贝"（多一次 memcpy，但正确）。

## 8. 时间基准与一致性

| 场景 | 时间源 | 为什么 |
| --- | --- | --- |
| 同机进程间（shm / UDP 回环） | `CLOCK_MONOTONIC` 微秒 | 不受 NTP 跳变影响；三个进程同内核，时钟天然一致 |
| 跨机（云对账、时间同步） | `CLOCK_REALTIME` 毫秒（payload `ts`） | 云端要的是绝对时间 |
| 重启后区分 | payload `boot_id` | 否则 `seq` 归零会被误判成"乱序/丢包" |

规则：

1. 同一条数据**两个时间都带**：内部用 `ts_us`（monotonic），上报时补 `ts`（realtime）。
2. 同机时钟一致是**前提**，不要用 `gettimeofday` 混着来（NTP 回拨会让 `ts_us` 倒退 → 环的时序判断失效）。
3. 跨机 sensor（如另一块板）接入时，才有对时需求：先用 MQTT `evs/v1/sys/cmd/timesync` 做粗同步（±10 ms 级），再决定要不要 PTP。

## 9. 落地顺序（每步独立可验收，不推翻前一步）

| 阶段 | 内容 | 前置 | 验收（写进 TESTLOG） |
| --- | --- | --- | --- |
| **P1** | 控制面接进媒体进程：`evs_media` 建一条 MQTT 通道，发 `media/evt/heartbeat`（1 Hz）+ `media/evt/stats` | `comm_api`（已有）、`config/node.json` 加 `comm` 段 | 无 broker 时不崩（只告警）；有 broker 时 `mosquitto_sub -t 'evs/v1/#'` 看到 1 Hz 心跳 |
| **P2** | UDP 快通道：`src/comm/ipc_protocol.{h,cpp}`（32 B 头 + CRC16 + 序列化）、收发各一条通道 | P1 | 回环 1 kHz 跑 60 s：`lost_pct ≤ 0.01%`、`max_gap_us ≤ 5 ms`；坏 CRC 包被丢弃且计数 |
| **P3** | 共享内存环：`src/comm/shm_ring.{h,cpp}`（header + slot + 写读 + drop 计数）、eventfd 通知 | P2 | 1 kHz 写、100 Hz 读，慢读者注入（`sleep`）验证 `drop_count` 单调且不阻塞写者 |
| **P4** | 大块数据 fd 传递（DMA-BUF over UDS） | P3、跨进程 import 实测 | 一次传递 1080p NV12（3.1 MB）：`recvmsg` 拿到 fd 后像素校验一致；CPU 拷贝次数 = 0 |
| **P5** | 三进程全部接入 + `ipc_stats` 上报 + systemd `After=` 顺序验证 | P1–P4 | 断电重启 20 次：环复用无残留、`ipc_stats` 连续 |

**不做的事（明确划界）**：不引 protobuf/msgpack/DDS；不做跨机共享内存（那是 RDMA/zenoh 的领域）；不把视频帧塞进 MQTT（哪怕是 base64）。

## 10. 待决策 / 待实测

| # | 问题 | 影响 | 建议 |
| --- | --- | --- | --- |
| 1 | sensor 到底有几个、什么频率、单包多大 | 决定 P2 是否需要，以及环尺寸 | 先按"IMU 1 kHz × 3 + 雷达 20 Hz × 64 KB"估算（雷达只能走 shm） |
| 2 | 同机还是多机 | 决定 shm 是否可用 | 三个 evs 进程同机（已定），sensor 若在 MCU 侧则走 UART/CAN → 由采集进程转 shm |
| 3 | 跨进程 DMA-BUF import 是否可行 | P4 是否零拷贝 | **必须实测**；失败则退化为 shm 内 memcpy |
| 4 | 环满策略（覆盖最旧 vs 拒绝写） | 数据语义 | 遥测用覆盖最旧 + `drop_count`；控制类用拒绝写 + 返回错 |
| 5 | `evs_comm` 是否兼作 broker 守护 | 部署形态 | 倾向 systemd 单独管 broker（`evs.target` 里再加一个 unit），`evs_comm` 只管路由（代码改动小） |
| 6 | 是否需要 `ack` 通道 | 命令幂等 | 首期用 `msg_id` + 幂等实现，ack 留位不实现 |

## 11. 与仓库红线的关系

1. **C++14**：无 `std::any` / `if constexpr` / 结构化绑定；`std::atomic` + 手写序列化足够。
2. **不引第三方库**：CRC16 手写表（约 20 行），shm 用 POSIX，通知用 eventfd，JSON 用已有的 `src/common/json.hpp`。
3. **失败必释放**：shm 打开失败、fd 传递失败、`sendmsg` 部分写，都要走对应清理分支（HANDOFF §2.3 第 4 条的同一条纪律）。
4. **桩要诚实**：P4 未实测前，代码里返回 `E_NOTIMPL` 并打印明确日志，不假装"已支持零拷贝"。

