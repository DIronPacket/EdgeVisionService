#include "ffmpeg_decoder.h"
#include <cstring>

FfmpegDecoder::FfmpegDecoder()
{
    avcodec_register_all();
}

FfmpegDecoder::~FfmpegDecoder()
{
    close();
}

bool FfmpegDecoder::open(bool is_h265)
{
    if(m_opened)
        return false;

    const AVCodec* codec = nullptr;
    if(is_h265)
    {
        codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    }
    else
    {
        codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    }
    if(!codec)
        return false;

    m_codec_ctx = avcodec_alloc_context3(codec);
    if(avcodec_open2(m_codec_ctx, codec, nullptr) < 0)
    {
        avcodec_free_context(&m_codec_ctx);
        return false;
    }

    m_frame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    m_opened = true;
    return true;
}

bool FfmpegDecoder::decode(const uint8_t* pkt_data, size_t pkt_len)
{
    if(!m_opened || !pkt_data || pkt_len == 0)
        return false;

    av_packet_unref(m_pkt);
    m_pkt->data = const_cast<uint8_t*>(pkt_data);
    m_pkt->size = static_cast<int>(pkt_len);

    int ret = avcodec_send_packet(m_codec_ctx, m_pkt);
    if(ret < 0)
        return false;

    // 循环读取解码后的帧
    while(avcodec_receive_frame(m_codec_ctx, m_frame) == 0)
    {
        if(m_cb)
        {
            FrameBuffer fb;
            fb.width = m_frame->width;
            fb.height = m_frame->height;
            fb.data[0] = m_frame->data[0];
            fb.data[1] = m_frame->data[1];
            fb.linesize[0] = m_frame->linesize[0];
            fb.linesize[1] = m_frame->linesize[1];
            fb.pixel_fmt = PixelFormat::NV12;
            m_cb(fb, m_user_arg);
        }
    }
    return true;
}

void FfmpegDecoder::set_decode_callback(DecodeFrameCallback cb, void* user_arg)
{
    m_cb = cb;
    m_user_arg = user_arg;
}

void FfmpegDecoder::close()
{
    if(m_codec_ctx)
    {
        avcodec_free_context(&m_codec_ctx);
    }
    if(m_frame)
        av_frame_free(&m_frame);
    if(m_pkt)
        av_packet_free(&m_pkt);
    m_cb = nullptr;
    m_user_arg = nullptr;
    m_opened = false;
}

bool FfmpegDecoder::is_opened() const
{
    return m_opened;
}
