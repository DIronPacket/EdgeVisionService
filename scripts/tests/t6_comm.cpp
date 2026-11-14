// ============================================================
// T6 验收：通信层（CommBase / UdpComm / MqttComm / CommManager）
//
// 覆盖：
//   1. 管理器工厂：按 type 创建 udp/mqtt，按 role 区分 server/client，空 name 自动命名
//   2. UDP 本机回环：client → server → server 回包给 client（验证「记住对端」）
//   3. 生命周期幂等：重复 Start / 重复 Stop / Destroy 后 Find
//   4. 错误路径：非法 port、同名冲突、未 Start 就 Send
//   5. MQTT：127.0.0.1:1883 有 broker 就跑 pub/sub 回环，没有就 SKIP（不算失败）
//
// 不碰硬件、不开 RTSP、不推流。运行：./scripts/tests/run_t6_comm.sh
// 记录见 docs/history/TESTLOG.md（L2 · T6 通信层）。
// ============================================================

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "comm_manager.h"

namespace {

int g_fail = 0;

void Check(bool ok, const std::string &what) {
  std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
  if (!ok) {
    ++g_fail;
  }
}

// 等计数到 expected（或超时）。计数是 atomic，主线程读、接收线程写。
bool WaitCount(const std::atomic<int> &counter, int expected, int timeout_ms) {
  const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (counter.load() >= expected) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return counter.load() >= expected;
}

// 探测本机 TCP 端口是否有服务在听（用于判断 MQTT broker 是否存在）。
// 本机 connect 失败是立即返回的（RST），不需要 select 超时。
bool TcpPortOpen(const char *ip, int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return false;
  }
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  const bool parsed = inet_pton(AF_INET, ip, &addr.sin_addr) == 1;
  const bool ok =
      parsed && ::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
  ::close(fd);
  return ok;
}

std::string PayloadText(const evs::comm::CommFrame &frame) {
  return std::string(reinterpret_cast<const char *>(frame.data), frame.size);
}

}  // namespace

namespace {

using evs::comm::CommBase;
using evs::comm::CommConfig;
using evs::comm::CommFrame;
using evs::comm::CommManager;
using evs::comm::CommRole;
using evs::comm::CommState;
using evs::comm::CommType;

// ------------------------------------------------------------
// 1. 管理器：工厂 / 注册表 / 自动命名 / 错误路径
// ------------------------------------------------------------
void TestManagerBasics() {
  std::cout << "\n-- 1. CommManager --" << std::endl;
  CommManager &mgr = CommManager::getInstance();
  mgr.Clear();
  Check(mgr.size() == 0, "Clear() 后注册表为空");

  CommConfig udp_server;
  udp_server.name = "probe-udp";
  udp_server.type = CommType::kUdp;
  udp_server.role = CommRole::kServer;
  udp_server.host = "127.0.0.1";
  udp_server.port = 39201;
  CommBase *probe = mgr.Create(udp_server);
  Check(probe != nullptr, "Create(udp, server)");
  if (probe == nullptr) {
    std::cout << "  原因: " << mgr.last_error() << std::endl;
    return;
  }

  Check(probe->state() == CommState::kStopped, "Init 完成但未 Start → state=stopped");
  Check(!probe->SendText("x"), "未 Start 就 Send 必须失败");
  Check(mgr.Find("probe-udp") == probe, "Find 返回同一对象");
  Check(mgr.Create(udp_server) == nullptr, "同名创建被拒绝");
  Check(!mgr.last_error().empty(), "被拒绝时 last_error 非空: " + mgr.last_error());
  Check(mgr.Destroy("probe-udp"), "Destroy 成功");
  Check(mgr.Find("probe-udp") == nullptr, "Destroy 后 Find = nullptr");

  // 空 name → 自动命名 <type>-<role>-<序号>
  CommConfig anon;
  anon.type = CommType::kUdp;
  anon.role = CommRole::kServer;
  anon.host = "127.0.0.1";
  anon.port = 39202;
  CommBase *auto_named = mgr.Create(anon);
  Check(auto_named != nullptr && auto_named->name() == "udp-server-1",
        "空 name 自动命名为 udp-server-1");
  if (auto_named != nullptr) {
    mgr.Destroy(auto_named->name());
  }

  // 非法 port：Init 校验拦住
  CommConfig bad_port;
  bad_port.type = CommType::kUdp;
  bad_port.role = CommRole::kServer;
  bad_port.port = 0;
  Check(mgr.Create(bad_port) == nullptr, "port=0 创建失败（UDP 需要 1..65535）");

  // UDP client 不给 host：Init 校验拦住
  CommConfig bad_client;
  bad_client.type = CommType::kUdp;
  bad_client.role = CommRole::kClient;
  bad_client.port = 39201;
  Check(mgr.Create(bad_client) == nullptr, "UDP client 缺 host 创建失败");

  mgr.Clear();
}

// ------------------------------------------------------------
// 2. UDP 本机回环：client → server，server 记住对端后回包
// ------------------------------------------------------------
void TestUdp() {
  std::cout << "\n-- 2. UDP server/client 回环 --" << std::endl;
  CommManager &mgr = CommManager::getInstance();
  mgr.Clear();

  std::mutex mu;
  std::atomic<int> server_frames(0);
  std::atomic<int> client_frames(0);
  std::string server_text;
  std::string client_text;
  std::string server_peer;

  // 走管理器级回调：验证「统一入口 + 通道名」这条路径能拿到数据
  mgr.SetReceiveCallback([&](const std::string &name, const CommFrame &frame) {
    std::lock_guard<std::mutex> lock(mu);
    const std::string text = PayloadText(frame);
    if (name == "udp-srv") {
      server_text = text;
      server_peer = frame.peer;
      server_frames.fetch_add(1);
    } else if (name == "udp-cli") {
      client_text = text;
      client_frames.fetch_add(1);
    }
  });

  // 端口可能被占用：从 39210 起逐个试到成功
  CommConfig server_cfg;
  server_cfg.name = "udp-srv";
  server_cfg.type = CommType::kUdp;
  server_cfg.role = CommRole::kServer;
  server_cfg.host = "127.0.0.1";
  CommBase *server = nullptr;
  int port = 0;
  for (int candidate = 39210; candidate < 39260 && server == nullptr; ++candidate) {
    server_cfg.port = candidate;
    server = mgr.Create(server_cfg);
    if (server != nullptr) {
      port = candidate;
    }
  }
  Check(server != nullptr, "创建 UDP server（39210 起第一个可用端口）");
  if (server == nullptr) {
    std::cout << "  原因: " << mgr.last_error() << std::endl;
    return;
  }

  CommConfig client_cfg;
  client_cfg.name = "udp-cli";
  client_cfg.type = CommType::kUdp;
  client_cfg.role = CommRole::kClient;
  client_cfg.host = "127.0.0.1";
  client_cfg.port = port;
  CommBase *client = mgr.Create(client_cfg);
  Check(client != nullptr, "创建 UDP client（指向 127.0.0.1:" + std::to_string(port) + "）");
  if (client == nullptr) {
    std::cout << "  原因: " << mgr.last_error() << std::endl;
    return;
  }

  Check(!server->SendText("early"), "server 未收到首包前 Send 失败（不知道回给谁）");
  Check(server->Start(), "server Start");
  Check(client->Start(), "client Start");

  Check(client->SendText("ping"), "client → server 发送 ping");
  Check(WaitCount(server_frames, 1, 1000), "server 收到 1 帧");
  {
    std::lock_guard<std::mutex> lock(mu);
    Check(server_text == "ping", "server 收到内容 = ping");
    Check(server_peer.find("127.0.0.1:") == 0, "server 记录对端地址 = " + server_peer);
  }

  Check(server->SendText("pong"), "server → client 回 pong（发给最近对端）");
  Check(WaitCount(client_frames, 1, 1000), "client 收到 1 帧");
  {
    std::lock_guard<std::mutex> lock(mu);
    Check(client_text == "pong", "client 收到内容 = pong");
  }

  Check(server->Start(), "重复 Start 幂等返回 true");
  server->Stop();
  server->Stop();
  Check(server->state() == CommState::kStopped, "Stop 两次后 state=stopped");

  Check(mgr.Destroy("udp-srv"), "Destroy server");
  Check(mgr.Destroy("udp-cli"), "Destroy client");
  Check(mgr.size() == 0, "注册表已清空");

  // 回调捕获的是本函数的栈变量：必须在退出前解绑，否则下次触发就是悬空引用
  mgr.SetReceiveCallback(nullptr);
}

// ------------------------------------------------------------
// 3. MQTT：本机 1883 有 broker 才跑，没有就 SKIP（不算失败）
// ------------------------------------------------------------
void TestMqtt() {
  std::cout << "\n-- 3. MQTT server/client --" << std::endl;
  CommManager &mgr = CommManager::getInstance();
  mgr.Clear();
  if (!TcpPortOpen("127.0.0.1", 1883)) {
    std::cout << "[SKIP] 127.0.0.1:1883 无 broker；启动方式：\n"
                 "       install/rk3588/sbin/mosquitto -c "
                 "install/rk3588/etc/mosquitto/mosquitto.conf -d" << std::endl;
    return;
  }

  std::mutex mu;
  std::atomic<int> server_frames(0);
  std::atomic<int> client_frames(0);
  std::string server_text;
  std::string client_text;
  std::string server_topic;

  mgr.SetReceiveCallback([&](const std::string &name, const CommFrame &frame) {
    std::lock_guard<std::mutex> lock(mu);
    const std::string text = PayloadText(frame);
    if (name == "mqtt-srv") {
      server_text = text;
      server_topic = frame.topic;
      server_frames.fetch_add(1);
    } else if (name == "mqtt-cli") {
      client_text = text;
      client_frames.fetch_add(1);
    }
  });

  // 服务端角色：订阅 evs/v1/#（收所有上报），发布 evs/v1/comm/cmd/pong
  CommConfig server_cfg;
  server_cfg.name = "mqtt-srv";
  server_cfg.type = CommType::kMqtt;
  server_cfg.role = CommRole::kServer;
  server_cfg.host = "127.0.0.1";
  server_cfg.port = 1883;
  server_cfg.topic = "evs/v1/#";
  server_cfg.topic_pub = "evs/v1/comm/cmd/pong";
  CommBase *server = mgr.Create(server_cfg);
  Check(server != nullptr, "创建 MQTT server 角色（订阅 evs/v1/#）");
  if (server == nullptr) {
    std::cout << "  原因: " << mgr.last_error() << std::endl;
    return;
  }

  // 客户端角色：订阅 evs/v1/comm/cmd/#（收命令），发布到 media/evt（上报）
  //
  // 注意：这里刻意让「发布主题」不在「订阅主题」范围内。
  // MQTT 的语义是「订阅匹配上的都会收到，包括自己发的」——若 client 发布到
  // evs/v1/comm/cmd/ping，它自己也会先收到这条，测试就分不清是回包还是自收。
  CommConfig client_cfg;
  client_cfg.name = "mqtt-cli";
  client_cfg.type = CommType::kMqtt;
  client_cfg.role = CommRole::kClient;
  client_cfg.host = "127.0.0.1";
  client_cfg.port = 1883;
  client_cfg.topic = "evs/v1/comm/cmd/#";
  client_cfg.topic_pub = "evs/v1/media/evt/heartbeat";
  CommBase *client = mgr.Create(client_cfg);
  Check(client != nullptr, "创建 MQTT client 角色（订阅 evs/v1/comm/cmd/#）");
  if (client == nullptr) {
    std::cout << "  原因: " << mgr.last_error() << std::endl;
    return;
  }

  Check(server->Start(), "MQTT server Start（connect + subscribe）");
  Check(client->Start(), "MQTT client Start（connect + subscribe）");

  Check(client->SendText("hello-from-client"), "client 上报 hello-from-client");
  Check(WaitCount(server_frames, 1, 2000), "server 收到 client 的上报");
  {
    std::lock_guard<std::mutex> lock(mu);
    Check(server_text == "hello-from-client", "server 内容 = hello-from-client");
    Check(server_topic == "evs/v1/media/evt/heartbeat",
          "server 看到的主题 = " + server_topic + "（通配订阅命中）");
  }

  Check(server->SendText("hello-from-server"), "server publish hello-from-server");
  Check(WaitCount(client_frames, 1, 2000), "client 收到 server 的回包");
  {
    std::lock_guard<std::mutex> lock(mu);
    Check(client_text == "hello-from-server", "client 内容 = hello-from-server");
  }

  server->Stop();
  client->Stop();
  Check(server->state() == CommState::kStopped && client->state() == CommState::kStopped,
        "Stop 后两个通道都 = stopped");
  Check(mgr.Destroy("mqtt-srv"), "Destroy mqtt-srv");
  Check(mgr.Destroy("mqtt-cli"), "Destroy mqtt-cli");
  mgr.SetReceiveCallback(nullptr);
}

}  // namespace

int main() {
  std::cout << "=== T6 通信层验收（pid " << static_cast<long>(::getpid()) << "）===" << std::endl;
  TestManagerBasics();
  TestUdp();
  TestMqtt();
  CommManager::getInstance().Clear();

  std::cout << "\n结果: " << (g_fail == 0 ? "全部通过" : "存在失败") << "（fail=" << g_fail
            << "）" << std::endl;
  return g_fail == 0 ? 0 : 1;
}
