#include "health_check.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace evs {
namespace upgrade {
namespace {

bool StartsWithNoCase(const std::string &s, const char *prefix) {
  const size_t n = std::strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
    if (a != b) return false;
  }
  return true;
}

}  // namespace

bool ParseRtspUrl(const std::string &url, std::string *host, int *port,
                  std::string *path) {
  if (host == nullptr || port == nullptr || path == nullptr) return false;
  if (!StartsWithNoCase(url, "rtsp://")) return false;

  const std::string rest = url.substr(7);
  const size_t slash = rest.find('/');
  const std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  *path = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);
  if (authority.empty()) return false;

  const size_t colon = authority.rfind(':');
  if (colon == std::string::npos) {
    *host = authority;
    *port = 554;
  } else {
    *host = authority.substr(0, colon);
    const std::string port_text = authority.substr(colon + 1);
    if (port_text.empty()) return false;
    const int p = std::atoi(port_text.c_str());
    if (p <= 0 || p > 65535) return false;
    *port = p;
  }
  return !host->empty();
}

HealthChecker::HealthChecker() {}

HealthChecker::HealthChecker(const HealthConfig &cfg) : cfg_(cfg) {}

HealthResult HealthChecker::CheckPort(int timeout_sec) const {
  HealthResult r;
  std::string host;
  int port = 0;
  std::string path;
  if (!ParseRtspUrl(cfg_.rtsp_url, &host, &port, &path)) {
    r.detail = "health.rtsp_url 非法（需要 rtsp://host[:port]/path）: '" +
               cfg_.rtsp_url + "'";
    return r;
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo *res = nullptr;
  const std::string port_text = std::to_string(port);
  const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &res);
  if (gai != 0) {
    r.detail = "解析 " + host + " 失败: " + gai_strerror(gai);
    return r;
  }

  bool connected = false;
  std::string last_err = "没有可用的地址";
  for (struct addrinfo *it = res; it != nullptr && !connected;
       it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) {
      last_err = std::string("socket 失败: ") + std::strerror(errno);
      continue;
    }
    // 非阻塞 connect + poll：避免在不可达地址上卡住 probe_timeout 之外的时间
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = ::connect(fd, it->ai_addr, it->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      pfd.revents = 0;
      const int wait_ms = (timeout_sec > 0 ? timeout_sec : 3) * 1000;
      rc = ::poll(&pfd, 1, wait_ms);
      if (rc == 0) {
        last_err = "连接超时（" + std::to_string(wait_ms) + " ms）";
      } else if (rc > 0) {
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) == 0 && so_err == 0) {
          connected = true;
        } else {
          last_err = std::string("connect 失败: ") + std::strerror(so_err);
        }
      } else {
        last_err = std::string("poll 失败: ") + std::strerror(errno);
      }
    } else if (rc == 0) {
      connected = true;
    } else {
      last_err = std::string("connect 失败: ") + std::strerror(errno);
    }
    ::close(fd);
  }
  ::freeaddrinfo(res);

  r.port_alive = connected;
  r.ok = connected;
  r.detail = connected ? ("TCP " + host + ":" + std::to_string(port) + " 可达")
                       : ("TCP " + host + ":" + std::to_string(port) + " 不可达: " +
                          last_err);
  return r;
}

HealthResult HealthChecker::CheckFps() const {
  HealthResult r;
  r.detail =
      "首帧/帧率探测尚未实现（骨架）：P3 用 ffprobe 子进程按 health.min_fps=" +
      std::to_string(cfg_.min_fps) + " 判定，probe_timeout_sec=" +
      std::to_string(cfg_.probe_timeout_sec) +
      "；板端 ffprobe 在 3rdparty/rk3588/ffmpeg/bin/ 下（不在 PATH），"
      "health.ffprobe_path 必须写绝对路径";
  return r;
}

HealthResult HealthChecker::CheckAll() const {
  HealthResult port = CheckPort();
  if (!port.ok) return port;
  HealthResult fps = CheckFps();
  fps.port_alive = true;
  return fps;
}

}  // namespace upgrade
}  // namespace evs
