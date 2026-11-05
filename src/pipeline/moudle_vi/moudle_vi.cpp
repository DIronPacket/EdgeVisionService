#include "moudle_vi.h"
#include "MemPool.h"
#include <iostream>
#include <chrono>
ViNode::ViNode() {}

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
      // 记录当前帧的开始时间
      auto frame_start_time = std::chrono::steady_clock::now();
      int cnt = 0;
      if (m_capture->read_frame(fb))
      {
      }
      else
        continue;
      // 【核心逻辑】计算这一帧处理花了多少时间
      auto frame_end_time = std::chrono::steady_clock::now();

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