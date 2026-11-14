# 三进程拆分设计（媒体 / 通信 / 升级）+ 进程间通信对象

> **状态：规划（未落地）**。本文只记录设计决策与落地顺序，代码尚未拆分。
> 最后更新：2026-11-12；适用分支：`rk3588`；校准提交：`ee046a3`。
> 相关文档：[ARCHITECTURE.md](../ARCHITECTURE.md)（当前单进程架构）· [ROADMAP.md](ROADMAP.md)（功能路线与 OTA）· [STATUS.md](../STATUS.md)（完成度与问题）· [TESTLOG.md](../history/TESTLOG.md)（验证记录）· [HANDOFF.md](../HANDOFF.md)（接手规范）

## 1. 背景与目标

当前仓库是**单进程**结构：`src/main/test/main_node.cpp`（`testnode`）装配整条链路，采集 → 预处理 → 编码 → 推流全部在同一个进程里。

演进方向是拆成 **3 个独立进程 + 1 个共享通信库**：

| 需求 | 含义 |
| --- | --- |
| 媒体处理独立成进程 | 采集/编码/推流；出问题时不能连带升级与通信一起挂 |
| 通信对象所有进程共用 | 一份 MQTT 客户端封装代码（`libcomm_api.so`），三进程各自实例化互不干扰 |
| 三个进程各自一个目录 | `src/media/` · `src/comm/` · `src/upgrade/` |
| 进程间只通过消息总线 | 不共享内存、不共享对象；共享的是**同一份库代码 + 同一套 topic 协议** |

设计原则：

1. **一个进程只做一件事**，崩溃域隔离（媒体崩了不影响升级与通信）。
2. **通信层是库，不是单例**：`MqttClient` 每个进程各持一个实例，禁止跨进程"共享对象"。
3. **topic 即接口**：进程间契约写在 `mqtt_topics.h` + 消息 schema 里，不靠头文件耦合。
4. **可回退**：拆分期间保留 `EdgeVisionService` 与 `testnode`，旧链路始终可跑。

## 2. 目标目录结构

```text
src/
├── CMakeLists.txt              # 只登记子目录
├── common/                     # 既有公共基础库 → libcommon_api.so（不动）
│   ├── MemManager/  configmgr/  preprocess/  profiler/  frame_mem.*  json.hpp
│   └── comm/                   # ★新增：通信对象（独立库 libcomm_api.so，三进程共用同一份代码）
│       ├── comm_events.h       # 事件/命令枚举（三进程共享语义）
│       ├── mqtt_topics.h       # 统一 topic 命名空间常量
│       ├── mqtt_message.h/.cpp # 消息体：topic + payload + qos + 时间戳
│       ├── mqtt_codec.h/.cpp   # payload 编解码（复用 common/json.hpp）
│       ├── mqtt_client.h/.cpp  # ★通信类本体：封装 libmosquitto，pimpl 隐藏 C 类型
│       └── CMakeLists.txt      # add_library(comm_api ${MODULE_LIB_TYPE} ...)
├── platform/                   # 平台后端 → platform_api（不动）
├── pipeline/                   # 媒体处理流水线节点 → pipeline_api（不动，moudle_* 保留原拼写）
│
├── media/                      # ★进程 1：媒体处理服务
│   ├── main.cpp                # 由 src/main/test/main_node.cpp 迁入
│   ├── media_service.h/.cpp    # 进程编排：读 node.json → PipelineBuilder → 启停 + SIGINT
│   └── CMakeLists.txt          # add_executable(evs_media ...)
├── comm/                       # ★进程 2：通信服务（目录已存在且为空，直接落地）
│   ├── main.cpp
│   ├── comm_service.h/.cpp     # broker 生命周期 + 命令路由 + 状态汇聚 + 健康检查
│   └── CMakeLists.txt          # add_executable(evs_comm ...)
├── upgrade/                    # ★进程 3：升级服务
│   ├── main.cpp
│   ├── upgrade_service.h/.cpp  # 版本查询 / 下载 / 校验 / 切换（先骨架）
│   └── CMakeLists.txt          # add_executable(evs_upgrade ...)
├── algorithm/                  # 既有空目录（预留算法域，本次不动）
└── main/                       # 保留：EdgeVisionService 兼容壳（空壳 main.cpp 不动）
```

产物：`out/bin/{evs_media, evs_comm, evs_upgrade}`；库 `out/lib/{libcommon_api.so, libcomm_api.so, libpipeline_api.so, platform_api}`。

`src/CMakeLists.txt` 目标形态：

```cmake
add_subdirectory(common)      # 内部再 add_subdirectory(comm) → comm_api
add_subdirectory(pipeline)
add_subdirectory(platform)
add_subdirectory(media)
add_subdirectory(comm)
add_subdirectory(upgrade)
add_subdirectory(main)        # 兼容壳，最后
```

## 3. 分层决策：`platform/` 与 `pipeline/` **不进** `media/`

**结论：不搬。** 三个理由，按硬度排序。

### 3.1 三种"变更原因"不能焊死在一条路径上

| 目录 | 本质 | 因为什么而改动 |
| --- | --- | --- |
| `platform/` | 硬件后端适配层（V4L2 / FFmpeg / MPP / ZLM） | 换平台、换芯片、换 SDK |
| `pipeline/` | 业务节点库（vi / vpss / push / print） | 加节点、改节点语义 |
| `media/` | 进程生命周期（配置装载、启停、信号、IPC 接入） | 进程编排需求 |

三者变化频率与责任人不同；塞进同一目录等于把三个变化轴耦合到一条路径上，未来第一件事就是抽出来。

### 3.2 会出现反向依赖（最硬的理由）

`src/comm`（通信服务）将来要枚举设备能力（分辨率、编码格式）上报；`src/upgrade`（升级服务）将来要暂停/恢复推流。若 `platform`/`pipeline` 藏在 `media/` 下：

```text
src/comm/    ──依赖──▶ src/media/platform/
src/upgrade/ ──依赖──▶ src/media/pipeline/
```

通信/升级进程反向依赖"媒体进程的私有目录"，目录结构立刻成环。`src/algorithm/` 这个既有空目录也说明作者预期过独立算法域——它同样需要 `encoder`/`decoder`。

### 3.3 CMake 传播链要重走一遍，P1-2 的坑会再踩

当前 ZLM/FFmpeg 头路径全局唯一的来源是这条链：

```text
rtsp ──(BUILD_RTSP_MODULE=ON)──▶ platform_api(INTERFACE) ──PUBLIC──▶ pipeline_api ──▶ 可执行文件
```

`src/platform/CMakeLists.txt:18` 的注释已写明：这条链断一次，`moudle_push`/`moudle_vi` 立即编译失败（历史坑见 `docs/history/TESTLOG.md` §5.18）。搬家要改 `src/CMakeLists.txt`、`pipeline/CMakeLists.txt` 里 `${PROJECT_SOURCE_DIR}/src/pipeline`、4 个 `moudle_*` 的相对路径，改完必须换新构建目录全量验证 —— 成本真实、收益为零。

### 3.4 判断规则（以后加目录直接套）

> **可复用的 = 顶层库目录；只服务一个进程且不可复用的 = 进程目录里。**

按此规则：`platform`/`pipeline` 是"库 → 顶层"，`media/main.cpp` + `media_service` 是"进程入口 → 放 `media/`"。

### 3.5 备选方案（若将来确实要重构）

| 方案 | 优点 | 缺点 | 结论 |
| --- | --- | --- | --- |
| A. `platform`+`pipeline` 搬进 `src/media/` | 物理上"媒体相关一眼看到" | 反向依赖、三轴耦合、必拆、传播链重走 | ❌ 不采用 |
| B. 现状 + `src/media/` 只放进程入口 | 复用边界清晰、CMake 零改动、增量最小 | 目录上看不出"媒体域" | ✅ **采用** |
| C. `src/libs/{common,platform,pipeline}` + `src/apps/{media,comm,upgrade}` | 业界标准的 libs/apps 分离 | 一次性大改，全部传播链重走 + 全量验证 | 🟡 三进程跑通后再单独立项 |

方案 B 下若想"看得见媒体域"，用两件事表达，不动文件位置：

```cmake
# src/media/CMakeLists.txt —— 媒体域聚合视图（纯声明，零成本）
add_library(media_domain INTERFACE)
target_link_libraries(media_domain INTERFACE pipeline_api platform_api)
```

```text
┌ 媒体域 ─────────────────────────┐   ┌ 通信域 ┐     ┌ 升级域 ┐
│ src/media      (进程入口)        │   │src/comm│     │src/upgrade│
│   ├ src/pipeline  (节点库)       │   │        │     │           │
│   └ src/platform  (后端库)       │   │        │     │           │
└─────────────────────────────────┘   └────────┘     └───────────┘
        共同依赖：src/common（含 comm/ 通信对象）
```

## 4. 三个进程的职责边界

| 进程 | 目录 | 产物 | 配置 | 主要依赖 | 职责 |
| --- | --- | --- | --- | --- | --- |
| 媒体处理 | `src/media/` | `evs_media` | `config/node.json` | common + comm + pipeline + platform | 采集/编码/推流；发 `media/evt/*`；收 `media/cmd/*` |
| 通信服务 | `src/comm/` | `evs_comm` | `config/comm.json` | common + comm | 拉起并守护 broker、命令路由、状态汇聚、心跳超时判定 |
| 升级服务 | `src/upgrade/` | `evs_upgrade` | `config/upgrade.json` | common + comm | 收 `upgrade/cmd/*`、上报 `upgrade/evt/*`、版本/包校验与切换 |

### 4.1 关于 broker 位置（重要事实）

`libmosquitto` **只有客户端**，broker 是独立可执行（本仓库已装到 `install/rk3588/sbin/mosquitto`）。因此 `evs_comm` 有两种形态：

- **A（倾向）**：`evs_comm` 用 `fork`+`exec` 拉起 `sbin/mosquitto -c etc/mosquitto/mosquitto.conf`，`waitpid` 守护并崩溃重启 —— "通信服务"确实自己管着 broker。
- **B**：broker 由 systemd / 容器入口单独拉起，`evs_comm` 只做路由与状态汇聚。

> 两者都保留，最终以 §9 待确认项 1 的结论为准。

## 5. 通信对象（`libcomm_api.so`）接口草案

C++14 约束（不用 `std::any`/`if constexpr`/结构化绑定）；`pimpl` 隐藏 `<mosquitto.h>`，对外头文件不暴露第三方类型（HANDOFF §2.7）。

```cpp
// src/common/comm/mqtt_client.h
namespace evs { namespace comm {

struct ClientOptions {
    std::string host            = "127.0.0.1";
    int         port            = 1883;
    std::string unix_socket;                  // 非空则走 UDS，本机进程间开销更低
    std::string client_id;                    // 建议 evs-<proc>-<pid>
    std::string username, password;
    bool        clean_session   = true;
    int         keepalive       = 30;
    int         reconnect_delay = 1;           // 秒
};

enum class ConnState { Disconnected, Connecting, Connected, Reconnecting };

using MessageHandler = std::function<void(const std::string& topic,
                                          const std::string& payload)>;
using StateHandler   = std::function<void(ConnState)>;

class MqttClient {
public:
    explicit MqttClient(const ClientOptions& opt);
    ~MqttClient();
    MqttClient(const MqttClient&)            = delete;   // 不可拷贝：持有 C 句柄与线程
    MqttClient& operator=(const MqttClient&) = delete;

    bool Start();                                       // lib_init → new → connect → 起 loop 线程
    void Stop();                                        // ★ disconnect → loop_stop → destroy → lib_cleanup
    bool Publish(const std::string& topic, const std::string& payload,
                 int qos = 0, bool retain = false);
    bool Subscribe(const std::string& topic, MessageHandler h, int qos = 0);
    bool IsConnected() const;
    void SetStateHandler(StateHandler h);

private:
    struct Impl;                                        // mosquitto 类型只出现在 .cpp
    std::unique_ptr<Impl> impl_;
};
}}
```

`Stop()` 的顺序注释不是装饰：**已实测**，未先 `mosquitto_disconnect()` 就调用 `mosquitto_loop_stop(m, false)` 会挂死；正确序列 `disconnect → loop_stop → destroy → lib_cleanup` 可正常退出。

### 5.1 topic 命名（写进 `mqtt_topics.h`，C++14 用 `static const char* const`）

```text
evs/v1/media/evt/{started,stopped,error,heartbeat}
evs/v1/media/cmd/{start,stop,set_osd}
evs/v1/upgrade/cmd/{check,download,apply}
evs/v1/upgrade/evt/{progress,done,failed}
evs/v1/comm/evt/{peer_online,peer_offline}
evs/v1/comm/cmd/{health}
```

约定：`evs/<协议版本>/<域>/<方向>/<动作>`；`evt` 上报、`cmd` 下发、`ack` 留作确认回执。

### 5.2 payload 约定

- 统一 JSON（复用 `src/common/json.hpp`，nlohmann 单头）；**不引入 protobuf/msgpack**（HANDOFF 硬约束：不引入 `install/` 中不存在的新库）。
- 必带字段：`v`（协议版本）、`ts`（毫秒时间戳）、`src`（进程名）。
- 单条建议 ≤ 4 KB；大块数据（帧、升级包）走文件路径，只把**句柄或路径**放进消息。

## 6. 第三方依赖现状（已实测，2026-11-12）

| 项 | 事实 |
| --- | --- |
| mosquitto | `2.0.22` 源码构建，前缀 `install/rk3588`；`bin/{mosquitto_pub,mosquitto_sub,mosquitto_rr,mosquitto_passwd}`、`sbin/mosquitto`（broker）、`etc/mosquitto/mosquitto.conf` |
| mosquitto 开关 | `WITH_TLS=ON` · `WITH_CJSON=OFF` · `WITH_UNIX_SOCKETS=ON` · `DOCUMENTATION=OFF` |
| libmosquitto | `libmosquitto.so.2.0.22`（SONAME `libmosquitto.so.1`），`NEEDED`：`libssl.so.3`/`libcrypto.so.3` → **动态**依赖 SSL |
| OpenSSL | `3.5.4`（`OpenSSL 3.5.4 30 Sep 2025`），源码 `3rdparty/rk3588/openssl-openssl-3.5.4`，装入 `install/rk3588` |
| OpenSSL 产物 | `libssl.so.3`（1252184 B）、`libcrypto.so.3`（6873736 B）、`libssl.a`、`libcrypto.a`、`include/openssl/*`、`lib/engines-3/`、`lib/cmake/`；**无 `openssl.pc`** |
| 与系统库的关系 | 非系统库拷贝：系统 `libssl.so.3` 为 630720 B、`libcrypto.so.3` 为 4053144 B，inode 与 mtime 均不同 |
| 符号兼容性 | `libmk_api.so` 仅需 `OPENSSL_3.0.0`；3.5.4 提供 `OPENSSL_3.0.0/3.0.3/3.0.8/3.0.9/3.1.0/3.2.0/3.3.0/3.4.0/3.5.0`（超集）→ 链接期不缺符号 |
| FFmpeg | `libavformat/libavcodec/...` 的 `NEEDED` 中**无** ssl/crypto，TLS 不相关 |

> 版本选择依据：3.5 是 LTS 分支（支持到 2030-04-08），3.5.4 是 2025 年最后一个补丁版，含 CVE-2025-9230/9231/9232 修复（其中 9231 是 64 位 ARM 上 SM2 的时序侧信道，与 RK3588 直接相关）。

需要注意的两点：

1. `install/rk3588/lib` 里放了 `libssl.so.3`，而运行脚本把该目录放进 `LD_LIBRARY_PATH`，**等效于全局把系统 OpenSSL 3.0.2 替换为 3.5.4**（含 ZLMediaKit 的 `libmk_api.so`）。符号层面兼容（见上表），但属于运行面变更，需实测；本次决定**暂不隔离**。
2. 缺 `openssl.pc` 意味着 pkg-config 找不到；CMake 侧改用 `lib/cmake/` 或显式变量：

```cmake
set(OPENSSL_ROOT_DIR ${CMAKE_SOURCE_DIR}/install/${TARGET_PLATFORM})
find_package(OpenSSL REQUIRED)          # 需要静态时再加 OPENSSL_USE_STATIC_LIBS ON
```

## 7. 落地顺序（4 步，每步可独立编译验证）

| 步骤 | 动作 | 验证方式 |
| --- | --- | --- |
| 1 | 建 `src/common/comm/`（6 个头/源 + CMakeLists）+ `cmake/FindMosquitto.cmake`；`comm_api` 只编库、不挂任何可执行 | `make` 通过；`nm -D out/lib/libcomm_api.so \| grep MqttClient` |
| 2 | `src/media/main.cpp` 由 `src/main/test/main_node.cpp` 迁入，新 target `evs_media`；`testnode` 保留 | `make` + `make run-node`（旧链路不回归）+ `./out/bin/evs_media` 冒烟 |
| 3 | `src/comm/` 落地 `evs_comm`：拉起/守护 broker + 订阅 `evs/v1/+/evt/#` 打印 | 起 `evs_comm` 后 `mosquitto_sub -t 'evs/#'` 能看到心跳 |
| 4 | `src/upgrade/` 落地骨架：订阅 `upgrade/cmd/check`，回 `upgrade/evt/done` | 手动 `mosquitto_pub` 一条 cmd，观察 evt 回包 |

每步完成后：按 [TESTLOG.md](../history/TESTLOG.md) 写 L2 记录（改代码/改配置没记录 = 未验证），并同步 [HANDOFF.md](../HANDOFF.md) §1.4 目录地图与 §1.9 任务索引、[ARCHITECTURE.md](../ARCHITECTURE.md) 的进程拓扑一节。

## 8. 已确认的构建坑（避免重复踩）

| 现象 | 根因 | 处理 |
| --- | --- | --- |
| mosquitto `make install` 卡在文档/man page | 默认构建文档需要 `xsltproc` | 加 `-DDOCUMENTATION=OFF` |
| `mosquitto_loop_stop(m, false)` 挂死 | 未先 `mosquitto_disconnect()` | 严格按 `disconnect → loop_stop → destroy → lib_cleanup` |
| `libmosquitto.so.1 => not found` | 非标准安装前缀 | 运行时带 `LD_LIBRARY_PATH=install/rk3588/lib` |
| 静态链 ssl 报 `relocation ... recompile with -fPIC` | 静态 `.a` 未用 PIC 编译 | OpenSSL 配置加 `-fPIC` |
| `find_library` 选到 `.so` 而非 `.a` | `libssl.a` 与 `libssl.so.3` 并存 | 需静态时显式给 `.a` 路径或 `-DOPENSSL_USE_STATIC_LIBS=TRUE` |
| aarch64 上 TLS 性能异常低 | 构建时加了 `no-asm`，丢失 ARMv8 AES/SHA 加速 | 不要加 `no-asm` |

## 9. 待确认项

1. `evs_comm` 是否自己 `fork/exec` 拉起 broker（形态 A），还是 broker 由 systemd/容器入口单独拉起（形态 B）。
2. 三个可执行命名：`evs_media / evs_comm / evs_upgrade`（与 `evs/v1` topic 前缀一致）还是沿用 `EdgeVisionService` 系列。
3. 媒体进程迁移后，`testnode` 是否保留为调试入口（建议保留至三进程稳定后）。
4. 通信对象是否需要 QoS 1 / retain 语义（影响 broker 配置与消息去重策略）。

## 10. 变更记录

| 日期 | 变更 |
| --- | --- |
| 2026-11-12 | 初版：记录三进程拆分目标、目标目录结构、`platform`/`pipeline` 不迁入 `media/` 的三条理由与判断规则、通信对象接口草案、topic 与 payload 约定、OpenSSL 3.5.4 / mosquitto 2.0.22 实测现状、4 步落地顺序与已知构建坑 |

[返回 ROADMAP](ROADMAP.md) · [返回 README](../../README.md)
