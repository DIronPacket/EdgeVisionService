#include "v4l2_capture.h"
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <sstream>
#include <vector>

V4l2Capture::V4l2Capture() = default;
V4l2Capture::~V4l2Capture()
{
    stop();
    close();
}

// 私有：解析src字符串，格式约定：/dev/video0:1280:720:NV12:30
bool V4l2Capture::parse_src_param(const std::string& src, std::string& dev_path, int& w, int& h, PixelFormat& fmt, int& fps)
{
    std::vector<std::string> segs;
    std::stringstream ss(src);
    std::string item;
    while(std::getline(ss, item, ':'))
    {
        segs.push_back(item);
    }
    if(segs.size() !=5)
        return false;

    dev_path = segs[0];
    w = std::stoi(segs[1]);
    h = std::stoi(segs[2]);
    fps = std::stoi(segs[4]);

    if(segs[3] == "NV12")
    {
        fmt = PixelFormat::FMT_NV12;
    }
    else if(segs[3] == "YUV420P")
    {
        fmt = PixelFormat::FMT_YUV420P;
    }
    else
    {
        return false;
    }
    return true;
}

// ===== 基类虚函数，override，替换原来的open(w,h,fmt,fps) =====
bool V4l2Capture::open(const std::string& src)
{
    if(m_opened) return false;

    std::string dev_path;
    int w, h, fps;
    PixelFormat fmt;
    if(!parse_src_param(src, dev_path, w, h, fmt, fps))
    {
        return false;
    }

    // 全局系统调用open，加::
    m_fd = ::open(dev_path.c_str(), O_RDWR);
    if(m_fd < 0)
    {
        return false;
    }

    // ========= 下面保留你原来V4L2配置、MMAP buffer申请代码，使用w/h/fmt/fps变量 =========
    // 这里填充v4l2 fmt设置、申请MMAP buffer

    m_opened = true;
    return true;
}

void V4l2Capture::set_capture_callback(CaptureFrameCallback cb, void* user_arg)
{
    m_cb = cb;
    m_user_arg = user_arg;
}

// start() 是子类独有，无override
bool V4l2Capture::start()
{
    if(!m_opened || m_running) return false;
    m_running = true;
    m_capture_thread = std::thread([this](){
        // 循环dequeue buffer，拿到帧，调用回调
        while(m_running)
        {
            // V4L2 取帧逻辑
            FrameBuffer fb;
            if(m_cb)
            {
                m_cb(fb, m_user_arg);
            }
        }
    });
    return true;
}

// stop() 是子类独有，无override
void V4l2Capture::stop()
{
    m_running = false;
    if(m_capture_thread.joinable())
    {
        m_capture_thread.join();
    }
}

void V4l2Capture::close()
{
    if(m_fd > 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_opened = false;
}

bool V4l2Capture::is_opened() const
{
    return m_opened;
}
