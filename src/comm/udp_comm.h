#ifndef EVS_COMM_UDP_COMM_H
#define EVS_COMM_UDP_COMM_H

#include <memory>

#include "comm_base.h"

namespace evs {
namespace comm {

// UDP 通信对象（POSIX socket，无第三方依赖）。
//
//   kServer：Init 里 socket + SO_REUSEADDR + bind(host:port)，随后 recvfrom 收包。
//            Send() 默认回到「最近一次收到的对端」——所以第一次 Send 之前必须先收到过包，
//            否则返回 false（不知道发给谁）。
//   kClient：Init 里只建 socket，Start 里 connect(host:port)；此后 send/recv 都固定该对端。
//            想同时收多个源请用 kServer。
//
// 头文件不出现 <netinet/in.h>：sockaddr 等平台类型全部藏在 .cpp 的 Impl 里（pimpl）。
class UdpComm : public CommBase {
public:
  UdpComm();
  ~UdpComm() override;

protected:
  bool DoInit(const CommConfig &cfg, std::string *err) override;
  bool DoStart(std::string *err) override;
  void DoStop() override;
  bool DoSend(const void *data, size_t size, std::string *err) override;
  void ReceiveLoop() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace comm
}  // namespace evs

#endif  // EVS_COMM_UDP_COMM_H
