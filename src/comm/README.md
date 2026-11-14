# src/comm/ —— 通信层（抽象基类 + UDP/MQTT 两种后端 + 全局管理器）

> 一页看懂：**有哪些类、server/client 到底指什么、怎么用、哪里不能碰**。
> 规定与红线见 [../../docs/HANDOFF.md](../../docs/HANDOFF.md) §2；验收记录见 [../../docs/history/TESTLOG.md](../../docs/history/TESTLOG.md) §5.21。

## 1. 产物与文件

```text
out/lib/libcomm_api.so        ← add_library(comm_api SHARED ...)
out/bin/test_comm             ← T6 验收程序（源码在 scripts/tests/t6_comm.cpp）
```

| 文件 | 职责 | 是否含第三方头 |
| --- | --- | --- |
| `comm_types.h/.cpp` | `CommType{kUdp,kMqtt}` / `CommRole{kServer,kClient}` / `CommState` / `CommConfig` / `CommFrame` / 两个回调类型 + `ToString` / `ParseCommType` / `ParseCommRole` | 否 |
| `comm_base.h/.cpp` | 抽象基类：模板方法的 `Init/Start/Stop/Send/SendText`、内部接收线程、状态机、`last_error()` | 否 |
| `udp_comm.h/.cpp` | UDP 后端：POSIX `socket/recvfrom/poll`，**零第三方依赖** | 否（`<netinet/in.h>` 等只在 `.cpp` 的 `Impl` 里） |
| `mqtt_comm.h/.cpp` | MQTT 后端：libmosquitto **C API** | 否（头里只前向声明 `struct mosquitto;`） |
| `comm_manager.h/.cpp` | 全局单例工厂 + 注册表 + 统一回调 | 否 |

依赖：`Threads`（标准）+ `mosquitto`（`install/<平台>/lib/libmosquitto.so`，2.0.22，`cmake/FindMosquitto.cmake` 查找，缺失即 `FATAL_ERROR`）。

## 2. 设计：一个基类 + 五个钩子

`CommBase` 把「生命周期 + 接收线程 + 状态 + 错误串」收敛掉，子类只回答 5 个问题：

| 纯虚函数 | UDP 的实现 | MQTT 的实现 |
| --- | --- | --- |
| `DoInit(cfg, err)` | `socket` + `SO_REUSEADDR` +（server）`bind` | `mosquitto_lib_init` 计数 + `mosquitto_new` + 设 user/pass/回调 |
| `DoStart(err)` | client：`connect`；server：无（监听已就绪） | `mosquitto_connect` + `mosquitto_subscribe` |
| `DoStop()` | `shutdown(fd, SHUT_RDWR)` 唤醒 `poll` | `mosquitto_disconnect` 让 `mosquitto_loop` 返回 |
| `DoSend(data,size,err)` | server：发给最近对端；client：`send` | `mosquitto_publish(topic_pub)` |
| `ReceiveLoop()` | `poll` → `recvfrom` → `EmitFrame` | `mosquitto_loop(timeout)` → 消息回调 → `EmitFrame` |

**`Stop()` 的顺序不能改**（改了就是卡死或 UAF）：

```text
running_ = false      // 1. 先让接收循环知道要退出
DoStop()             // 2. 再打断阻塞调用（shutdown / disconnect）
join()               // 3. 最后等线程真正结束
```

如果先 `join()` 再 `DoStop()`，阻塞在 `poll`/`mosquitto_loop` 的线程永远等不到唤醒 → `Stop()` 卡死。
如果先 `running_=false` 就 `join()`，`poll` 的超时（`recv_timeout_ms`，默认 200ms）也能让它退出，但 `DoStop()` 会作用在已关闭的 fd 上。

**幂等**：重复 `Start()` 返回 `true`；`Stop()` 可重复调；`Stop` 之后可以再次 `Init + Start`（UDP 用 `shutdown` 而不是 `close` 就是为了这个 —— fd 只在析构 / 下次 `DoInit` 里关）。

## 3. ★ server / client 在两种后端里含义不同（最容易误解的地方）

| | UDP | MQTT |
| --- | --- | --- |
| `kServer` | **真的在监听**：`bind(host:port)`，等别人发来；`Send()` 回到「最近一次收到的对端」 | **不是 broker**：订阅通配主题（默认 `evs/v1/#`）接收各端上报的「订阅端角色」 |
| `kClient` | `connect(host:port)` 到服务端，`send/recv` 固定该对端 | 订阅自己的下行主题（默认 `evs/v1/comm/cmd/#`），向 `topic_pub` 发布上报 |
| 谁在中间 | 无 | **独立进程 broker**（`install/<平台>/sbin/mosquitto`） |

为什么 MQTT 的「服务端」不是 broker：**libmosquitto 只提供客户端 API**，broker 是另一个程序。所以代码里的 `CommRole` 表达的是「在业务拓扑里扮演哪个角色」，而不是「谁在 listen 端口」。

```text
UDP：   client ──数据报──▶ server            （server 记住最近对端才能回）
MQTT：  client ──publish──▶ [broker] ──▶ server （server 用 evs/v1/# 通配订阅收）
        client ◀──subscribe cmd/…── [broker] ◀── server publish
```

### 3.1 MQTT 会收到自己发的消息（不是 bug）

MQTT 的投递规则是「**订阅匹配上的都收到，包括发布者自己**」。所以：

- `topic_pub` 落在自己的 `topic` 通配范围内 → 自己也会收到一份，业务上容易误判成「对端回包」。
- 想避免就把上报/命令主题分到不同前缀（验收程序就是这么做的：`client` 订阅 `evs/v1/comm/cmd/#`、上报到 `evs/v1/media/evt/heartbeat`）。

要「本地不回灌」需要 MQTT 5 的 `No Local`（`mosquitto_subscribe_v5` + `MQTT_SUB_OPT_NO_LOCAL`）。当前实现走 MQTT v3.1.1，**未启用**；若真需要，改 `MqttComm::DoStart()` 换 v5 订阅接口即可（broker 2.0.22 支持）。

### 3.2 默认发布主题怎么定（三条规则，`MqttComm::DoInit`）

| 情况 | `pub_topic` |
| --- | --- |
| `cfg.topic_pub` 非空 | 用它（推荐显式配置） |
| 订阅主题里含 `#` / `+` | 发到 `evs/v1/comm/evt/server`（server 角色）或 `evs/v1/comm/evt/client`（client 角色） |
| 订阅主题是具体主题 | 发到订阅主题本身 |

第二条是必须的：**通配符不能当发布主题**（broker 会拒绝），所以 server 角色（默认订阅 `evs/v1/#`）不能「回发到订阅主题」。

由此得到一个实际结论：

- 想让 server 的回包到达 client，就显式设 `topic_pub`，且它必须落在 client 的订阅范围里（验收程序：server `topic_pub=evs/v1/comm/cmd/pong`，client 订阅 `evs/v1/comm/cmd/#`）。
- server 订阅 `evs/v1/#` 时，**自己发的 `evs/v1/comm/evt/server` 也会被自己收到**（§3.1）。业务侧要靠 `frame.topic` 区分上报与回包，别只看计数。

## 4. 用法

### 4.1 走全局管理器（推荐）

```cpp
#include "comm_manager.h"

using namespace evs::comm;

CommManager &mgr = CommManager::getInstance();

// 统一的收包入口：第一个参数是通道名
mgr.SetReceiveCallback([](const std::string &name, const CommFrame &f) {
  // f.peer（UDP = "ip:port"；MQTT = broker 地址）
  // f.topic（MQTT 主题；UDP = 配置里的 topic）
  // f.data / f.size —— 只在回调期间有效，要留存请自己拷
});

// UDP 服务端：监听 0.0.0.0:9000
CommBase *udp_srv = mgr.Create(CommType::kUdp, CommRole::kServer, "0.0.0.0", 9000);

// MQTT 客户端：连 broker，订阅命令主题，上报到 media/evt
CommConfig mq;
mq.type = CommType::kMqtt;
mq.role = CommRole::kClient;
mq.host = "127.0.0.1";
mq.port = 1883;
mq.topic = "evs/v1/comm/cmd/#";
mq.topic_pub = "evs/v1/media/evt/heartbeat";
CommBase *mq_cli = mgr.Create(mq);

if (udp_srv == nullptr || mq_cli == nullptr) {
  std::cerr << "创建失败: " << mgr.last_error() << std::endl;
}

mgr.StartAll();                 // 或逐个 Start(name)
mq_cli->SendText("{\"alive\":1}");
mgr.StopAll();                  // 也可以在单个通道上 Stop()
mgr.Clear();                    // StopAll + 释放
```

`Create` 返回的指针**由管理器持有**，不要 `delete`；`Destroy(name)` 之后再访问就是野指针。

### 4.2 直接 new 子类（单元测试 / 只用一个通道）

```cpp
#include "udp_comm.h"

evs::comm::UdpComm server;
evs::comm::CommConfig cfg;
cfg.role = evs::comm::CommRole::kServer;
cfg.port = 9000;
server.SetReceiveCallback([](const evs::comm::CommFrame &f) { /* ... */ });
if (!server.Init(cfg) || !server.Start()) {
  std::cerr << server.last_error() << std::endl;
}
```

`CommBase` 不可拷贝、无移动（持有线程与 fd）；要放进容器请用 `unique_ptr`。

## 5. 线程与回调的三条硬约束

1. **收包回调在接收线程里同步执行**（每对象最多一条）。别在里面做重活、等锁、写盘；要做业务就投递到自己的队列。
2. **不要在回调里调 `Stop()` / `Destroy()` / `Clear()`** —— 接收线程 join 自己 → 永久卡死。要停请从别的线程调（或先把自己的线程标记退出）。
3. **`SetReceiveCallback` / `SetStateCallback` 在 `Start()` 之前注册**。运行期替换没有同步保护（`CommBase` 里是裸 `std::function` 赋值，与 HANDOFF §2 的"不过度设计"一致）。

状态回调可能在**两个地方**触发：`Init/Start/Stop` 的调用线程（状态切换）与接收线程（运行期出错进 `kError`）。回调里不要去碰正在被改的对象。

## 6. 错误处理约定

- 所有可能失败的入口返回 `bool` / 指针，**失败原因一律写 `last_error()`**（英文库错误码经 `mosquitto_strerror` 转成可读串）。
- `state()`：`kIdle`（未 Init）→ `kRunning` → `kStopped`；任何一步失败进 `kError`（可重新 `Init` 重试）。
- `Send()` 在未 `Start` 时返回 false（UDP 还没确定对端 / MQTT 未连 broker），这是**契约而非缺陷**；调用方必须检查返回值。

## 7. 为什么 UDP 不引第三方库（以及为什么没引 ZLToolKit）

| 方案 | 结论 |
| --- | --- |
| POSIX socket + `poll`（当前实现） | ✅ 零依赖、板端必然可用、`Stop()` 语义完全可控（`shutdown` 唤醒 `poll`） |
| ZLToolKit（ZLMediaKit 的 C++ 工具库） | ❌ **`install/` 里没有它的头文件**（只有 `ZLMediaKit/*.h` 的 C API 与 `libmk_api.so`、`libzlmediakit.a` 静态库）→ 无法编译，且违反 HANDOFF §2「不引入 `install/` 中没有的新第三方库」 |
| 自己上 `epoll` 多路复用 | 通道数上百时值得做；当前每通道一条接收线程（默认个位数通道）足够，`poll` 超时 200ms 也让 `Stop()` 足够灵敏 |

通信层沿用项目既有风格：第三方都走 **C API + pimpl**（与 `zlm_push_server` 用 `mk_api.h` 一致），头文件里不出现任何第三方类型。

## 8. 验收（T6）

```bash
./scripts/tests/run_t6_comm.sh          # 构建目录默认 out/，可用 BUILD=xxx 覆盖
```

覆盖 43 项断言：管理器工厂/命名/同名拒绝/查表/销毁 → UDP 回环（client→server、server 记住对端回包、重复 Start/Stop 幂等）→ MQTT（broker 在跑则真实 pub/sub 回环，否则 SKIP 并打印启动命令）。结果记录在 [TESTLOG §5.21](../../docs/history/TESTLOG.md)。

MQTT 分支需要本机 broker：

```bash
./install/rk3588/sbin/mosquitto -c /tmp/evs_mosq.conf -d   # conf 里至少 listener 1883 + allow_anonymous true
```

## 9. 已知限制（别当成已实现）

1. **没接进主链路**：`EdgeVisionService` 目前不创建任何通信通道（`main.cpp` 未改），comm 是独立可用库。
2. **没有消息分帧/协议**：`Send` 发的是裸字节；JSON/二进制协议、CRC、分片重组都留给上层。
3. **UDP server 只记住「最近一个」对端**：想同时回多个客户端要自己维护 `peer → 会话` 映射（或用 MQTT）。
4. **UDP 不重传、不排序**；MQTT 的可靠性靠 QoS（默认 0 = 最多一次）。
5. **TLS 未接线**：`CommConfig` 里没有证书字段，`libmosquitto` 支持但当前未暴露（`install/` 有 openssl）。
6. **服务端不起 broker**：见 §3；部署时要单独跑 `mosquitto` 进程（systemd 单元见 [COMMANDS.md §12](../../docs/ops/COMMANDS.md)）。
7. **无背压**：回调处理慢时 UDP 会直接丢包（内核缓冲满），MQTT 由 broker 的 inflight 队列兜（QoS>0 时）。


