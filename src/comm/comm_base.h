#ifndef EVS_COMM_COMM_BASE_H
#define EVS_COMM_COMM_BASE_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "comm_types.h"

namespace evs {
namespace comm {

// 通信对象基类 —— 把「生命周期 + 接收线程 + 状态 + 错误」这套样板收敛在一处，
// 子类只需要回答 5 个问题：
//
//   DoInit(cfg, err)   资源准备（UDP: socket/bind；MQTT: lib_init/new/参数）
//   DoStart(err)       真正建立连接（UDP client: connect；MQTT: connect + subscribe）
//   DoStop()           中止阻塞（UDP: shutdown；MQTT: disconnect），由 Stop() 在 join 前调用
//   DoSend(data,size)  发一帧；失败写 err
//   ReceiveLoop()      接收循环，直到 running()==false 才返回（Stop() 会先 DoStop() 唤醒它）
//
// 调用顺序（Init → Start → Send* → Stop）是强约束，Init/Start/Stop 都做了幂等处理：
// 重复 Start 直接返回 true；Stop 后可以再次 Init+Start。
//
// 线程模型：每个对象最多一条内部接收线程，收包回调在**该线程**里执行。
// 因此回调里不要再调 Stop()（自己 join 自己）；要停请从别的线程调。
class CommBase {
public:
  virtual ~CommBase();

  // 不可拷贝：持有线程与 fd/句柄（与 FrameBuffer 同样的理由）
  CommBase(const CommBase &) = delete;
  CommBase &operator=(const CommBase &) = delete;

  // 初始化。已处于 kRunning 时会先 Stop()。失败时 state=kError，原因见 last_error()。
  bool Init(const CommConfig &cfg);

  // 启动接收线程并建立连接。未 Init 会失败；重复调用返回 true。
  bool Start();

  // 停止接收线程并释放连接资源。可重复调用。
  void Stop();

  // 发送一帧（字节流）。返回 false 时原因见 last_error()。
  //   UDP  server: 发给「最近一次收到的对端」；client: 发给配置里的服务端
  //   MQTT 发布到 topic_pub（为空则用 topic）
  bool Send(const void *data, size_t size);
  bool SendText(const std::string &text);

  const CommConfig &config() const { return cfg_; }
  CommType type() const { return type_; }
  CommRole role() const { return cfg_.role; }
  CommState state() const { return state_.load(); }
  bool running() const { return running_.load(); }
  std::string name() const { return cfg_.name; }

  // 最近一次错误的可读描述（空 = 没出错）
  std::string last_error() const;

  // 注册回调。**必须在 Start() 之前注册**，运行期替换不保证线程安全。
  void SetReceiveCallback(ReceiveCallback cb) { on_frame_ = cb; }
  void SetStateCallback(StateCallback cb) { on_state_ = cb; }

protected:
  explicit CommBase(CommType type);

  // ---- 子类实现 ----
  virtual bool DoInit(const CommConfig &cfg, std::string *err) = 0;
  virtual bool DoStart(std::string *err) = 0;
  virtual void DoStop() = 0;
  virtual bool DoSend(const void *data, size_t size, std::string *err) = 0;
  virtual void ReceiveLoop() = 0;

  // ---- 供子类使用 ----
  const CommConfig &cfg() const { return cfg_; }
  void SetLastError(const std::string &err);
  void ClearLastError();
  void SetState(CommState state);
  // 把一帧交给用户回调（回调为空时丢弃；不抛异常给接收线程）
  void EmitFrame(const CommFrame &frame);

private:
  void RxThreadMain();
  void JoinRxThread();

  CommType type_;
  mutable std::mutex mutex_;   // 保护 cfg_ / last_error_
  std::string last_error_;

  CommConfig cfg_;
  std::atomic<CommState> state_;
  std::atomic<bool> running_;
  bool thread_started_;        // 只在 Stop()/Init() 这种单线程入口里读写

  std::thread rx_thread_;
  ReceiveCallback on_frame_;
  StateCallback on_state_;
};

}  // namespace comm
}  // namespace evs

#endif  // EVS_COMM_COMM_BASE_H
