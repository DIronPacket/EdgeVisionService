#include "mqtt_comm.h"

#include <mosquitto/mosquitto.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

namespace evs {
namespace comm {

namespace {

// mosquitto_lib_init/cleanup 是**进程级全局**，不是对象级：
// 多个 MqttComm 实例必须引用计数，否则第一个对象析构就把库清了，其余实例立刻失效。
std::mutex g_lib_mutex;
int g_lib_refs = 0;

bool LibAcquire() {
  std::lock_guard<std::mutex> lock(g_lib_mutex);
  if (g_lib_refs == 0 && mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
    return false;
  }
  ++g_lib_refs;
  return true;
}

void LibRelease() {
  std::lock_guard<std::mutex> lock(g_lib_mutex);
  if (g_lib_refs > 0) {
    --g_lib_refs;
  }
  if (g_lib_refs == 0) {
    mosquitto_lib_cleanup();
  }
}

std::string MqttErr(int rc) { return std::string(mosquitto_strerror(rc)); }

int NextClientSeq() {
  static std::atomic<int> seq(0);
  return seq.fetch_add(1) + 1;
}

// 服务端角色收全网上报，客户端角色只收发给自己的命令
std::string DefaultTopic(CommRole role) {
  return role == CommRole::kServer ? "evs/v1/#" : "evs/v1/comm/cmd/#";
}

// 分片睡眠：Stop() 后不必等满一个重连间隔
void SleepInterruptible(const CommBase *self, int total_ms) {
  const int slice = 50;
  int left = total_ms;
  while (left > 0 && self->running()) {
    const int step = left < slice ? left : slice;
    std::this_thread::sleep_for(std::chrono::milliseconds(step));
    left -= step;
  }
}

}  // namespace

struct MqttComm::Impl {
  mosquitto *handle = nullptr;
  std::string host;
  int port = 1883;
  std::string broker;      // "host:port"，用于错误信息与 CommFrame::peer
  std::string sub_topic;
  std::string pub_topic;
  std::atomic<bool> connected;
  bool lib_acquired = false;

  Impl() : connected(false) {}
};

MqttComm::MqttComm() : CommBase(CommType::kMqtt), impl_(new Impl()) {}

MqttComm::~MqttComm() {
  Stop();  // 先停接收线程（虚函数仍指向本类）
  if (impl_->handle != nullptr) {
    mosquitto_destroy(impl_->handle);
    impl_->handle = nullptr;
  }
  if (impl_->lib_acquired) {
    LibRelease();
    impl_->lib_acquired = false;
  }
}

void MqttComm::OnConnectCb(mosquitto * /*mosq*/, void *userdata, int rc) {
  MqttComm *self = static_cast<MqttComm *>(userdata);
  if (self == nullptr) {
    return;
  }
  self->impl_->connected.store(rc == 0);
}

void MqttComm::OnDisconnectCb(mosquitto * /*mosq*/, void *userdata, int /*rc*/) {
  MqttComm *self = static_cast<MqttComm *>(userdata);
  if (self == nullptr) {
    return;
  }
  self->impl_->connected.store(false);
}

void MqttComm::OnMessageCb(mosquitto * /*mosq*/, void *userdata,
                           const mosquitto_message *msg) {
  MqttComm *self = static_cast<MqttComm *>(userdata);
  if (self == nullptr || msg == nullptr) {
    return;
  }
  CommFrame frame;
  frame.peer = self->impl_->broker;
  frame.topic = msg->topic != nullptr ? msg->topic : "";
  frame.data = static_cast<const uint8_t *>(msg->payload);
  frame.size = msg->payloadlen > 0 ? static_cast<size_t>(msg->payloadlen) : 0;
  self->EmitFrame(frame);
}


bool MqttComm::DoInit(const CommConfig &cfg, std::string *err) {
  const std::string host = cfg.host.empty() ? "127.0.0.1" : cfg.host;
  const int port = cfg.port == 0 ? 1883 : cfg.port;
  if (port < 1 || port > 65535) {
    *err = "MQTT port 必须在 1..65535 之间";
    return false;
  }
  if (cfg.qos < 0 || cfg.qos > 2) {
    *err = "MQTT qos 只能是 0/1/2";
    return false;
  }
  if (cfg.keepalive_sec < 0) {
    *err = "keepalive_sec 不能为负";
    return false;
  }

  // 重复 Init：先销毁上一次的句柄（库引用保持不变）
  if (impl_->handle != nullptr) {
    mosquitto_destroy(impl_->handle);
    impl_->handle = nullptr;
  }
  impl_->connected.store(false);

  if (!impl_->lib_acquired) {
    if (!LibAcquire()) {
      *err = "mosquitto_lib_init 失败";
      return false;
    }
    impl_->lib_acquired = true;
  }

  impl_->host = host;
  impl_->port = port;
  impl_->broker = host + ":" + std::to_string(port);
  impl_->sub_topic = cfg.topic.empty() ? DefaultTopic(cfg.role) : cfg.topic;
  if (!cfg.topic_pub.empty()) {
    impl_->pub_topic = cfg.topic_pub;
  } else if (impl_->sub_topic.find('#') != std::string::npos ||
             impl_->sub_topic.find('+') != std::string::npos) {
    // 通配符不能作为发布主题（broker 会拒绝）；订阅通配时默认发到本角色的 evt 主题
    impl_->pub_topic = cfg.role == CommRole::kServer ? "evs/v1/comm/evt/server"
                                                    : "evs/v1/comm/evt/client";
  } else {
    impl_->pub_topic = impl_->sub_topic;
  }

  std::string client_id = cfg.client_id;
  if (client_id.empty()) {
    client_id = "evs-comm-" + std::to_string(static_cast<long>(::getpid())) + "-" +
                std::to_string(NextClientSeq());
  }

  mosquitto *handle = mosquitto_new(client_id.c_str(), true /* clean_session */, this);
  if (handle == nullptr) {
    *err = "mosquitto_new 失败（内存不足或 client_id 非法: " + client_id + "）";
    return false;
  }

  mosquitto_connect_callback_set(handle, &MqttComm::OnConnectCb);
  mosquitto_disconnect_callback_set(handle, &MqttComm::OnDisconnectCb);
  mosquitto_message_callback_set(handle, &MqttComm::OnMessageCb);

  if (!cfg.username.empty()) {
    const int rc = mosquitto_username_pw_set(
        handle, cfg.username.c_str(), cfg.password.empty() ? nullptr : cfg.password.c_str());
    if (rc != MOSQ_ERR_SUCCESS) {
      *err = "设置用户名/密码失败: " + MqttErr(rc);
      mosquitto_destroy(handle);
      return false;
    }
  }

  // 重连退避参数（真正的重试由 ReceiveLoop 驱动，这里与它保持一致）
  mosquitto_reconnect_delay_set(handle, 1, 5, true);

  impl_->handle = handle;
  return true;
}

bool MqttComm::DoStart(std::string *err) {
  if (impl_->handle == nullptr) {
    *err = "mqtt 句柄未初始化";
    return false;
  }

  int rc = mosquitto_connect(impl_->handle, impl_->host.c_str(), impl_->port,
                             cfg().keepalive_sec);
  if (rc != MOSQ_ERR_SUCCESS) {
    *err = "连接 broker " + impl_->broker + " 失败: " + MqttErr(rc) +
           "（broker 起了吗？install/<平台>/sbin/mosquitto -c etc/mosquitto/mosquitto.conf）";
    return false;
  }
  impl_->connected.store(true);

  // 订阅失败不致命（可能 broker ACL 限制），但要让调用方知道
  rc = mosquitto_subscribe(impl_->handle, nullptr, impl_->sub_topic.c_str(), 0);
  if (rc != MOSQ_ERR_SUCCESS) {
    *err = "订阅 " + impl_->sub_topic + " 失败: " + MqttErr(rc);
    return false;
  }
  return true;
}

void MqttComm::DoStop() {
  // 让阻塞的 mosquitto_loop 立即返回；未连接时返回 MOSQ_ERR_NO_CONN，忽略即可
  if (impl_->handle != nullptr) {
    mosquitto_disconnect(impl_->handle);
  }
  impl_->connected.store(false);
}

bool MqttComm::DoSend(const void *data, size_t size, std::string *err) {
  if (impl_->handle == nullptr) {
    *err = "mqtt 句柄未初始化";
    return false;
  }
  if (!impl_->connected.load()) {
    *err = "MQTT 未连接 broker（" + impl_->broker + "），消息未发送";
    return false;
  }
  if (size > static_cast<size_t>(0x7fffffff)) {
    *err = "MQTT 单条消息过大";
    return false;
  }

  const int rc = mosquitto_publish(impl_->handle, nullptr, impl_->pub_topic.c_str(),
                                   static_cast<int>(size), data, cfg().qos, cfg().retain);
  if (rc != MOSQ_ERR_SUCCESS) {
    *err = "publish 到 " + impl_->pub_topic + " 失败: " + MqttErr(rc);
    return false;
  }
  return true;
}

void MqttComm::ReceiveLoop() {
  while (running()) {
    const int rc = mosquitto_loop(impl_->handle, cfg().recv_timeout_ms, 1);
    if (rc == MOSQ_ERR_SUCCESS) {
      continue;
    }
    if (!running()) {
      break;  // Stop() 里的 disconnect 让 loop 返回，属正常停止
    }

    if (rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST) {
      impl_->connected.store(false);
      // 断线重连：固定间隔重试，直到成功或 Stop()
      while (running() && !impl_->connected.load()) {
        SleepInterruptible(this, cfg().reconnect_delay_ms);
        if (!running()) {
          break;
        }
        const int again = mosquitto_reconnect(impl_->handle);
        if (again != MOSQ_ERR_SUCCESS) {
          continue;
        }
        impl_->connected.store(true);
        // clean_session=true 时 broker 丢弃了旧订阅，重连后必须重新订阅
        const int sub = mosquitto_subscribe(impl_->handle, nullptr, impl_->sub_topic.c_str(), 0);
        if (sub != MOSQ_ERR_SUCCESS) {
          SetLastError("重连后重新订阅 " + impl_->sub_topic + " 失败: " + MqttErr(sub));
        }
        break;
      }
      continue;
    }

    SetLastError("mosquitto_loop 返回错误: " + MqttErr(rc));
    break;
  }
}

}  // namespace comm
}  // namespace evs
