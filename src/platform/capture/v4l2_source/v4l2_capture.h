#ifndef V4L2_CAPTURE_H
#define V4L2_CAPTURE_H
#include "capture_interface.h"
#include <thread>

class V4l2Capture : public CaptureInterface
{
public:
    V4l2Capture();
    ~V4l2Capture() override;

    // ====== 和基类CaptureInterface完全对齐，必须override ======
    bool open(const std::string& src) override;
    bool read_frame(FrameBuffer& frame) override{};
    void set_capture_callback(CaptureFrameCallback cb, void* user_arg) override;
    void close() override;
    bool is_opened() const override;

    // ====== V4L2子类独有的接口，删掉override！基类没有 ======
    bool start();
    void stop();

private:
    // 内部解析src字符串，例如 "/dev/video0:1280:720:Nv12:30"
    bool parse_src_param(const std::string& src, std::string& dev_path, int& w, int& h, PixelFormat& fmt, int& fps);

    bool m_opened{false};
    int m_fd{-1};
    CaptureFrameCallback m_cb{nullptr};
    void* m_user_arg{nullptr};
    std::thread m_capture_thread;
    bool m_running{false};
    // buffer、v4l2相关变量
};
#endif
