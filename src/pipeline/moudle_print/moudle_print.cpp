#include "moudle_print.h"
#include "block_profiler.h"
#include <iostream>

PrintNode::PrintNode() : running_(false) {}

PrintNode::~PrintNode() { stop(); }

void PrintNode::start()
{
  if (running_.exchange(true))
    return;

  thread_ = std::thread(&PrintNode::run, this);
  std::cout << "[" << get_full_name() << "] Print Node Started." << std::endl;
}

void PrintNode::stop()
{
  if (!running_.exchange(false))
    return;

  // 等待线程退出（队列阻塞由 getBlock 的超时兜底）
  if (thread_.joinable())
  {
    thread_.join();
  }

  std::cout << "[" << get_full_name() << "] Print Node Stopped." << std::endl;
}

void PrintNode::create(int channel_id, const std::string &name)
{
  channel_id_ = channel_id;
  m_name = name + std::to_string(channel_id);
  // 消费上游传下来的块，与 PushNode 一致用 USER 队列
  m_queue.InitQueue(m_name, 20, pool::QueueType::USER);
}

void PrintNode::set_data(const char *config_str)
{
  std::lock_guard<std::mutex> lock(cfg_mtx_);
  // 1. 判空保护
  if (!config_str)
  {
    std::cerr << "Config string is null!" << std::endl;
    return;
  }

  try
  {
    // 2. 解析 JSON：只认 {"print": {...}} 这一层，
    //    避免和同一份配置里的 push 段混在一起
    json j = json::parse(config_str);
    if (!j.contains("print") || !j["print"].is_object())
    {
      std::cout << "[PrintNode] 配置中没有 print 段，使用默认值（每 "
                << print_interval_ << " 帧打印一次）" << std::endl;
      return;
    }

    json p = j["print"];
    cfg_ = p;

    enable_ = p.value("enable", enable_);
    print_interval_ = p.value("interval", print_interval_);

    if (print_interval_ <= 0)
    {
      std::cerr << "[PrintNode] interval 取值非法: " << print_interval_
                << "，回退 30" << std::endl;
      print_interval_ = 30;
    }

    std::cout << "[PrintNode] 打印配置已加载: enable="
              << (enable_ ? "true" : "false")
              << " interval=" << print_interval_ << "帧" << std::endl;
  }
  catch (const json::parse_error &e)
  {
    // 3. 捕获解析异常，防止崩溃
    std::cerr << "JSON Parse Error: " << e.what() << std::endl;
  }
  catch (const json::type_error &e)
  {
    std::cerr << "JSON Type Error: " << e.what() << std::endl;
  }
}

void PrintNode::run()
{
  while (running_.load())
  {
    pool::MemBlock *block = m_queue.getBlock(100);
    if (!block)
      continue;

    ++frame_no_;

    // 帧率窗口起点：首帧到达时初始化（fps_last_frame_ 为 0 表示窗口未开始）
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (fps_last_frame_ == 0)
    {
      fps_last_tp_ = now;
      fps_last_frame_ = frame_no_;
    }

    profile::NodeProfiler *profiler = profile::get_profiler(block);
    if (!profiler)
    {
      if (!warned_no_profiler_)
      {
        warned_no_profiler_ = true;
        std::cerr << "[" << get_full_name()
                  << "] block 上没有耗时统计结构（MemManager::MemInit() 未初始化？），"
                     "本节点只负责回收块"
                  << std::endl;
      }
    }
    else if (enable_ && (print_interval_ <= 1 ||
                         (frame_no_ % static_cast<uint64_t>(print_interval_)) == 0))
    {
      // 实测帧率：本行报表相对上一行报表的窗口平均帧率
      // （首行从首帧算起，等价于累计平均；窗口内帧数 = 帧间隔数）
      // elapsed 为 0 说明时钟还没来得及走（例如首帧 interval=1），
      // 此时保持 -1，report() 会跳过该段而不是输出 inf
      double fps = -1.0;
      const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  now - fps_last_tp_)
                                  .count();
      if (elapsed_us > 0)
      {
        fps = static_cast<double>(frame_no_ - fps_last_frame_) * 1000000.0 /
              static_cast<double>(elapsed_us);
        fps_last_tp_ = now;
        fps_last_frame_ = frame_no_;
      }

      std::cout << "[" << get_full_name() << "] "
                << profiler->report(static_cast<int>(frame_no_), fps) << std::endl;
    }

    // 终点节点：用完还回池子
    // （MemBlock::reset() 会顺带清空 profiler，下一帧从零开始）
    m_queue.release(block);
  }
}
