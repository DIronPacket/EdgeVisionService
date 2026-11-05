#ifndef MOUDLE_PRINT_H
#define MOUDLE_PRINT_H

#include "pipeline_node.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

// ==========================================
// 打印节点：流水线终点，汇总本帧各节点耗时与总耗时
//
// 典型链路： ViNode -> PushNode -> PrintNode
//   上游节点（Vi/Push）在处理前后往 block 上的 profile::NodeProfiler
//   写开始/结束时间戳，本节点取出后打印，然后把 block 归还内存池。
//
// 自身不参与耗时统计：它不做数据处理，且打印时自身尚未结束，
// 记进去只会让报表多出一条 running。
// ==========================================
class PrintNode : public PipelineNode {
public:
  PrintNode();

  ~PrintNode() override;

  // 重写基类的核心生命周期接口
  void start() override;
  void stop() override;
  void create(int channel_id, const std::string &name) override;
  void set_data(const char *config) override;

private:
  // 内部工作线程函数
  void run();

private:
  std::atomic<bool> running_;
  std::thread thread_;

  json cfg_;
  std::mutex cfg_mtx_;

  // 打印配置（来自 config/push.json 的 "print" 段，缺省也能工作）
  bool enable_ = true;
  int print_interval_ = 30; // 每 N 帧打印一次，1 表示逐帧打印

  uint64_t frame_no_ = 0;          // 已处理帧计数（只在 run() 线程访问）
  bool warned_no_profiler_ = false; // 缺耗时结构只告警一次

  // ---- 实测帧率 ----
  // 口径：上一行报表到本行报表之间的「帧数 / 墙钟时长」，即这一段窗口的
  // 平均产出速率（第一行报表从首帧算起，等价于累计平均）。
  // 用 steady_clock，和各节点耗时的时间基准一致；只在 run() 线程访问。
  std::chrono::steady_clock::time_point fps_last_tp_{}; // 窗口起点时刻
  uint64_t fps_last_frame_ = 0;                         // 窗口起点处的帧号，0 表示窗口未开始
};

#endif
