#ifndef MEM_META_H
#define MEM_META_H

#include <mutex>
#include <string>
#include <typeinfo>
#include <unordered_map>
template<typename T>
class MemSlot
{
public:
  MemSlot(){};
  MemSlot(T _t){
    t=_t;
  }
  void setObj(T _t)
  {
    t=_t;
  }
  void freeObj()
  {
    delete t;
    t=nullptr;
  }
  T getObj()
  {
    return t;
  }
  T* getObjPtr()
  {
    return &t;
  }

private:
  T t;
};

// 负责挂载各种自定义数据的“附加数据池”
class MemMeta {
public:
  MemMeta() = default;

  // ==========================================
  // 1. 存放数据指针
  // ==========================================
  template <typename T> void set_ptr(const std::string &name, T ptr) {
    std::lock_guard<std::mutex> lock(mtx_);
    // 如果同名已存在，直接覆盖旧的指针（旧指针的生命周期由业务自己管理）
    MemSlot<T>* memSlot = new MemSlot<T>(ptr);
    data_map_[name] = memSlot;
  }

  // ==========================================
  // 2. 获取数据指针
  // ==========================================
  template <typename T> 
  T data(const std::string &name) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = data_map_.find(name);
    if (it == data_map_.end()) {
      return T();
    }

    MemSlot<T> *memSlot = (MemSlot<T>*)it->second;
    return memSlot->getObj();
  }

  // ==========================================
  // 3. 检查是否存在
  // ==========================================
  bool has(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return data_map_.find(name) != data_map_.end();
  }


private:
  std::unordered_map<std::string, void*> data_map_;
  mutable std::mutex mtx_;
};

#endif