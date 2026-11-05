#include "moudle_push.h"
#include "MemPool.h"
#include <cctype>
#include <iostream>

namespace {
// 配置字符串统一转小写，容忍 h264/H264/AVC 等写法
std::string lower(const std::string &s) {
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) {
    r[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(r[i])));
  }
  return r;
}
} // namespace

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

bool PushNode::apply_codec(const std::string &codec) {
  const std::string c = lower(codec);
  if (c == "h264" || c == "avc") {
    codec_type_ = ZLM_FRAME_VIDEO_H264; // 推流侧按 H264 声明
    codec_coding_ = VIDEO_CodingH264;   // MPP 编码器按 H264 打开
    return true;
  }
  if (c == "h265" || c == "hevc") {
    codec_type_ = ZLM_FRAME_VIDEO_H265;
    codec_coding_ = VIDEO_CodingH265;
    return true;
  }
  std::cerr << "[PushNode] codec 取值非法: \"" << codec
            << "\"，可选 h264 / h265，保持原值" << std::endl;
  return false;
}

bool PushNode::apply_protocol(const std::string &protocol,
                              const std::string &transport) {
  std::string proto = lower(protocol);
  std::string trans = lower(transport);

  // 允许把承载方式直接写进 protocol（rtsp / tcp / udp / rtmp）
  if (proto == "tcp" || proto == "udp") {
    trans = proto;
    proto = "rtsp";
  }
  if (trans != "tcp" && trans != "udp") {
    std::cerr << "[PushNode] transport 取值非法: \"" << transport
              << "\"，可选 tcp / udp，按 tcp 处理" << std::endl;
    trans = "tcp";
  }

  protocol_ = proto;
  transport_ = trans;

  if (proto != "rtsp") {
    // 后端 platform/rtsp 只封装了 ZLMediaKit 的 RTSP 服务端；
    // RTMP / 裸 RTP 需要重新封装 mk_media 的推流能力，这里明确报错而不是静默忽略
    std::cerr << "[PushNode] 推流协议 \"" << proto
              << "\" 当前后端未实现（platform/rtsp 仅封装 RTSP 服务端），"
                 "该路推流将被跳过" << std::endl;
    return false;
  }
  return true;
}

void PushNode::set_data(const char *config_str) {
  std::lock_guard<std::mutex> lock(cfg_mtx_);
  // 1. 判空保护
  if (!config_str) {
    std::cerr << "Config string is null!" << std::endl;
    return;
  }

  try {
    // 2. 解析 JSON
    json j = json::parse(config_str);

    // 兼容 {"push":{...}}、{"push":[{...}]} 与直接 {...} 三种写法
    if (j.contains("push")) {
      if (j["push"].is_object()) {
        j = j["push"];
      } else if (j["push"].is_array() && !j["push"].empty()) {
        j = j["push"][0];
      }
    }

    cfg_ = j;

    // 3. 基础参数：缺字段时沿用当前值，允许只写关心的几项
    enable_ = j.value("enable", enable_);
    width_ = j.value("width", width_);
    height_ = j.value("height", height_);
    fps_ = j.value("fps", fps_);
    bitrate_kbps_ = j.value("bitrate_kbps", bitrate_kbps_);
    gop_ = j.value("gop", gop_);
    rtsp_port_ = j.value("port", rtsp_port_);
    app_ = j.value("app", app_);
    stream_ = j.value("stream", stream_);

    // 4. 编码类型与推流协议
    apply_codec(j.value("codec", std::string("h265")));
    apply_protocol(j.value("protocol", std::string("rtsp")),
                   j.value("transport", std::string("tcp")));

    // 5. 数值兜底，避免把 0 宽高 / 0 帧率传给编码器
    if (width_ <= 0 || height_ <= 0 || fps_ <= 0 || bitrate_kbps_ <= 0 ||
        rtsp_port_ <= 0) {
      std::cerr << "[PushNode] 配置存在非法数值(width/height/fps/bitrate_kbps/"
                   "port)，请检查 config/push.json"
                << std::endl;
    }

    // 6. 推流时间戳按配置帧率推进（原来是写死 30fps）
    frame_interval_us_ = (fps_ > 0) ? (1000000u / static_cast<uint64_t>(fps_))
                                    : 33333u;
    config_loaded_ = true;

    std::cout << "[PushNode] 推流配置已加载: " << width_ << "x" << height_
              << " codec=" << (codec_type_ == ZLM_FRAME_VIDEO_H264 ? "h264" : "h265")
              << " fps=" << fps_ << " bitrate=" << bitrate_kbps_ << "kbps"
              << " gop=" << gop_ << "(预留未生效)"
              << " protocol=" << protocol_ << " transport=" << transport_
              << " url=rtsp://<板端IP>:" << rtsp_port_ << "/" << app_ << "/"
              << stream_ << std::endl;
  } catch (const json::parse_error &e) {
    // 4. 捕获解析异常，防止崩溃
    std::cerr << "JSON Parse Error: " << e.what() << std::endl;
  } catch (const json::type_error &e) {
    std::cerr << "JSON Type Error: " << e.what() << std::endl;
  }
}
void PushNode::sendFrame(const uint8_t *pkt_data, size_t pkt_len,
                         CodingType type, uint64_t pts_us, void *user_arg) {
  // user_arg 传的是 PushNode 实例：句柄与时间戳步长都要按实例取，不能放全局
  PushNode *self = static_cast<PushNode *>(user_arg);
  // printf("send frame:  handle:%p  pkt_len:%d \n",handle,pkt_len);
  if (!self || !self->m_handle || pkt_len == 0)
    return;
  ZlmFrameCodec codingType = (type == CodingType::VIDEO_CodingH264)
                                 ? ZLM_FRAME_VIDEO_H264
                                 : ZLM_FRAME_VIDEO_H265;
  // 【核心修复】抛弃外部传进来的系统时间戳，强制使用平滑递增的帧号时间戳
  uint64_t smooth_pts_us = self->frame_index_ * self->frame_interval_us_;
  self->frame_index_++;
  int ret = zlm_rtsp_input_encodeData(self->m_handle, codingType, pkt_data,
                                      pkt_len, smooth_pts_us, smooth_pts_us);
  if (ret != ZLM_ERR_OK) {
    std::cerr << "send rtsp frame err:" << ret << "\n";
    return;
  }
  // printf("    successfully send frame,pts:%llu\n", pts_us);
  fflush(stdout);
}
void PushNode::init_zlm_push() {
  if (!enable_) {
    std::cout << "[PushNode] enable=false，跳过推流初始化（只跑采集与流水线）"
              << std::endl;
    return;
  }
  if (protocol_ != "rtsp") {
    std::cerr << "[PushNode] 协议 " << protocol_
              << " 未实现，跳过 RTSP 服务初始化" << std::endl;
    return;
  }

  int outCode = 0;
  m_handle = zlm_rtsp_onekey_start(static_cast<uint16_t>(rtsp_port_), codec_type_,
                                   app_.c_str(), stream_.c_str(), width_, height_,
                                   fps_, nullptr, nullptr, &outCode);
  if (!m_handle) {
    std::cerr << "rtsp server create fail, code:" << outCode << std::endl;
    return;
  }
  std::cout << "[PushNode] RTSP 服务已启动: rtsp://<板端IP>:" << rtsp_port_ << "/"
            << app_ << "/" << stream_ << "（transport=" << transport_
            << "，实际由播放端 SETUP 决定）" << std::endl;
  // 回调上下文传 this：句柄与时间戳步长都从实例读
  enc.set_encode_callback(sendFrame, this);
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
        // 输入帧尺寸必须与配置一致：ViNode+V4L2 会按配置缩放，
        // 其他上游（ffmpeg 采集 / 自定义节点）不读该配置，需自行保证一致
        if (!warned_size_mismatch_ &&
            (fb->width != width_ || fb->height != height_))
        {
          warned_size_mismatch_ = true;
          std::cerr << "[PushNode] 警告: 配置分辨率 " << width_ << "x" << height_
                    << " 与输入帧 " << fb->width << "x" << fb->height
                    << " 不一致（上游未按该分辨率缩放，如 ffmpeg 采集不走缩放）。"
                    << "编码器只能按帧尺寸编码，播放端可能花屏；"
                    << "请把 config/push.json 的 width/height 改成实际输入分辨率"
                    << std::endl;
        }
        // 编码器按帧真实尺寸打开，否则 MPP 会直接报错
        if (enc.open(fb->width, fb->height, codec_coding_, fps_, bitrate_kbps_))
        {
          encoder_ready = true;
          std::cout << "[" << get_full_name() << "] encoder opened: " << fb->width
                    << "x" << fb->height << " "
                    << (codec_coding_ == VIDEO_CodingH264 ? "h264" : "h265")
                    << " fps=" << fps_ << " bitrate=" << bitrate_kbps_ << "kbps"
                    << std::endl;
        }
        else
        {
          printf("failed open enc\n");
          encoder_ready = false;
          // 取块必有归还，失败分支同样要 release
          m_queue.release(block);
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