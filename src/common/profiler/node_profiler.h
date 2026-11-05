#ifndef NODE_PROFILER_H
#define NODE_PROFILER_H

#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>

namespace profile
{

// 单帧最多记录的节点个数（超出的节点会被丢弃并在报表里提示）
static const int kMaxStages = 8;
// 节点名最大长度（含结尾 '\0'）
static const int kStageNameLen = 32;
// 挂在 MemMeta 上的槽位名，见 MemManager::MemInit()
static const char *const kProfilerSlot = "profiler";

// 单调递增的微秒时间戳：steady_clock 不受系统时间调整影响，
// 适合做耗时统计（不能用 system_clock，NTP 校时会跳变）
inline uint64_t now_us()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// ==========================================
// 单个节点的耗时记录：节点名 + 开始/结束时间戳
// ==========================================
struct StageCost
{
  char name[kStageNameLen];
  uint64_t begin_us;
  uint64_t end_us;
  bool finished;

  StageCost() : begin_us(0), end_us(0), finished(false) { name[0] = '\0'; }

  uint64_t cost_us() const { return finished ? (end_us - begin_us) : 0; }
};

// ==========================================
// 一帧的耗时统计：挂在 MemBlock 的 MemMeta 上，随块在节点间传递
//
// 用法（每个节点）：
//   拿到 block 后   -> begin(节点名)
//   处理完、传下游前 -> end(节点名)
//
// 线程安全：block 在同一时刻只属于一个节点（节点间通过带锁的队列交接），
// 因此本类内部不加锁，避免热路径（每帧每节点两次调用）上的锁开销。
// ==========================================
class NodeProfiler
{
public:
  NodeProfiler() { reset(); }

  // 一帧开始 / 块回收时清空（由 MemBlock::reset() 调用）
  void reset()
  {
    count_ = 0;
    overflowed_ = false;
    for (int i = 0; i < kMaxStages; ++i)
    {
      stages_[i].name[0] = '\0';
      stages_[i].begin_us = 0;
      stages_[i].end_us = 0;
      stages_[i].finished = false;
    }
  }

  // 节点开始处理：记录节点名与开始时间戳
  // 返回 false 表示本帧记录已满（节点数超过 kMaxStages）
  bool begin(const std::string &node_name)
  {
    // 同名字段已存在（同帧内被再次处理）时复用记录，覆盖开始时间
    for (int i = 0; i < count_; ++i)
    {
      if (std::string(stages_[i].name) == node_name)
      {
        stages_[i].begin_us = now_us();
        stages_[i].end_us = 0;
        stages_[i].finished = false;
        return true;
      }
    }
    if (count_ >= kMaxStages)
    {
      overflowed_ = true;
      return false;
    }
    StageCost &s = stages_[count_++];
    std::strncpy(s.name, node_name.c_str(), kStageNameLen - 1);
    s.name[kStageNameLen - 1] = '\0';
    s.begin_us = now_us();
    s.end_us = 0;
    s.finished = false;
    return true;
  }

  // 节点处理结束：记录结束时间戳（重复调用只认第一次）
  bool end(const std::string &node_name)
  {
    for (int i = 0; i < count_; ++i)
    {
      if (std::string(stages_[i].name) == node_name)
      {
        if (!stages_[i].finished)
        {
          stages_[i].end_us = now_us();
          stages_[i].finished = true;
        }
        return true;
      }
    }
    return false;
  }

  int stage_count() const { return count_; }

  // 按记录顺序取第 i 条（调用方需保证 0 <= i < stage_count()）
  const StageCost &stage(int i) const { return stages_[i]; }

  // 各节点耗时之和
  uint64_t busy_us() const
  {
    uint64_t sum = 0;
    for (int i = 0; i < count_; ++i)
    {
      if (stages_[i].finished)
        sum += stages_[i].cost_us();
    }
    return sum;
  }

  // 首尾跨度：第一个节点开始 -> 最后一个节点结束（含节点间排队时间）
  uint64_t span_us() const
  {
    uint64_t first = 0;
    uint64_t last = 0;
    for (int i = 0; i < count_; ++i)
    {
      if (!stages_[i].finished)
        continue;
      if (first == 0 || stages_[i].begin_us < first)
        first = stages_[i].begin_us;
      if (stages_[i].end_us > last)
        last = stages_[i].end_us;
    }
    return (first != 0 && last > first) ? (last - first) : 0;
  }

  // 生成单行报表：各节点耗时(ms) + 节点合计 + 首尾跨度 [+ 实测帧率]
  // 仅列已结束的节点，未结束的（例如打印节点自己）标注 running
  // fps < 0 表示调用方没有帧率数据，此时不输出该段。本类不自己算帧率：
  // 帧率是"整条流水线的产出速率"，只有末端节点才知道（见 PrintNode）
  std::string report(int frame_no = -1, double fps = -1.0) const
  {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(3);

    if (frame_no >= 0)
      oss << "frame#" << frame_no << " ";
    oss << "耗时(ms):";
    for (int i = 0; i < count_; ++i)
    {
      const StageCost &s = stages_[i];
      oss << " " << s.name << "=";
      if (s.finished)
        oss << (static_cast<double>(s.cost_us()) / 1000.0);
      else
        oss << "running";
    }
    oss << " | 节点合计=" << (static_cast<double>(busy_us()) / 1000.0)
        << " 首尾跨度=" << (static_cast<double>(span_us()) / 1000.0);
    if (fps >= 0.0)
    {
      // 帧率只保留两位小数（沿用 fixed 格式，精度在流上是粘的）
      oss.precision(2);
      oss << " | 实测=" << fps << "fps";
    }
    if (overflowed_)
      oss << " [节点数超过 " << kMaxStages << "，超出部分未记录]";
    return oss.str();
  }

private:
  StageCost stages_[kMaxStages];
  int count_;
  bool overflowed_;
};

} // namespace profile

#endif
