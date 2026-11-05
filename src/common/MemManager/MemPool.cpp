#include "MemPool.h"
#include "node_profiler.h"
namespace pool
{

  int MemBlock::userCnt()
  {
    std::lock_guard<std::mutex> lock(_mutex);
    return user_cnt;
  }
  void MemBlock::setOwner(const std::string &owner)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    owner_name = owner;
  }
  void MemBlock::addUser(const std::string &owner)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    userAdd();
    user_list.push_back(owner);
  }

  void MemBlock::subUser(const std::string &name)
  {
    std::lock_guard<std::mutex> lock(_mutex);
    userSub();
    user_list.remove(name);
  }

  void MemBlock::reset()
  {
    user_list.clear();
    owner = nullptr;
    owner_name.clear();
    // 块归还到池子时清空本帧的耗时记录，
    // 否则下一帧会读到上一帧残留的节点耗时
    if (_memMeta)
    {
      profile::NodeProfiler *profiler =
          _memMeta->data<profile::NodeProfiler *>(profile::kProfilerSlot);
      if (profiler)
        profiler->reset();
    }
  }

  void MemBlock::userAdd() { user_cnt++; }
  void MemBlock::userSub() { user_cnt--; }

  void MemQueue::InitQueue(const std::string &name, int size,
                           QueueType type_)
  {
    m_type = type_;
    max_cnt = size;
    owner_name = name;
    MemPool::getInstance().registerQueue(this, owner_name);
  }
  MemBlock *MemQueue::getBlock(uint64_t timeout)
  {
    return getBlock(this, timeout);
  }
  MemBlock *MemQueue::getBlock(MemQueue *_queue, uint64_t timeout)
  {
    if (_queue->m_type == QueueType::OWNER)
    {
      return MemPool::getInstance().acquire(_queue->getName());
    }
    std::unique_lock<std::mutex> lock(user_mutex);
    int size = Size();
    if (size <= 0)
    {
      if (timeout == 0)
      {
        return nullptr;
      }
      else if (timeout < 0)
      {

        _queue_cond.wait(lock, [this]
                         { return this->user_queue.size() > 0 || this->_stopped; });
      }
      else
      {
        // 超时等待
        if (!_queue_cond.wait_for(
                lock, std::chrono::milliseconds(timeout), [this]
                { return this->user_queue.size() > 0 || this->_stopped; }))
        {
          return nullptr;
        }
      }
    }
    if (user_queue.size() <= 0)
      return nullptr;
    MemBlock *p = user_queue.front();
    user_queue.pop();
    return p;
  }
  void MemQueue::release(MemBlock *_block) { return release(_block, owner_name); }
  void MemQueue::release(MemBlock *_block, const std::string &name)
  {
    std::lock_guard<std::mutex> lock(user_mutex);
    pool::MemPool::getInstance().release(_block, name);
  }
  int MemQueue::readyRead(MemQueue *queue_)
  {
    queue_->_queue_cond.notify_one();
    return 0;
  }
  void MemQueue::push(MemBlock *_block)
  {
    unsigned int size = this->Size();
    if (size >= max_cnt)
    {
      MemBlock *p = this->pull();
      release(p);
      printf("  ###   --- name:%s (size,max_cnt):(%d,%d) block:%p\n", getName().c_str(), size, max_cnt, (void *)p);
    }
    std::unique_lock<std::mutex> lock(user_mutex);
    MemPool::getInstance().pushBlock(_block, getName());
    user_queue.push(_block);
  }
  MemBlock *MemQueue::pull()
  {
    std::unique_lock<std::mutex> lock(user_mutex);
    MemBlock *p = user_queue.front();
    user_queue.pop();
    return p;
  }
  void MemQueue::pushFrame(MemBlock *_block)
  {
    pushFrame(this, _block);
  }
  int MemQueue::pushFrame(MemQueue *queue_, MemBlock *block)
  {
    queue_->push(block);
    return readyRead(queue_);
  }
  int MemQueue::Size() { return user_queue.size(); }
  std::string MemQueue::getName() { return owner_name; }

  void MemPool::addBuffer(MemMeta *_memMeta)
  {
    MemBlock *block = new MemBlock(_memMeta);
    block->reset();
    free_list_.push(block);
    frame_cnt++;
  }
  void MemPool::pushBlock(MemBlock *_block, const std::string &name)
  {
    std::unique_lock<std::mutex> lock(free_mtx_);
    _block->addUser(name);
  }
  bool MemPool::registerQueue(MemQueue *rQueue, std::string name)
  {
    if (!rQueue || name.empty() || queueMap_.count(name))
      return false;
    std::lock_guard<std::mutex> lock(free_mtx_);
    queueMap_.insert(std::pair<std::string, MemQueue *>(name, rQueue));
    return true;
  }
  // ==========================================
  // 2. 申请一个块
  // ==========================================
  MemBlock *MemPool::acquire(const std::string &name)
  {
    MemBlock *block = nullptr;

    if (free_list_.size() > 0)
    {
      {
        std::lock_guard<std::mutex> lock(free_mtx_);
        block = free_list_.front();
        free_list_.pop();
        frame_cnt--;
      }
      if (nullptr != block)
      {
        block->setOwner(name);
        block->addUser(name);
      }
    }
    return block;
  }
  MemBlock *MemPool::delGetBlock()
  {
    MemBlock *block = nullptr;
    if (free_list_.size() > 0)
    {
      std::lock_guard<std::mutex> lock(free_mtx_);
      block = free_list_.front();
      free_list_.pop();
      frame_cnt--;
    }
    return block;
  }
  // ==========================================
  // 3. 归还一个块
  // ==========================================
  void MemPool::release(MemBlock *block, const std::string &name)
  {
    if (!block)
      return;
    std::lock_guard<std::mutex> lock(free_mtx_);
    block->subUser(name);
    // printf("   --- [sub name : %s , block : %p]\n", name.c_str(), block);
    if (block->userCnt() == 0)
    {
      // printf("   --- [reset frame] name:%s block:%p \n", name.c_str(), block);
      block->reset();
      free_list_.push(block);
      frame_cnt++;
    }
  }

  MemPool::~MemPool()
  {
    // 析构时不用加锁，保证线程已全部退出即可
  }
} // namespace pool
