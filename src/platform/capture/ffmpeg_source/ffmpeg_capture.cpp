#include "ffmpeg_capture.h"
#include <chrono>
#include <cstdio>
#include <cstring>

FfmpegCapture::FfmpegCapture() {}

FfmpegCapture::~FfmpegCapture() { close(); }

bool FfmpegCapture::open(const std::string &src)
{
  release();
  m_fmt_ctx = avformat_alloc_context();

  AVDictionary *options = nullptr;
  const AVInputFormat *ifmt = nullptr;

  // ==========================================
  // 1. 判断输入源类型
  // ==========================================
  if (src.find("/dev/video") == 0)
  {
    // 【场景 A】本地 USB 摄像头 (V4L2)
    ifmt = av_find_input_format("v4l2");
    if (!ifmt)
    {
      printf("Error: v4l2 input format not found!\n");
      return false;
    }

    // 强制指定参数（根据你 v4l2-ctl 看到的情况）
    av_dict_set(&options, "input_format", "yuyv422", 0); // 或 "mjpeg"
    av_dict_set(&options, "framerate", "30", 0);
    av_dict_set(&options, "video_size", "1920x1080", 0);

    printf("[FfmpegCapture] Opening Camera: %s\n", src.c_str());
  }
  else if (src.find("rtsp://") == 0 || src.find("rtmp://") == 0 || src.find("http://") == 0)
  {
    // 【场景 B】网络流 (RTSP/RTMP/HTTP)
    // 设置超时，防止断流时无限卡死
    av_dict_set(&options, "stimeout", "5000000", 0);    // 5秒超时 (微秒)
    av_dict_set(&options, "buffer_size", "1316000", 0); // 加大缓冲区
    // 如果是 UDP 容易丢包，可以加上强制 TCP 传输（如果服务器支持）
    // av_dict_set(&options, "rtsp_transport", "tcp", 0);

    printf("[FfmpegCapture] Opening Network Stream: %s\n", src.c_str());
  }
  else
  {
    // 【场景 C】本地文件
    printf("[FfmpegCapture] Opening Local File: %s\n", src.c_str());
  }

  int ret = avformat_open_input(&m_fmt_ctx, src.c_str(), nullptr, nullptr);
  if (ret < 0)
    return false;

  ret = avformat_find_stream_info(m_fmt_ctx, nullptr);
  if (ret < 0)
  {
    avformat_close_input(&m_fmt_ctx);
    m_fmt_ctx = nullptr;
    return false;
  }

  m_video_idx =
      av_find_best_stream(m_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (m_video_idx < 0)
  {
    avformat_close_input(&m_fmt_ctx);
    m_fmt_ctx = nullptr;
    return false;
  }
  m_video_stream = m_fmt_ctx->streams[m_video_idx];
  m_codec = avcodec_find_decoder(m_video_stream->codecpar->codec_id);
  if (!m_codec)
    return false;

  m_codec_ctx = avcodec_alloc_context3(m_codec);
  avcodec_parameters_to_context(m_codec_ctx, m_video_stream->codecpar);
  ret = avcodec_open2(m_codec_ctx, m_codec, nullptr);
  if (ret < 0)
  {
    avcodec_free_context(&m_codec_ctx);
    avformat_close_input(&m_fmt_ctx);
    m_fmt_ctx = nullptr;
    return false;
  }

  m_pkt = av_packet_alloc();
  m_frame_raw = av_frame_alloc();
  m_frame_dst = av_frame_alloc();

  m_opened = true;
  return true;
}

// ==========================================
// 【核心修改】传入 FrameBuffer*
// ==========================================
bool FfmpegCapture::read_frame(FrameBuffer *out_frame)
{
  if (!m_opened || !out_frame)
    return false;

  while (av_read_frame(m_fmt_ctx, m_pkt) >= 0)
  {
    if (m_pkt->stream_index == m_video_idx)
    {
      avcodec_send_packet(m_codec_ctx, m_pkt);
      int ret = avcodec_receive_frame(m_codec_ctx, m_frame_raw);
      if (ret == 0)
      {
        // 传入指针给 transfer_frame 进行数据写入
        // printf("successfully receive frame , input frame:%p\n",
        // out_frame);
        bool ok = transfer_frame(m_frame_raw, out_frame);
        av_packet_unref(m_pkt);
        return ok;
      }
    }
    av_packet_unref(m_pkt);
  }
  return false;
}

// ==========================================
// 【核心修改】数据直接写入传入的 FrameBuffer*
// ==========================================
bool FfmpegCapture::transfer_frame(AVFrame *av_frame, FrameBuffer *out_frame)
{
  if (!av_frame || !out_frame)
    return false;
  // printf("pre trans frame\n");
  int w = av_frame->width;
  int h = av_frame->height;
  enum AVPixelFormat dst_fmt = AV_PIX_FMT_NV12;

  // FFmpeg 硬解常见对齐（1080 -> 1152 或 1088，取决于具体实现，这里按你的写法）
  int hw_stride = ((w + 127) / 128) * 128;
  size_t buf_size = (size_t)hw_stride * h * 3 / 2;

  // 【极度重要】校验外部传进来的缓冲区是否足够大
  if (out_frame->capacity < buf_size)
  {
    printf("FfmpegCapture Error: FrameBuffer capacity (%zu) is smaller than "
           "required (%zu). Check MediaPool config!\n",
           out_frame->capacity, buf_size);
    return false;
  }

  // 外部必须已经把 out_frame->data 指向了 MediaBlock 里的物理内存
  if (!out_frame->data)
  {
    printf("FfmpegCapture Error: out_frame->data is nullptr!\n");
    return false;
  }

  // printf("pre get scale,out-frame:%p\n",out_frame);
  m_sws_ctx = sws_getCachedContext(
      m_sws_ctx, w, h, (enum AVPixelFormat)av_frame->format, w, h, dst_fmt,
      SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!m_sws_ctx)
  {
    printf("failed get cached sws_ctx context\n");
    return false;
  }

  // 计算 Y 和 UV 的物理指针偏移（直接指向池子里的物理内存）
  uint8_t *y_ptr = out_frame->data[0];
  uint8_t *uv_ptr = out_frame->data[1];

  uint8_t *dst_data[4] = {y_ptr, uv_ptr, nullptr, nullptr};
  int dst_stride[4] = {hw_stride, hw_stride, 0, 0}; // ✅ swscale按1152行跨度输出
  // printf("pre scale frame\n");
  int ret_h = sws_scale(m_sws_ctx, av_frame->data, av_frame->linesize, 0, h,
                        dst_data, dst_stride);

  if (ret_h != h)
  {
    printf("ret_h != h,ret_h:%d  h:%d y_ptr:%p uv_ptr:%p \n",
           ret_h, h, y_ptr, uv_ptr);
    return false;
  }

  // ==========================================
  // 更新 FrameBuffer 的元数据
  // ==========================================
  out_frame->width = w;
  out_frame->height = h;
  out_frame->stride[0] = out_frame->stride[1] = hw_stride;
  out_frame->fmt = PixelFormat::FMT_NV12;
  out_frame->data_size = buf_size; // 实际写入的大小

  auto ts = std::chrono::system_clock::now().time_since_epoch();
  out_frame->timestamp_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(ts).count();
  // printf("successfully  trans frame\n");
  return true;
}

void FfmpegCapture::close()
{
  release();
  m_opened = false;
}

bool FfmpegCapture::is_opened() const { return m_opened; }

void FfmpegCapture::release()
{
  if (m_sws_ctx)
  {
    sws_freeContext(m_sws_ctx);
    m_sws_ctx = nullptr;
  }
  if (m_frame_dst)
    av_frame_free(&m_frame_dst);
  if (m_frame_raw)
    av_frame_free(&m_frame_raw);
  if (m_pkt)
    av_packet_free(&m_pkt);
  if (m_codec_ctx)
    avcodec_free_context(&m_codec_ctx);
  if (m_fmt_ctx)
    avformat_close_input(&m_fmt_ctx);

  m_video_idx = -1;
  m_video_stream = nullptr;
  m_codec = nullptr;
  m_fmt_ctx = nullptr;
  m_codec_ctx = nullptr;
}