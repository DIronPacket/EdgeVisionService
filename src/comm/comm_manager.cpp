#include "comm_manager.h"

#include <utility>

#include "mqtt_comm.h"
#include "udp_comm.h"

namespace evs {
namespace comm {

CommManager::CommManager() {}

CommManager::~CommManager() {
  // 进程退出路径：先把所有接收线程停干净，避免线程还在跑而对象已析构
  Clear();
}

std::string CommManager::MakeUniqueName(const CommConfig &cfg) {
  const std::string base = std::string(ToString(cfg.type)) + "-" + ToString(cfg.role);
  for (int i = 1;; ++i) {
    const std::string candidate = base + "-" + std::to_string(i);
    if (comms_.find(candidate) == comms_.end()) {
      return candidate;
    }
  }
}

void CommManager::SetLastError(const std::string &err) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  last_error_ = err;
}

std::string CommManager::last_error() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return last_error_;
}

CommBase *CommManager::Create(const CommConfig &cfg) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);

  CommConfig resolved = cfg;
  if (resolved.name.empty()) {
    resolved.name = MakeUniqueName(resolved);
  }
  if (comms_.find(resolved.name) != comms_.end()) {
    last_error_ = "通道名已存在: " + resolved.name;
    return nullptr;
  }

  std::unique_ptr<CommBase> comm;
  switch (resolved.type) {
  case CommType::kUdp:
    comm.reset(new UdpComm());
    break;
  case CommType::kMqtt:
    comm.reset(new MqttComm());
    break;
  default:
    last_error_ = std::string("不支持的通信类型: ") + ToString(resolved.type);
    return nullptr;
  }

  // 绑定管理器级回调：把通道名一起带出去，上层不用自己维护 name→对象 的映射
  const std::string name = resolved.name;
  if (on_frame_) {
    ManagerReceiveCallback frame_cb = on_frame_;
    comm->SetReceiveCallback([frame_cb, name](const CommFrame &frame) {
      frame_cb(name, frame);
    });
  }
  if (on_state_) {
    ManagerStateCallback state_cb = on_state_;
    comm->SetStateCallback([state_cb, name](CommState state) {
      state_cb(name, state);
    });
  }

  if (!comm->Init(resolved)) {
    last_error_ = "创建通道 " + resolved.name + " 失败: " + comm->last_error();
    return nullptr;
  }

  CommBase *raw = comm.get();
  comms_[resolved.name] = std::move(comm);
  order_.push_back(resolved.name);
  last_error_.clear();
  return raw;
}

CommBase *CommManager::Create(CommType type, CommRole role, const std::string &host, int port) {
  CommConfig cfg;
  cfg.type = type;
  cfg.role = role;
  cfg.host = host;
  cfg.port = port;
  return Create(cfg);
}

CommBase *CommManager::Find(const std::string &name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::map<std::string, std::unique_ptr<CommBase>>::iterator it = comms_.find(name);
  return it == comms_.end() ? nullptr : it->second.get();
}

bool CommManager::Destroy(const std::string &name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::map<std::string, std::unique_ptr<CommBase>>::iterator it = comms_.find(name);
  if (it == comms_.end()) {
    last_error_ = "找不到通道: " + name;
    return false;
  }
  it->second->Stop();  // 先停线程，再让 unique_ptr 析构
  comms_.erase(it);
  for (size_t i = 0; i < order_.size(); ++i) {
    if (order_[i] == name) {
      order_.erase(order_.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  }
  return true;
}

bool CommManager::Start(const std::string &name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::map<std::string, std::unique_ptr<CommBase>>::iterator it = comms_.find(name);
  if (it == comms_.end()) {
    last_error_ = "找不到通道: " + name;
    return false;
  }
  if (!it->second->Start()) {
    last_error_ = "启动通道 " + name + " 失败: " + it->second->last_error();
    return false;
  }
  return true;
}

void CommManager::Stop(const std::string &name) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::map<std::string, std::unique_ptr<CommBase>>::iterator it = comms_.find(name);
  if (it != comms_.end()) {
    it->second->Stop();
  }
}

bool CommManager::StartAll() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  bool all_ok = true;
  for (size_t i = 0; i < order_.size(); ++i) {
    std::map<std::string, std::unique_ptr<CommBase>>::iterator it = comms_.find(order_[i]);
    if (it == comms_.end()) {
      continue;
    }
    if (!it->second->Start()) {
      all_ok = false;
      last_error_ = "启动通道 " + order_[i] + " 失败: " + it->second->last_error();
    }
  }
  return all_ok;
}

void CommManager::StopAll() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = 0; i < order_.size(); ++i) {
    std::map<std::string, std::unique_ptr<CommBase>>::iterator it = comms_.find(order_[i]);
    if (it != comms_.end()) {
      it->second->Stop();
    }
  }
}

void CommManager::Clear() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (size_t i = 0; i < order_.size(); ++i) {
    std::map<std::string, std::unique_ptr<CommBase>>::iterator it = comms_.find(order_[i]);
    if (it != comms_.end()) {
      it->second->Stop();
    }
  }
  comms_.clear();
  order_.clear();
}

std::vector<std::string> CommManager::names() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return order_;
}

size_t CommManager::size() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return comms_.size();
}

void CommManager::SetReceiveCallback(ManagerReceiveCallback cb) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  on_frame_ = std::move(cb);
}

void CommManager::SetStateCallback(ManagerStateCallback cb) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  on_state_ = std::move(cb);
}

}  // namespace comm
}  // namespace evs
