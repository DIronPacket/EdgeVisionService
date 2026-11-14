#include "comm_base.h"

#include <exception>
#include <utility>

namespace evs {
namespace comm {

CommBase::CommBase(CommType type)
    : type_(type), state_(CommState::kIdle), running_(false), thread_started_(false) {}

CommBase::~CommBase() {
  // 派生类析构已执行完，这里只保证线程被回收（Stop 里的 DoStop 是虚函数，
  // 此时已调到子类——所以正常路径应在子类析构前显式 Stop()，这里属于兜底）。
  running_.store(false);
  JoinRxThread();
}

bool CommBase::Init(const CommConfig &cfg) {
  if (running_.load()) {
    Stop();
  }

  std::string err;
  if (cfg.type != type_) {
    err = std::string("配置类型(") + ToString(cfg.type) + ") 与对象类型(" +
          ToString(type_) + ") 不一致";
  } else if (cfg.recv_timeout_ms < 1 || cfg.recv_timeout_ms > 5000) {
    err = "recv_timeout_ms 应在 1..5000 之间（决定 Stop() 的响应延迟）";
  } else if (cfg.role == CommRole::kClient && cfg.host.empty() && type_ == CommType::kUdp) {
    // UDP 客户端没有默认地址可回退；MQTT 客户端空 host 由子类补成 127.0.0.1
    err = "UDP client 必须提供 host（服务端地址）";
  }
  if (!err.empty()) {
    SetLastError(err);
    SetState(CommState::kError);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    cfg_ = cfg;
    last_error_.clear();
  }

  err.clear();
  if (!DoInit(cfg_, &err)) {
    if (!err.empty()) {
      SetLastError(err);
    } else if (last_error().empty()) {
      SetLastError("Init 失败（后端未给出原因）");
    }
    SetState(CommState::kError);
    return false;
  }

  SetState(CommState::kStopped);  // 已初始化、未启动
  return true;
}

bool CommBase::Start() {
  if (running_.load()) {
    return true;
  }
  if (state_.load() == CommState::kIdle) {
    SetLastError("Start 之前必须先 Init");
    SetState(CommState::kError);
    return false;
  }

  std::string err;
  if (!DoStart(&err)) {
    if (!err.empty()) {
      SetLastError(err);
    } else if (last_error().empty()) {
      SetLastError("Start 失败（后端未给出原因）");
    }
    SetState(CommState::kError);
    return false;
  }

  running_.store(true);
  try {
    rx_thread_ = std::thread(&CommBase::RxThreadMain, this);
    thread_started_ = true;
  } catch (const std::exception &e) {
    running_.store(false);
    DoStop();
    SetLastError(std::string("创建接收线程失败: ") + e.what());
    SetState(CommState::kError);
    return false;
  }

  SetState(CommState::kRunning);
  return true;
}

void CommBase::Stop() {
  if (!running_.load() && !thread_started_) {
    if (state_.load() == CommState::kRunning) {
      SetState(CommState::kStopped);
    }
    return;
  }

  running_.store(false);
  // 先让后端的阻塞点返回（UDP: shutdown；MQTT: disconnect），再 join —— 顺序反了会卡住。
  DoStop();
  JoinRxThread();
  SetState(CommState::kStopped);
}

bool CommBase::Send(const void *data, size_t size) {
  if (data == nullptr && size > 0) {
    SetLastError("Send: data 为空但 size>0");
    return false;
  }
  if (!running_.load()) {
    SetLastError("Send 之前必须先 Start");
    return false;
  }

  std::string err;
  if (!DoSend(data, size, &err)) {
    SetLastError(err.empty() ? "Send 失败（后端未给出原因）" : err);
    return false;
  }
  return true;
}

bool CommBase::SendText(const std::string &text) {
  return Send(text.data(), text.size());
}

std::string CommBase::last_error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_error_;
}

void CommBase::SetLastError(const std::string &err) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_error_ = err;
}

void CommBase::ClearLastError() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_error_.clear();
}

void CommBase::SetState(CommState state) {
  state_.store(state);
  if (on_state_) {
    // 用户在回调里抛异常不能打死接收线程
    try {
      on_state_(state);
    } catch (...) {
    }
  }
}

void CommBase::EmitFrame(const CommFrame &frame) {
  if (!on_frame_) {
    return;
  }
  try {
    on_frame_(frame);
  } catch (const std::exception &e) {
    SetLastError(std::string("收包回调抛异常: ") + e.what());
  } catch (...) {
    SetLastError("收包回调抛未知异常");
  }
}

void CommBase::RxThreadMain() {
  try {
    ReceiveLoop();
  } catch (const std::exception &e) {
    SetLastError(std::string("接收线程异常退出: ") + e.what());
    SetState(CommState::kError);
  } catch (...) {
    SetLastError("接收线程未知异常退出");
    SetState(CommState::kError);
  }
  // 正常退出（Stop 把 running_ 置 false）时不动 state：kStopped 由 Stop() 统一设置。
  running_.store(false);
}

void CommBase::JoinRxThread() {
  if (rx_thread_.joinable()) {
    rx_thread_.join();
  }
  thread_started_ = false;
}

}  // namespace comm
}  // namespace evs
