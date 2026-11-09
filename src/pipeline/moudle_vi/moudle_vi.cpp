#include "moudle_vi.h"
#include "MemPool.h"
#include "block_profiler.h"
#include <iostream>
#include <chrono>
#include <sstream>

namespace
{

// 把 vi.src 解析成「采集类型 + 采集实现认识的地址」（T7）。
// 判据是前缀，不是「字符串里有没有 /dev」（DESIGN §5.4）：
//   dev:            → V4L2（body 可写完整参数字段，也可只写设备路径）
//   file:           → FFmpeg 本地文件
//   rtsp/rtmp/http/udp → FFmpeg 网络流
//   无前缀          → 兼容 T7 之前的写法：/dev/video* 当 V4L2，其余当本地文件
bool ResolveSrc(const std::string &raw, int fps, CaptureType &type, std::string &real_src)
{
  const std::string dev_prefix = "dev:";
  const std::string file_prefix = "file:";

  if (raw.compare(0, dev_prefix.size(), dev_prefix) == 0)
  {
    type = CaptureType::CAPTURE_V4L2;
    const std::string body = raw.substr(dev_prefix.size());
    if (body.empty())
      return false;

    // V4l2Capture 的 src 契约是「设备:宽:高:格式:fps」共 5 段（v4l2_capture.cpp:21）。
    // 配置里通常只写设备路径（dev:/dev/video0），这里补上采集端参数：
    // 默认 640x480 NV12，与 T7 之前的默认采集参数一致。注意这不是输出尺寸——
    // 输出尺寸由 vi.width/height 经 set_output_size() 交给 sws 缩放，两者解耦。
    int segs = 1;
    for (size_t i = 0; i < body.size(); ++i)
    {
      if (body[i] == ':')
        ++segs;
    }
    if (segs == 5)
    {
      real_src = body; // 写全了参数就原样透传，不做二次解释
      return true;
    }
    if (segs != 1)
      return false; // 参数写了一半（例如 dev:/dev/video0:640）→ 直接报错，不猜
    std::ostringstream oss;
    oss << body << ":640:480:NV12:" << (fps > 0 ? fps : 30);
    real_src = oss.str();
    return true;
  }

  if (raw.compare(0, file_prefix.size(), file_prefix) == 0)
  {
    type = CaptureType::CAPTURE_FFMPEG;
    real_src = raw.substr(file_prefix.size());
    return !real_src.empty();
  }

  // 网络流前缀由 FfmpegCapture::open 自己再判一次（它按前缀设 options），这里只挑实现
  if (raw.compare(0, 6, "rtsp://") == 0 || raw.compare(0, 6, "rtmp://") == 0 ||
      raw.compare(0, 5, "http://") == 0 || raw.compare(0, 5, "udp://") == 0)
  {
    type = CaptureType::CAPTURE_FFMPEG;
    real_src = raw;
    return true;
  }

  if (raw.compare(0, 10, "/dev/video") == 0)
  {
    type = CaptureType::CAPTURE_V4L2;
    real_src = raw;
    return true;
  }

  type = CaptureType::CAPTURE_FFMPEG;
  real_src = raw;
  return !raw.empty();
}

} // namespace

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

    // T7：vi 节点优先读自己的 vi 段；v0 合成的 vi 段、以及老调用方式只有
    // push 段或直接 {...}，此时退回 push 段，保证 T7 之前的行为不变。
    // 同时兼容对象与单元素数组两种写法（{"vi":{...}} / {"vi":[{...}]}）。
    static const char *kSegKeys[] = {"vi", "push"};
    for (size_t i = 0; i < sizeof(kSegKeys) / sizeof(kSegKeys[0]); ++i)
    {
      const char *key = kSegKeys[i];
      if (!j.contains(key))
        continue;
      if (j[key].is_object())
        j = j[key];
      else if (j[key].is_array() && !j[key].empty() && j[key][0].is_object())
        j = j[key][0];
      break; // 命中 vi / push 段即以其为准（vi 优先）
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

    // 4. 采集源（T7）：只有显式写了 src 才覆盖，缺省仍走 T7 之前的 /dev/video0，
    //    所以 v0 push.json（合成 vi 段里没有 src）的行为逐字节不变。
    if (j.contains("src"))
    {
      if (!j["src"].is_string())
      {
        std::cerr << "[ViNode] src: 必须是字符串，忽略该字段，保持 " << m_src
                  << std::endl;
      }
      else
      {
        const std::string raw = j["src"].get<std::string>();
        CaptureType type = m_capture_type;
        std::string real_src;
        if (ResolveSrc(raw, out_fps_, type, real_src))
        {
          m_capture_type = type;
          m_src = real_src;
          std::cout << "[ViNode] 采集源: " << raw << " -> "
                    << (m_capture_type == CaptureType::CAPTURE_V4L2
                            ? "CAPTURE_V4L2"
                            : "CAPTURE_FFMPEG")
                    << " (" << m_src << ")" << std::endl;
        }
        else
        {
          // 不静默降级：配置写错时必须能从日志看出「实际用了哪个源」
          std::cerr << "[ViNode] 无法解析 src=\"" << raw
                    << "\"（支持 dev:/dev/video0、dev:/dev/video0:W:H:NV12:FPS、"
                       "file:/path/x.mp4、rtsp://…），保持默认 "
                    << m_src << std::endl;
        }
      }
    }
  }
  catch (const json::parse_error &e)
  {
    // 5. 捕获解析异常，防止崩溃
    std::cerr << "JSON Parse Error: " << e.what() << std::endl;
  }
  catch (const json::type_error &e)
  {
    std::cerr << "JSON Type Error: " << e.what() << std::endl;
  }
}

void ViNode::run()
{
  bool is_open = false;
  // T7：采集实现由 vi.src 前缀决定（set_data 里解析好），不再固定 V4L2
  m_capture = CreateCapture(m_capture_type);
  // P0-2：CAPTURE_V4L2 只在 RK 平台编入（capture_factory.cpp 用 TARGET_PLATFORM_RK3588
  // 包住了），x86 下工厂返回 nullptr。以前这里接着就 m_capture->open()，必然段错误；
  // 现在明确报错并退出本节点线程，让其余链路照常跑（N3：失败不拖垮全局）。
  if (!m_capture)
  {
    std::cerr << "[" << get_full_name() << "] 采集实现不可用（src=" << m_src
              << "）：当前平台未编译该采集后端，节点退出" << std::endl;
    return;
  }

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
        if (!m_capture->open(m_src))
        {
          // 取块必有归还（§2.4）：open 失败这条分支以前直接 break，已取的块没还，
          // 池子会少一块（长跑里反复重建链路就会累积成 P0-9 那类"越跑越少"）。
          m_queue.release(block);
          std::cerr << "[" << get_full_name() << "] open failed: " << m_src << std::endl;
          break;
        }
        std::cout << "[" << get_full_name() << "] successfully open stream:" << m_src
                  << ", output=" << out_width_ << "x" << out_height_ << std::endl;
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