#include "udp_comm.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace evs {
namespace comm {

namespace {

std::string ErrnoText(const char *what) {
  return std::string(what) + " 失败: " + std::strerror(errno);
}

bool FillAddr(const std::string &host, int port, sockaddr_in *addr, std::string *err) {
  std::memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(static_cast<uint16_t>(port));
  if (host.empty()) {
    addr->sin_addr.s_addr = htonl(INADDR_ANY);  // 0.0.0.0：所有网卡
    return true;
  }
  if (inet_pton(AF_INET, host.c_str(), &addr->sin_addr) != 1) {
    *err = "非法 IPv4 地址: " + host + "（当前只支持点分十进制）";
    return false;
  }
  return true;
}

std::string AddrText(const sockaddr_in &addr) {
  char ip[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(static_cast<int>(ntohs(addr.sin_port)));
}

}  // namespace

// pimpl：sockaddr_in / pollfd 等平台类型只出现在本文件，对外头文件保持干净。
struct UdpComm::Impl {
  int fd = -1;
  sockaddr_in peer;                    // client: 服务端地址；server: 最近一次收到的对端
  bool has_peer = false;               // server 专用：是否收到过包（决定 Send 是否有目标）
  std::mutex peer_mutex;               // Send 在调用线程读、接收线程写
  std::vector<uint8_t> rx_buf;         // 收包缓冲（65536 = UDP 单包上限），复用避免每包分配

  Impl() { std::memset(&peer, 0, sizeof(peer)); }
};

UdpComm::UdpComm() : CommBase(CommType::kUdp), impl_(new Impl()) {}

UdpComm::~UdpComm() {
  Stop();  // 先让接收线程退出（此时虚函数仍指向本类），再关 fd
  if (impl_ && impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
}


bool UdpComm::DoInit(const CommConfig &cfg, std::string *err) {
  if (cfg.port < 1 || cfg.port > 65535) {
    *err = "UDP port 必须在 1..65535 之间";
    return false;
  }

  // 重新 Init 时先清掉上一次的 socket（Stop() 只 shutdown，不 close）
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->has_peer = false;
  impl_->rx_buf.assign(65536, 0);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    *err = ErrnoText("socket");
    return false;
  }

  int reuse = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    *err = ErrnoText("setsockopt(SO_REUSEADDR)");
    ::close(fd);
    return false;
  }
  if (cfg.udp_broadcast) {
    int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) {
      *err = ErrnoText("setsockopt(SO_BROADCAST)");
      ::close(fd);
      return false;
    }
  }

  sockaddr_in addr;
  if (!FillAddr(cfg.host, cfg.port, &addr, err)) {
    ::close(fd);
    return false;
  }

  if (cfg.role == CommRole::kServer) {
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      *err = ErrnoText("bind") + "（端口被占用？server 需要已有端口）";
      ::close(fd);
      return false;
    }
  } else {
    impl_->peer = addr;   // client 的目标地址在 Init 就固定下来
    impl_->has_peer = true;
  }

  impl_->fd = fd;
  return true;
}

bool UdpComm::DoStart(std::string *err) {
  if (!impl_ || impl_->fd < 0) {
    *err = "socket 未初始化";
    return false;
  }
  // server 已经在 Init 里 bind 好了，这里无事可做；client 需要 connect 才能用 send/recv
  if (cfg().role == CommRole::kClient) {
    if (::connect(impl_->fd, reinterpret_cast<sockaddr *>(&impl_->peer),
                  sizeof(impl_->peer)) != 0) {
      *err = ErrnoText("connect");
      return false;
    }
  }
  return true;
}

void UdpComm::DoStop() {
  // 不 close：shutdown 只把 socket 置为不可读写，能让阻塞的 poll 立即返回，
  // 之后仍可以再次 Start（重新 connect / 继续收）。
  if (impl_ && impl_->fd >= 0) {
    ::shutdown(impl_->fd, SHUT_RDWR);
  }
}

bool UdpComm::DoSend(const void *data, size_t size, std::string *err) {
  if (!impl_ || impl_->fd < 0) {
    *err = "socket 未初始化";
    return false;
  }
  if (size > 65507) {  // 65535 - 20(IP) - 8(UDP)
    *err = "单个 UDP 报文不能超过 65507 字节，请自行分片";
    return false;
  }

  if (cfg().role == CommRole::kClient) {
    const ssize_t sent = ::send(impl_->fd, data, size, 0);
    if (sent < 0) {
      *err = ErrnoText("send");
      return false;
    }
    if (static_cast<size_t>(sent) != size) {
      *err = "send 短写";
      return false;
    }
    return true;
  }

  // server：回给最近一次收到的对端（所以「先收后发」是硬约束）
  sockaddr_in target;
  {
    std::lock_guard<std::mutex> lock(impl_->peer_mutex);
    if (!impl_->has_peer) {
      *err = "server 还没有收到过任何报文，不知道回给谁（先等一帧或改用 client 模式）";
      return false;
    }
    target = impl_->peer;
  }
  const ssize_t sent = ::sendto(impl_->fd, data, size, 0,
                                reinterpret_cast<sockaddr *>(&target), sizeof(target));
  if (sent < 0) {
    *err = ErrnoText("sendto");
    return false;
  }
  if (static_cast<size_t>(sent) != size) {
    *err = "sendto 短写";
    return false;
  }
  return true;
}

void UdpComm::ReceiveLoop() {
  pollfd pfd;
  pfd.fd = impl_->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  while (running()) {
    pfd.revents = 0;
    // 超时而不是永久阻塞：running_ 变 false 后最迟 recv_timeout_ms 内退出
    const int ready = ::poll(&pfd, 1, cfg().recv_timeout_ms);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetLastError(ErrnoText("poll"));
      break;
    }
    if (ready == 0) {
      continue;  // 超时，回去检查 running()
    }
    if (!(pfd.revents & POLLIN)) {
      if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        break;  // Stop() 里的 shutdown 会走到这里
      }
      continue;
    }

    sockaddr_in from;
    std::memset(&from, 0, sizeof(from));
    socklen_t from_len = sizeof(from);
    const ssize_t got = ::recvfrom(impl_->fd, impl_->rx_buf.data(), impl_->rx_buf.size(), 0,
                                   reinterpret_cast<sockaddr *>(&from), &from_len);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (!running()) {
        break;  // shutdown 导致的失败，属于正常停止
      }
      SetLastError(ErrnoText("recvfrom"));
      break;
    }
    if (got == 0) {
      continue;  // 空数据报：忽略
    }

    if (cfg().role == CommRole::kServer) {
      // 记住对端：server 的下一次 Send 就发给它（按最近一次来信，单客户端场景足够）
      std::lock_guard<std::mutex> lock(impl_->peer_mutex);
      impl_->peer = from;
      impl_->has_peer = true;
    }

    CommFrame frame;
    frame.peer = AddrText(from);
    frame.topic = cfg().topic;
    frame.data = impl_->rx_buf.data();  // 只在回调期间有效
    frame.size = static_cast<size_t>(got);
    EmitFrame(frame);
  }
}

}  // namespace comm
}  // namespace evs
