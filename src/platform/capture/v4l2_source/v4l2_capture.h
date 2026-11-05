#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H

#include "capture_interface.h"
#include <vector>
#include <string>
#include <linux/videodev2.h>

// V4L2 内存映射结构体
struct V4L2Buffer
{
    void *start;
    size_t length;
};

class V4l2Capture : public CaptureInterface
{
public:
    V4l2Capture();
    ~V4l2Capture() override;

    bool open(const std::string &src) override;
    bool read_frame(FrameBuffer *frame) override; // 【核心】同步读帧
    void set_capture_callback(CaptureFrameCallback cb, void *user_arg) override { /* 拉取模式不需要回调 */ }
    // 设置缩放输出分辨率（NV12 要求宽高为偶数），必须在 open() 之前调用
    void set_output_size(int w, int h) override;
    void close() override;
    bool is_opened() const override;

private:
    bool parse_src_param(const std::string &src, std::string &dev_path, int &w, int &h, PixelFormat &fmt, int &fps);

    bool m_opened{false};
    int m_fd{-1};

    int m_src_width{0};
    int m_src_height{0};
    int m_dst_width{1920};
    int m_dst_height{1080};
    int m_fps{0};
    PixelFormat m_fmt{PixelFormat::FMT_NV12};
    std::vector<V4L2Buffer> m_buffers;
    struct SwsContext *m_sws_ctx{nullptr};
};

#endif