#ifndef FFMPEG_DECODER_H
#define FFMPEG_DECODER_H
#include "decoder_interface.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

class FfmpegDecoder : public DecoderInterface
{
public:
    FfmpegDecoder();
    ~FfmpegDecoder() override;

    bool open(bool is_h265) override;
    bool decode(const uint8_t* pkt_data, size_t pkt_len) override;
    void set_decode_callback(DecodeFrameCallback cb, void* user_arg) override;
    void close() override;
    bool is_opened() const override;

private:
    bool m_opened{false};
    DecodeFrameCallback m_cb{nullptr};
    void* m_user_arg{nullptr};

    AVCodecContext* m_codec_ctx{nullptr};
    AVFrame* m_frame{nullptr};
    AVPacket* m_pkt{nullptr};
};
#endif
