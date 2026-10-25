#ifndef FFMPEG_ENCODER_H
#define FFMPEG_ENCODER_H
#include "encoder_interface.h"
extern "C"{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

class FfmpegEncoder : public EncoderInterface
{
public:
    FfmpegEncoder();
    ~FfmpegEncoder() override;

    bool open(int w, int h, PixelFormat fmt, int fps, int bitrate_kbps) override;
    bool encode(const FrameBuffer& frame) override;
    void set_encode_callback(EncodeCallback cb, void* user_arg) override;
    void close() override;
    bool is_opened() const override;

private:
    bool m_opened{false};
    EncodeCallback m_cb{nullptr};
    void* m_user_arg{nullptr};

    AVCodecContext* m_codec_ctx{nullptr};
    AVFrame* m_frame{nullptr};
    AVPacket* m_pkt{nullptr};
};
#endif
