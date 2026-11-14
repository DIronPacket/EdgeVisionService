#ifndef EVS_COMM_COMM_TYPES_H
#define EVS_COMM_COMM_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace evs {
namespace comm {

// 通信后端类型。新增后端时：这里加枚举值 → comm_manager.cpp 的 Create 加分支 → 写一个
// CommBase 子类（见 udp_comm.h / mqtt_comm.h 的样板）。
enum class CommType {
  kUdp = 0,   // UDP：数据报，无连接，适合局域网/本机高频小包
  kMqtt = 1,  // MQTT：走 broker 的发布/订阅，适合跨设备、弱网、需要重连与 QoS
};

// 连接角色。**同一个词在两种后端里的含义不完全相同，务必看清**：
//
//   UDP   kServer = bind 本地端口等待别人发来，Send() 默认回到「最近一次收到的对端」；
//         kClient = connect 到服务端（host:port），Send() 固定发给服务端。
//   MQTT  libmosquitto 只有客户端 API，broker 是独立进程（install/<平台>/sbin/mosquitto）。
//         所以这里的「服务端」= 订阅通配主题、接收各客户端上报的**订阅端角色**，
//         不是自己当 broker（详见 src/comm/README.md §3）。
enum class CommRole {
  kServer = 0,
  kClient = 1,
};

// 生命周期状态。kError 只表示上一次 Init/Start 失败，可再次 Init 重试。
enum class CommState {
  kIdle = 0,     // 未初始化
  kRunning = 1,  // 已初始化且接收线程在跑
  kStopped = 2,  // 初始化过、已被 Stop
  kError = 3,    // 初始化或运行期出错（原因见 last_error()）
};

const char *ToString(CommType type);
const char *ToString(CommRole role);
const char *ToString(CommState state);

// 字符串 → 枚举（配置解析用）。不区分大小写；未知值返回 false 且不写 *out。
bool ParseCommType(const std::string &text, CommType *out);
bool ParseCommRole(const std::string &text, CommRole *out);

// 一份通用通信配置。字段是两种后端并集，子类只读自己关心的部分
// （注释里标了 UDP / MQTT 各自的归属），这样 CommManager 的工厂只需要一个入参结构。
struct CommConfig {
  // ---- 通用 ----
  std::string name = "";             // 管理器内唯一名；空 = 自动生成 "udp-server-1"
  CommType type = CommType::kUdp;
  CommRole role = CommRole::kClient;

  // ---- 地址（UDP 与 MQTT 都用）----
  // kClient：对端/服务端地址，必填（UDP 为空会 Init 失败；MQTT 为空默认 127.0.0.1）
  // kServer：监听地址，UDP 空 = 0.0.0.0（所有网卡）；MQTT 里是 broker 地址，空 = 127.0.0.1
  std::string host = "";
  int port = 0;                      // 0 = 按后端取默认（UDP 必填，MQTT 默认 1883）

  // ---- MQTT 专属 ----
  std::string topic = "";            // 订阅主题；空 = server 取 "evs/v1/#"，client 取 "evs/v1/comm/cmd/#"
  std::string topic_pub = "";        // 发布主题；空 = 使用 topic
  std::string client_id = "";        // 空 = "evs-comm-<pid>-<序号>"
  std::string username = "";         // 空 = 匿名（broker 允许时）
  std::string password = "";
  int keepalive_sec = 30;            // 心跳周期；0 = 关闭
  int qos = 0;                       // 发布 QoS：0/1/2（订阅固定 QoS 0，避免 broker 不支持时失败）
  bool retain = false;               // 发布是否保留最后一条
  int reconnect_delay_ms = 1000;     // 断线后重连间隔

  // ---- UDP 专属 ----
  bool udp_broadcast = false;        // true = 打开 SO_BROADCAST（仅 server 侧有意义）

  // ---- 接收循环 ----
  int recv_timeout_ms = 200;         // 单次等待超时：决定 Stop() 的响应延迟（100~500 合适）
};

// 收到的一帧。**data 只在回调执行期间有效**（指向内部接收缓冲），
// 需要留存请自己拷走 —— 这样可以做到收包零分配。
struct CommFrame {
  std::string peer;        // 来源：UDP 为 "ip:port"；MQTT 为 broker 地址
  std::string topic;       // MQTT 主题；UDP 填配置里的 topic（可为空）
  const uint8_t *data = nullptr;
  size_t size = 0;
};

// 收包回调：在**接收线程**里同步调用，不要在回调里做重活/等锁/长期阻塞。
using ReceiveCallback = std::function<void(const CommFrame &)>;
// 状态变化回调：同样在接收线程或调用 Init/Start/Stop 的线程里触发。
using StateCallback = std::function<void(CommState)>;

}  // namespace comm
}  // namespace evs

#endif  // EVS_COMM_COMM_TYPES_H
