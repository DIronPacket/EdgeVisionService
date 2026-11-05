#include "moudle_vi.h"
#include "MemPool.h"
#include "block_profiler.h"
#include <iostream>
#include <chrono>
// running_ 是 std::atomic<bool>，默认构造不写值（C++14），必须显式初始化，
// 否则内存里的脏值会让 start() 里 running_.exchange(true) 直接返回 true，线程起不来
ViNode::ViNode() : running_(false) {}

ViNode::~ViNode() { stop(); }

void ViNode::start()
{
  if (running_.exchange(true))
    return;

  // 2. 启动线程
  thread_ = std::thread(&ViNode::run, this);
  std::cout << "[" << get_full_name() << "] VI Node Started." << std::endl;
}

void ViNode::stop()
{
  if (!running_.exchange(false))
    return;

  // 唤醒阻塞的队列
  cv_.notify_all();

  // 等待线程退出
  if (thread_.joinable())
  {
    thread_.join();
  }

  std::cout << "[" << get_full_name() << "] VI Node Stopped." << std::endl;
}
void ViNode::create(int channel_id, const std::string &name)
{
  channel_id_ = channel_id;
  m_name = name + std::to_string(channel_id);
  m_queue.InitQueue(m_name, 20, pool::QueueType::OWNER);
}

void ViNode::set_data(const char *config_str)
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
    // 2. 解析 JSON
    json j = json::parse(config_str);

    // 兼容 {"push":{...}}、{"push":[{...}]} 与直接 {...} 三种写法
    if (j.contains("push"))
    {
      if (j["push"].is_object())
        j = j["push"];
      else if (j["push"].is_array() && !j["push"].empty())
        j = j["push"][0];
    }

    cfg_ = j;

    // 3. 采集输出分辨率 / 帧率：与 PushNode 共用同一份配置，
    //    保证「采集缩放后的尺寸」与「推流声明的尺寸」一致，否则播放端会花屏
    out_width_ = j.value("width", out_width_);
    out_height_ = j.value("height", out_height_);
    out_fps_ = j.value("fps", out_fps_);

    if (out_width_ <= 0 || out_height_ <= 0 ||
        (out_width_ % 2) != 0 || (out_height_ % 2) != 0)
    {
      std::cerr << "[ViNode] 输出分辨率非法: " << out_width_ << "x" << out_height_
                << "（需为正偶数），回退 1920x1080" << std::endl;
      out_width_ = 1920;
      out_height_ = 1080;
    }
    if (out_fps_ <= 0)
      out_fps_ = 30;

    std::cout << "[ViNode] 采集输出分辨率: " << out_width_ << "x" << out_height_
              << " fps=" << out_fps_ << std::endl;
  }
  catch (const json::parse_error &e)
  {
    // 4. 捕获解析异常，防止崩溃
    std::cerr << "JSON Parse Error: " << e.what() << std::endl;
  }
  catch (const json::type_error &e)
  {
    std::cerr << "JSON Type Error: " << e.what() << std::endl;
  }
}

void ViNode::run()
{
  std::string fileName = "/dev/video0:640:480:NV12:30"; //"2.mp4";
  bool is_open = false;
  m_capture = CreateCapture(CaptureType::CAPTURE_V4L2);

  // 【新增】设定目标帧率和帧间隔（帧率来自 config/push.json 的 fps）
  const int TARGET_FPS = (out_fps_ > 0) ? out_fps_ : 30;
  const auto frame_duration = std::chrono::microseconds(1000000 / TARGET_FPS); // 33333 微秒
  while (running_.load())
  {
    pool::MemBlock *block = m_queue.getBlock(100);
    // printf("vinode getBlock 1:%p \n", block);
    if (!block)
      continue;
    // printf("vinode getBlock 2:%p \n",block);
    FrameBuffer *fb = block->_memMeta->data<FrameBuffer *>("nv12");
    if (fb)
    {
      // printf("############# vi :%p,vi-data:%p\n", (void *)fb, (void *)fb->data);
      if (!is_open)
      {
        // 先把配置的分辨率设为采集层的缩放目标（sws 上下文在 open() 里按此创建）
        m_capture->set_output_size(out_width_, out_height_);
        if (!m_capture->open(fileName))
        {
          std::cerr << "open file failed:" << fileName << "\n";
          break;
        }
        printf("successfully open stream:%s, output=%dx%d\n", fileName.c_str(),
               out_width_, out_height_);
        is_open = true;
      }
      // 本节点耗时统计：开始时间戳（拿到 block 之后、真正开始采集之前）
      profile::NodeProfiler *profiler = profile::get_profiler(block);
      if (profiler)
        profiler->begin(m_name);
      // 记录当前帧的开始时间
      auto frame_start_time = std::chrono::steady_clock::now();
      if (!m_capture->read_frame(fb))
      {
        // 先写结束时间戳再 continue，否则本帧在本节点会一直显示 running
        if (profiler)
          profiler->end(m_name);
        // 取块必有归还（P0-3）：read_frame 失败常见于「配置 fps 高于采集端
        // 实际帧率」，此时 DQBUF 拿不到新帧直接返回 false。不归还的话内存池
        // 会被逐帧抽干，几十帧后源节点就永久卡在 MemPool::acquire() 上，
        // 整条流水线静默停摆（下游全部在空队列上等待）。
        m_queue.release(block);
        // 采集层通常已自带退避（V4L2 里是 usleep(2000)），这里只做通用兜底，
        // 保证换成别的采集源时也不会退化成忙轮询把 CPU 打满
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      // 【核心逻辑】计算这一帧处理花了多少时间
      auto frame_end_time = std::chrono::steady_clock::now();
      // 本节点耗时统计：结束时间戳
      // 必须放在下面的帧率节流 sleep 之前，否则会把等待下一帧的空闲时间
      // 也算进节点耗时（每帧恒定 33ms，失去统计意义）
      if (profiler)
        profiler->end(m_name);

      // 【修复】直接相减，不进行 duration_cast，让 auto 自动推导类型
      auto process_cost = frame_end_time - frame_start_time;
      // 1. 转换成毫秒（浮点数，可以保留小数，比如 1.23 ms）
      auto cost_ms = std::chrono::duration_cast<std::chrono::microseconds>(process_cost).count() / 1000.0;
      // printf("[Frame Cost] %.2f ms\n", cost_ms);
      // 如果这帧的处理耗时小于 33.3ms，就睡眠等待剩下的时间
      if (process_cost < frame_duration)
      {
        // 这里需要把两个时间单位对齐，直接做减法
        auto sleep_time = frame_duration - process_cost;
        cost_ms = std::chrono::duration_cast<std::chrono::microseconds>(sleep_time).count() / 1000.0;
        std::this_thread::sleep_for(sleep_time);
        // printf("             ------------ sleep %.2f ms \n", cost_ms);
      }
    }
    push_input_pipe(block);
    m_queue.release(block);
    fflush(stdout);
  }
  m_capture->close();
}