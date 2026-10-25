#include "ffmpeg_encoder.h"
#include <cstring>

FfmpegEncoder::FfmpegEncoder()
{
    avcodec_register_all();
}

FfmpegEncoder::~FfmpegEncoder()
{
    close();
}

bool FfmpegEncoder::open(int w, int h, PixelFormat fmt, int fps, int bitrate_kbps)
{
    if(m_opened) return false;
    // 默认H265，后续可扩展参数切换H264
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H265);
    if (!codec) return false;

    m_codec_ctx = avcodec_alloc_context3(codec);
    m_codec_ctx->width = w;
    m_codec_ctx->height = h;
    m_codec_ctx->time_base = {1, fps};
    m_codec_ctx->framerate = {fps,1};
    m_codec_ctx->bit_rate = bitrate_kbps * 1000;
    m_codec_ctx->gop_size = fps;
    m_codec_ctx->max_b_frames = 0;
    m_codec_ctx->pix_fmt = AV_PIX_FMT_NV12;

    if(avcodec_open2(m_codec_ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_codec_ctx);
        return false;
    }

    m_frame = av_frame_alloc();
    m_frame->width = w;
    m_frame->height = h;
    m_frame->format = AV_PIX_FMT_NV12;
    av_frame_get_buffer(m_frame, 0);

    m_pkt = av_packet_alloc();
    m_opened = true;
    return true;
}

bool FfmpegEncoder::encode(const FrameBuffer& frame)
{
    if(!m_opened) return false;

    // 拷贝NV12原始帧数据
    memcpy(m_frame->data[0], frame.data[0], frame.linesize[0] * frame.height);
    memcpy(m_frame->data[1], frame.data[1], frame.linesize[1] * frame.height / 2);

    int ret = avcodec_send_frame(m_codec_ctx, m_frame);
    if(ret <0) return false;

    // 循环取出编码后的码流，触发回调
    while(avcodec_receive_packet(m_codec_ctx, m_pkt) == 0)
    {
        if(m_cb)
        {
            m_cb(m_pkt->data, m_pkt->size, m_user_arg);
        }
        av_packet_unref(m_pkt);
    }
    return true;
}

void FfmpegEncoder::set_encode_callback(EncodeCallback cb, void* user_arg)
{
    m_cb = cb;
    m_user_arg = user_arg;
}

void FfmpegEncoder::close()
{
    if(m_codec_ctx)
    {
        avcodec_send_frame(m_codec_ctx, nullptr);
        avcodec_free_context(&m_codec_ctx);
    }
    if(m_frame) av_frame_free(&m_frame);
    if(m_pkt) av_packet_free(&m_pkt);
    m_cb = nullptr;
    m_user_arg = nullptr;
    m_opened = false;
}

bool FfmpegEncoder::is_opened() const
{
    return m_opened;
}
