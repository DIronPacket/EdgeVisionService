#include "moudle_push.h"
#include "MemPool.h"
#include <iostream>

PushNode::PushNode() {}

PushNode::~PushNode() { stop(); }

void PushNode::start()
{
  if (running_.exchange(true))
    return;

  // 1. 初始化 ZLM
  init_zlm_push();

  // 2. 启动线程
  thread_ = std::thread(&PushNode::run, this);
  std::cout << "[" << get_full_name() << "] Push Node Started." << std::endl;
}

void PushNode::stop()
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

  // 释放 ZLM 资源
  if (m_handle)
  {
    zlm_rtsp_stop(m_handle);
    m_handle = nullptr;
  }
  std::cout << "[" << get_full_name() << "] Push Node Stopped." << std::endl;
}
void PushNode::create(int channel_id, const std::string &name)
{
  channel_id_ = channel_id;
  m_name = name + std::to_string(channel_id);
  // 启动时自己去 QueueManager 拿或建专属队列
  m_queue.InitQueue(m_name);
}

void PushNode::set_data(const char *config_str)
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

    // 3. 使用 JSON
    if (j.contains("width"))
    {
      int w = j["width"];
      std::cout << "Width: " << w << std::endl;
    }
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
// 在类的 private 成员里添加：
uint64_t frame_index_ = 0;
const uint64_t FRAME_INTERVAL_US = 33333; // 30fps 对应 33333 微秒
void PushNode::sendFrame(const uint8_t *pkt_data, size_t pkt_len,
                         CodingType type, uint64_t pts_us, void *user_arg)
{
  ZlmServerStreamHandle handle = static_cast<ZlmServerStreamHandle>(user_arg);
  // printf("send frame:  handle:%p  pkt_len:%d \n",handle,pkt_len);
  if (!handle || pkt_len == 0)
    return;
  ZlmFrameCodec codingType = (type == CodingType::VIDEO_CodingH264)
                                 ? ZLM_FRAME_VIDEO_H264
                                 : ZLM_FRAME_VIDEO_H265;
  // 【核心修复】抛弃外部传进来的系统时间戳，强制使用平滑递增的帧号时间戳
  uint64_t smooth_pts_us = frame_index_ * FRAME_INTERVAL_US;
  frame_index_++;
  int ret = zlm_rtsp_input_encodeData(handle, codingType, pkt_data, pkt_len,
                                      smooth_pts_us, smooth_pts_us);
  // uint8_t *safe_pkt = new uint8_t[pkt_len];
  // memcpy(safe_pkt, pkt_data, pkt_len);
  // int ret = zlm_rtsp_input_encodeData(handle, codingType, safe_pkt, pkt_len,
  //                                     smooth_pts_us, smooth_pts_us);
  // delete[] safe_pkt;
  if (ret != ZLM_ERR_OK)
  {
    std::cerr << "send rtsp frame err:" << ret << "\n";
    return;
  }
  // printf("    successfully send frame,pts:%llu\n", pts_us);
  fflush(stdout);
}
void PushNode::init_zlm_push()
{
  int server_code = 0;
  int outCode = 0;
  m_handle = zlm_rtsp_onekey_start(8550, ZLM_FRAME_VIDEO_H265, "live", "ch0", 1920, 1080, 30,
                                   nullptr, nullptr, &outCode);
  if (!m_handle)
  {
    std::cerr << "rtsp server create fail, code:" << server_code << std::endl;
    return;
  }
  enc.set_encode_callback(sendFrame, m_handle);
}

void PushNode::run()
{
  bool encoder_ready = false;
  while (running_.load())
  {
    pool::MemBlock *block = m_queue.getBlock(100);
    // printf("############# pushnode getBlock 1:%p \n", block);
    if (!block)
      continue;
    FrameBuffer *fb = block->_memMeta->data<FrameBuffer *>("nv12");
    if (fb)
    {
      // printf("############# push :%p,push-data:%p\n", (void *)fb, (void *)fb->data);
      if (!encoder_ready)
      {
        if (enc.open(fb->width, fb->height, CodingType::VIDEO_CodingH265, 30, 500))
        {
          encoder_ready = true;
        }
        else
        {
          printf("failed open enc\n");
          encoder_ready = false;
          continue;
        }
      }
      else
      {
        enc.encode(fb);
        // printf("successfully encode  frame\n");
      }
      // printf("push :%p\n", (void *)fb);
      // 假设上游节点把编码后的裸流数据放在了 "encoded_data" 槽位里
      // 你可以根据实际业务修改这里的 Ke

      // std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    push_input_pipe(block);
    // 终点节点，用完还回池子
    m_queue.release(block);
    fflush(stdout);
  }
}