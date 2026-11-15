#ifndef EVS_UPGRADE_HEALTH_CHECK_H
#define EVS_UPGRADE_HEALTH_CHECK_H

#include <string>

namespace evs {
namespace upgrade {

// 健康检查配置（config/upgrade.json 的 health 段，§10.2）
struct HealthConfig {
  std::string rtsp_url;          // 例如 rtsp://127.0.0.1:8550/live/ch0
  std::string ffprobe_path;      // 空 = 走 PATH 找；板端实测只在 3rdparty/.../bin 下
  int probe_timeout_sec = 10;
  int min_fps = 15;
  int stable_sec = 60;           // 连续健康多久才 COMMIT（§7.1）
};

// 一次检查的结果。骨架期只填 port_alive / detail，fps 恒为 -1（未测）。
struct HealthResult {
  bool ok = false;
  bool port_alive = false;
  int fps = -1;                  // -1 = 未测（ffprobe 桩）
  std::string detail;
};

// 解析 rtsp://host[:port]/path。port 省略时给 554。
// URL 非法（不是 rtsp:// 或没有主机名）返回 false。
bool ParseRtspUrl(const std::string &url, std::string *host, int *port,
                  std::string *path);

// 健康判据（§7.1）的执行者：只**报告**健康与否，不重启服务（重启交给 systemd / 状态机）。
class HealthChecker {
 public:
  HealthChecker();
  explicit HealthChecker(const HealthConfig &cfg);

  void set_config(const HealthConfig &cfg) { cfg_ = cfg; }
  const HealthConfig &config() const { return cfg_; }

  // 判据 1：RTSP 端口 TCP 可达（不带协议，只证明"有人在监听"）
  HealthResult CheckPort(int timeout_sec = 3) const;

  // 判据 2/3：首帧 + 帧率 —— **桩**，P3 用 ffprobe 子进程实现
  HealthResult CheckFps() const;

  // 组合：端口不通直接不健康；端口通了才继续测帧率
  HealthResult CheckAll() const;

 private:
  HealthConfig cfg_;
};

}  // namespace upgrade
}  // namespace evs

#endif  // EVS_UPGRADE_HEALTH_CHECK_H
