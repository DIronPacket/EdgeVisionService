#ifndef MEM_META_H
#define MEM_META_H

#include <cstdio>
#include <mutex>
#include <string>
#include <typeinfo>
#include <unordered_map>

#include "frame_buffer.h"

// ==========================================
// 槽位里的裸指针怎么还回去
//   默认：delete 结构体 —— T 自己的析构负责收尾。FrameBuffer 就是这样：
//   它的数据不在结构体里（Allocate() 另外 new[] 了一块记在 owned_mem），
//   但 ~FrameBuffer() 会调 Release()，所以这里 delete 一句就够，不必认识它。
//
// 约定（加新类型时按这三条对号）：
//   1) T 必须是裸指针，且对象是 new 出来的（MemSlot<int> 这种值类型会在 delete 处编译不过）；
//   2) 能自己写析构的，就写进析构（首选，槽位这边零知识）；分配器不匹配、
//      不方便加析构的（malloc/free、new[]、C 库句柄），在**该类型旁边**特化 slot_free<T*>；
//   3) 只借用别人内存的视图（栈对象、BindMemory 帧）**不要放进槽位**：
//      槽位即所有权，clear()/析构会无条件 delete 它。
// 注：这里保留 frame_buffer.h 的 include —— MemPool.h 的使用者（moudle_push.cpp 等）
//     靠这条链拿到 FrameBuffer 的完整定义（data<FrameBuffer*> 需要完整类型）。
// ==========================================
template <typename T> inline void slot_free(T t) { delete t; }

// 新增：只为了让 void* 也能被安全销毁（虚析构）
class MemSlotBase
{
public:
  virtual ~MemSlotBase() {}
};

template <typename T>
class MemSlot : public MemSlotBase
{
public:
  MemSlot() : t(T()) {};
  MemSlot(T _t)
  {
    t = _t;
  }
  ~MemSlot()
  {
    freeObj();
  }
  void setObj(T _t)
  {
    t = _t;
  }
  void freeObj()
  {
    slot_free(t);
    t = nullptr;
  }
  T getObj()
  {
    return t;
  }
  T *getObjPtr()
  {
    return &t;
  }

private:
  T t;
};

// 负责挂载各种自定义数据的“附加数据池”
class MemMeta
{
public:
  MemMeta() = default;
  // 析构兜底：漏调 clear() 也不会把槽位里的对象留在堆上
  ~MemMeta() { clear(); }

  // 槽位本身就是所有权凭据，拷贝一份必然双删
  MemMeta(const MemMeta &) = delete;
  MemMeta &operator=(const MemMeta &) = delete;

  // ==========================================
  // 1. 存放数据指针（所有权随槽位转移）
  // ==========================================
  template <typename T>
  void set_ptr(const std::string &name, T ptr)
  {
    MemSlot<T> *memSlot = new MemSlot<T>(ptr);

    MemSlotBase *old_slot = nullptr;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto it = data_map_.find(name);
      if (it != data_map_.end())
      {
        // 同一个指针重复登记：不能释放对象，只把刚建的新槽位丢掉
        MemSlot<T> *same = dynamic_cast<MemSlot<T> *>(it->second);
        if (nullptr != same && same->getObj() == ptr)
        {
          memSlot->setObj(T()); // 先摘掉指针，免得新槽位析构把还在用的对象删了
          delete memSlot;
          return;
        }
        old_slot = it->second;
      }
      data_map_[name] = memSlot;
    }
    // 同名覆盖：旧槽位在这里归还，它的析构会把旧对象一起释放
    // （原来是纯覆盖，旧槽位连同里面的 FrameBuffer 一起丢 → P0-11）
    delete old_slot;
  }

  // ==========================================
  // 2. 获取数据指针
  // ==========================================
  template <typename T>
  T data(const std::string &name)
  {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = data_map_.find(name);
    if (it == data_map_.end())
    {
      return T();
    }

    // 类型不符返回空：原来是把 void* 硬转成 MemSlot<T>*，类型写错就是 UB（P1-10）
    MemSlot<T> *memSlot = dynamic_cast<MemSlot<T> *>(it->second);
    if (nullptr == memSlot)
    {
      std::fprintf(stderr, "[MemMeta] 槽位 \"%s\" 的类型不是 %s → 返回空\n",
                   name.c_str(), typeid(T).name());
      return T();
    }
    return memSlot->getObj();
  }

  // ==========================================
  // 3. 检查是否存在
  // ==========================================
  bool has(const std::string &name) const
  {
    std::lock_guard<std::mutex> lock(mtx_);
    return data_map_.find(name) != data_map_.end();
  }

  // ==========================================
  // 4. 统一释放所有槽位（FreeMem() 调一次就够）
  //    返回实际回收的对象个数，便于 FreeMem 打一行账
  //    先 swap 再释放：删除动作不进锁，也不给并发调用者看到半清状态
  // ==========================================
  size_t clear()
  {
    std::unordered_map<std::string, MemSlotBase *> old_slots;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      old_slots.swap(data_map_);
    }

    size_t released = 0;
    for (auto &item : old_slots)
    {
      if (nullptr != item.second)
      {
        // 虚析构 → MemSlot<T>::freeObj() → slot_free(t) 把对象一起还回去
        delete item.second;
        ++released;
      }
    }
    return released;
  }

private:
  // 存基类指针而不是 void*：void* 既不能安全 delete（UB、不调析构），
  // 也就走不到 freeObj()，槽位里的对象永远没人回收
  std::unordered_map<std::string, MemSlotBase *> data_map_;
  mutable std::mutex mtx_;
};

#endif