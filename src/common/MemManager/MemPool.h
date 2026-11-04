#ifndef MEM_POOL_H
#define MEM_POOL_H

#include "MemMeta.h"
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <list>
#include <mutex>
#include <queue>
#include <string>
#include <unistd.h>
#include <unordered_map>

namespace pool
{

  enum class QueueType
  {
    OWNER, // 源头节点使用：从全局池拿空块
    USER   // 下游节点使用：从上游传过来的块
  };
  // 禁止拷贝基类
  class noncopyable
  {
  protected:
    noncopyable() {}
    ~noncopyable() {}

  private:
    // 禁止拷贝
    noncopyable(const noncopyable &that) = delete;
    noncopyable(noncopyable &&that) = delete;
    noncopyable &operator=(const noncopyable &that) = delete;
    noncopyable &operator=(noncopyable &&that) = delete;
  };

  class MemBlock : public noncopyable
  {
  public:
    MemBlock(MemMeta *_memMeta__ = nullptr) { _memMeta = _memMeta__; }
    ~MemBlock() {};

    MemMeta *data() { return _memMeta; }
    int userCnt();

    void setOwner(const std::string &owner);
    void addUser(const std::string &owner);

    void subUser(const std::string &name);
    void reset();

  private:
    void userAdd();
    void userSub();

  public:
    MemMeta *_memMeta;

  private:
    std::mutex _mutex;
    void *owner;
    std::string owner_name;
    int user_cnt = 0;
    std::list<std::string> user_list;
  };
  class MemQueue
  {
  public:
    MemQueue() {};
    ~MemQueue() {};

    void InitQueue(const std::string &name, int size = 20,
                   QueueType type_ = QueueType::USER);
    MemBlock *getBlock(uint64_t timeout = 100);
    MemBlock *getBlock(MemQueue *_queue, uint64_t timeout = 100);

    void pushFrame(MemBlock *_block);
    int pushFrame(MemQueue *queue_, MemBlock *block);

    void release(MemBlock *_block);
    void release(MemBlock *_block, const std::string &name);

    int Size();
    std::string getName();

  private:
    int readyRead(MemQueue *queue_);
    void push(MemBlock *_block);
    MemBlock *pull();

  public:
    std::condition_variable _queue_cond;

  private:
    std::mutex user_mutex;
    QueueType m_type;
    int max_cnt = 0;
    void *owner_ptr;
    bool _stopped = false;
    std::string owner_name;
    std::queue<MemBlock *> user_queue;
  };

  class MemPool
  {
  public:
    static MemPool &getInstance()
    {
      static MemPool instance;
      return instance;
    }
    // 禁止拷贝
    MemPool(const MemPool &) = delete;
    MemPool &operator=(const MemPool &) = delete;

    void addBuffer(MemMeta *_memMeta);
    void pushBlock(MemBlock *_block, const std::string &name);

    bool registerQueue(MemQueue *rQueue, std::string name);
    // ==========================================
    // 2. 申请一个块
    // ==========================================
    MemBlock *acquire(const std::string &name);
    MemBlock *delGetBlock();

    // ==========================================
    // 3. 归还一个块
    // ==========================================
    void release(MemBlock *block, const std::string &name);

  private:
    MemPool() = default;

    ~MemPool();

  private:
    // 数据区
    int frame_cnt = 0;
    std::vector<MemBlock *> all_blocks_;                   // 只读，无需加锁
    std::queue<MemBlock *> free_list_;                     // 高频读写
    std::unordered_map<std::string, MemQueue *> queueMap_; // 低频读写

    // 双锁分离
    std::mutex free_mtx_; // 保护 free_list_
  };
} // namespace pool

#endif