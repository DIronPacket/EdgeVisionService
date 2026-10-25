#ifndef FFMPEG_SOURCE_H
#define FFMPEG_SOURCE_H

#include "capture_interface.h"
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
}

class FfmpegCapture : public CaptureInterface
{
public:
    FfmpegCapture();
    ~FfmpegCapture() override;

    bool open(const std::string& src) override;
    bool read_frame(FrameBuffer& frame) override;
    void set_capture_callback(CaptureFrameCallback cb, void* user_arg) override{};
    void close() override;
    bool is_opened() const override;

private:
    void release();
    bool transfer_frame(AVFrame* av_frame, FrameBuffer& out_frame);

    AVFormatContext* m_fmt_ctx = nullptr;
    AVCodecContext* m_codec_ctx = nullptr;
    const AVCodec* m_codec = nullptr;
    AVStream* m_video_stream = nullptr;

    AVPacket* m_pkt = nullptr;
    AVFrame* m_frame_raw = nullptr;
    AVFrame* m_frame_dst = nullptr;
    SwsContext* m_sws_ctx = nullptr;

    int m_video_idx = -1;
    bool m_opened = false;
};

#endif
