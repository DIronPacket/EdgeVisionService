#ifndef EVS_COMM_MQTT_COMM_H
#define EVS_COMM_MQTT_COMM_H

#include <memory>

#include "comm_base.h"

// 只前向声明，不 include <mosquitto/mosquitto.h>：第三方类型不进公共头（HANDOFF §2.7）。
struct mosquitto;
struct mosquitto_message;

namespace evs {
namespace comm {

// MQTT 通信对象（基于 libmosquitto，纯 C API + pimpl）。
//
// ★ 关于「服务端/客户端」—— 这里必须说清楚，否则容易误解：
//   libmosquitto **只提供客户端 API**，本类不会自己起 broker。
//   kServer = 订阅通配主题（默认 "evs/v1/#"）接收各客户端上报的「订阅端角色」；
//   kClient = 订阅自己的下行主题（默认 "evs/v1/comm/cmd/#"），向服务端发布上报。
//   真正的 broker 是独立进程 install/<平台>/sbin/mosquitto（见 src/comm/README.md §3）。
//
// 断线重连：ReceiveLoop 里 mosquitto_loop 返回 NO_CONN / CONN_LOST 时，
// 按 reconnect_delay_ms 间隔重试 mosquitto_reconnect，成功后自动重新订阅。
class MqttComm : public CommBase {
public:
  MqttComm();
  ~MqttComm() override;

protected:
  bool DoInit(const CommConfig &cfg, std::string *err) override;
  bool DoStart(std::string *err) override;
  void DoStop() override;
  bool DoSend(const void *data, size_t size, std::string *err) override;
  void ReceiveLoop() override;

private:
  // libmosquitto 的回调：userdata 就是 MqttComm*（mosquitto_new 时传入）
  static void OnConnectCb(mosquitto *mosq, void *userdata, int rc);
  static void OnDisconnectCb(mosquitto *mosq, void *userdata, int rc);
  static void OnMessageCb(mosquitto *mosq, void *userdata, const mosquitto_message *msg);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace comm
}  // namespace evs

#endif  // EVS_COMM_MQTT_COMM_H
