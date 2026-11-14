#ifndef EVS_COMM_COMM_MANAGER_H
#define EVS_COMM_COMM_MANAGER_H

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "comm_base.h"
#include "comm_types.h"

namespace evs {
namespace comm {

// 全局通信服务管理类。
//
// 为什么需要它：通信对象有两种后端（udp/mqtt）、两种角色（server/client），
// 上层（main、未来的 evs_comm 进程、任意业务模块）不应该知道该 new 哪个子类。
// 这里集中做三件事：
//   1) 工厂：按 CommConfig.type 创建 UdpComm / MqttComm，并完成 Init；
//   2) 注册表：按 name 查找、启停、销毁，避免对象散落在各处导致忘记 Stop；
//   3) 全局回调：所有通道的收包/状态变化可以统一从一个入口处理。
//
// 用法（全局类，任意位置可取）：
//   auto &mgr = evs::comm::CommManager::getInstance();
//   mgr.SetReceiveCallback([](const std::string &name, const CommFrame &f) {
//       // name = 通道名，f.topic / f.peer / f.data / f.size
//   });
//   CommConfig cfg;
//   cfg.type = CommType::kUdp; cfg.role = CommRole::kServer;
//   cfg.host = "0.0.0.0"; cfg.port = 9000;
//   CommBase *server = mgr.Create(cfg);      // 创建 + Init（state=kStopped）
//   if (server) server->Start();             // 起接收线程
//
// 生命周期与线程：
//   - 单例进程级唯一，首次 getInstance() 构造，进程退出时析构（内部 StopAll）。
//   - 所有方法内部加锁，可从任意线程调用；**但不要在收包回调里调用 Destroy/Clear**
//     （对象在自己线程里 Stop 会 join 自己 → 卡死）。要停请从别的线程调。
//   - Find() 返回的是裸指针，管理器持有所有权；Destroy 之后该指针立即失效。
class CommManager {
public:
  // 全局回调：第一个参数是通道名（便于区分是哪个对象收到的）
  using ManagerReceiveCallback =
      std::function<void(const std::string &name, const CommFrame &frame)>;
  using ManagerStateCallback = std::function<void(const std::string &name, CommState state)>;

  static CommManager &getInstance() {
    static CommManager instance;
    return instance;
  }

  // 工厂：按 cfg.type 创建对应子类并 Init。
  //   - cfg.name 为空时自动命名 "<type>-<role>-<序号>"（如 udp-server-1）
  //   - 同名已存在 / 不支持的类型 / Init 失败 → 返回 nullptr，原因见 last_error()
  //   - 返回的指针由本管理器持有，不要 delete
  CommBase *Create(const CommConfig &cfg);

  // 便捷重载（其余字段取默认值）
  CommBase *Create(CommType type, CommRole role, const std::string &host, int port);

  CommBase *Find(const std::string &name);
  bool Destroy(const std::string &name);   // 先 Stop 再删除

  bool Start(const std::string &name);
  void Stop(const std::string &name);
  bool StartAll();                          // 任一失败返回 false（其余仍会启动）
  void StopAll();
  void Clear();                             // StopAll + 清空注册表

  std::vector<std::string> names() const;   // 保持创建顺序
  size_t size() const;

  void SetReceiveCallback(ManagerReceiveCallback cb);  // 之后 Create 的对象自动绑定
  void SetStateCallback(ManagerStateCallback cb);

  std::string last_error() const;

private:
  CommManager();
  ~CommManager();
  CommManager(const CommManager &) = delete;
  CommManager &operator=(const CommManager &) = delete;

  std::string MakeUniqueName(const CommConfig &cfg);   // 调用方需持锁
  void SetLastError(const std::string &err);

  // recursive_mutex：Init/Stop 内部会触发状态回调，回调里若再次查管理器不至于自死锁
  mutable std::recursive_mutex mutex_;
  std::map<std::string, std::unique_ptr<CommBase>> comms_;
  std::vector<std::string> order_;
  ManagerReceiveCallback on_frame_;
  ManagerStateCallback on_state_;
  std::string last_error_;
};

}  // namespace comm
}  // namespace evs

#endif  // EVS_COMM_COMM_MANAGER_H
