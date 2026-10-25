#include "ffmpeg_capture.h"
#include <cstring>
#include <chrono>

FfmpegCapture::FfmpegCapture()
{
}

FfmpegCapture::~FfmpegCapture()
{
    close();
}

bool FfmpegCapture::open(const std::string& src)
{
    release();
    m_fmt_ctx = avformat_alloc_context();

    // 网络流优化参数，适配UDP/HTTP/RTSP
    av_opt_set(m_fmt_ctx, "buffer_size", "1316000", 0);
    av_opt_set(m_fmt_ctx, "timeout", "5000000", 0);

    int ret = avformat_open_input(&m_fmt_ctx, src.c_str(), nullptr, nullptr);
    if (ret < 0)
    {
        return false;
    }
    ret = avformat_find_stream_info(m_fmt_ctx, nullptr);
    if (ret < 0)
    {
        avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
        return false;
    }

    // 查找视频流
    m_video_idx = av_find_best_stream(m_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_video_idx < 0)
    {
        avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
        return false;
    }
    m_video_stream = m_fmt_ctx->streams[m_video_idx];
    m_codec = avcodec_find_decoder(m_video_stream->codecpar->codec_id);
    if (!m_codec)
    {
        avformat_close_input(&m_fmt_ctx);
        m_fmt_ctx = nullptr;
        return false;
    }
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

bool FfmpegCapture::read_frame(FrameBuffer& frame)
{
    if (!m_opened)
        return false;

    while (av_read_frame(m_fmt_ctx, m_pkt) >= 0)
    {
        if (m_pkt->stream_index == m_video_idx)
        {
            avcodec_send_packet(m_codec_ctx, m_pkt);
            int ret = avcodec_receive_frame(m_codec_ctx, m_frame_raw);
            if (ret == 0)
            {
                bool ok = transfer_frame(m_frame_raw, frame);
                // printf("error:%s\n",strerror(errno));
                // printf("transfer_frame-ok:%d m_frame_raw:%d\n",ok?1:0,m_frame_raw->format);
                av_packet_unref(m_pkt);
                return ok;
            }
        }
        av_packet_unref(m_pkt);
    }
    return false;
}

bool FfmpegCapture::transfer_frame(AVFrame* av_frame, FrameBuffer& out_frame)
{
    int w = av_frame->width;
    int h = av_frame->height;
    enum AVPixelFormat dst_fmt = AV_PIX_FMT_NV12;

    int hw_stride = ((w + 127)/128)*128; // 1080 →1152

    m_sws_ctx = sws_getCachedContext(m_sws_ctx,
        w, h, (enum AVPixelFormat)av_frame->format,
        w, h, dst_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!m_sws_ctx)
    {
        return false;
    }

    // 分配硬件对齐总大小 hw_stride * h * 3/2
    size_t buf_size = (size_t)hw_stride * h * 3 / 2;
    if (!out_frame.allocate(buf_size))
    {
        printf("failed allocate outframe\n");
        return false;
    }
        

    uint8_t* y_ptr = out_frame.data;
    uint8_t* uv_ptr = out_frame.data + (size_t)hw_stride * h;

    uint8_t* dst_data[4] = { y_ptr, uv_ptr, nullptr, nullptr };
    int dst_stride[4] = { hw_stride, hw_stride, 0, 0 }; // ✅ swscale按1152行跨度输出

    int ret_h = sws_scale(m_sws_ctx,
        av_frame->data, av_frame->linesize,
        0, h,
        dst_data, dst_stride);

    out_frame.width = w;
    out_frame.height = h;
    out_frame.stride = hw_stride; // 【需要给FrameBuffer新增int stride成员】
    out_frame.fmt = PixelFormat::FMT_NV12;
    auto ts = std::chrono::system_clock::now().time_since_epoch();
    out_frame.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ts).count();

    return ret_h == h;
}

void FfmpegCapture::close()
{
    release();
    m_opened = false;
}

bool FfmpegCapture::is_opened() const
{
    return m_opened;
}

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
