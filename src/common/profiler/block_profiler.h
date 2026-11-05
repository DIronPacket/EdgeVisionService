#ifndef BLOCK_PROFILER_H
#define BLOCK_PROFILER_H

#include "MemPool.h"
#include "node_profiler.h"

namespace profile
{

// ==========================================
// 从 block 上取出耗时统计结构
// 该结构由 MemManager::MemInit() 在初始化内存池时挂到每个 block 的 MemMeta 上，
// 随后随 block 在节点间传递。block / MemMeta / 槽位任一缺失都返回 nullptr。
// ==========================================
inline NodeProfiler *get_profiler(pool::MemBlock *block)
{
  if (!block || !block->_memMeta)
    return nullptr;
  return block->_memMeta->data<NodeProfiler *>(kProfilerSlot);
}

// ==========================================
// 作用域统计：构造时 begin()，析构时 end()
// 适用于「一次处理对应一段代码块」的场景，可避免提前 return 漏掉结束时间戳。
// 注意：若代码块里包含等待下一帧的 sleep，用它会把这部分空闲时间也算进去，
// 这种场景请改成显式 begin()/end() 把 sleep 排除在外。
// ==========================================
class NodeCostScope
{
public:
  NodeCostScope(NodeProfiler *profiler, const std::string &node_name)
      : profiler_(profiler), node_name_(node_name)
  {
    if (profiler_)
      profiler_->begin(node_name_);
  }

  ~NodeCostScope()
  {
    if (profiler_)
      profiler_->end(node_name_);
  }

private:
  NodeCostScope(const NodeCostScope &) = delete;
  NodeCostScope &operator=(const NodeCostScope &) = delete;

  NodeProfiler *profiler_;
  std::string node_name_;
};

} // namespace profile

#endif
