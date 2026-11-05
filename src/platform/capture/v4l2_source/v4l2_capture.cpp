#include "v4l2_capture.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <vector>
#include <iostream>
#include <chrono>
#include <fstream>

extern "C"
{
#include <libswscale/swscale.h>
}

V4l2Capture::V4l2Capture() = default;
V4l2Capture::~V4l2Capture() { close(); }

bool V4l2Capture::parse_src_param(const std::string &src, std::string &dev_path, int &w, int &h, PixelFormat &fmt, int &fps)
{
    std::vector<std::string> segs;
    std::stringstream ss(src);
    std::string item;
    while (std::getline(ss, item, ':'))
    {
        segs.push_back(item);
    }
    if (segs.size() != 5)
        return false;

    dev_path = segs[0];
    w = std::stoi(segs[1]);
    h = std::stoi(segs[2]);
    fps = std::stoi(segs[4]);

    if (segs[3] == "NV12")
        fmt = PixelFormat::FMT_NV12;
    else if (segs[3] == "YUV420P")
        fmt = PixelFormat::FMT_YUV420P;
    else
        return false;

    return true;
}

void V4l2Capture::set_output_size(int w, int h)
{
    // NV12 要求宽高为偶数；sws 上下文在 open() 里按该尺寸创建，所以必须先调用本函数
    if (w <= 0 || h <= 0 || (w % 2) != 0 || (h % 2) != 0)
    {
        std::cerr << "V4l2Capture: 非法输出分辨率 " << w << "x" << h
                  << "（需为正偶数），保持 " << m_dst_width << "x" << m_dst_height << std::endl;
        return;
    }
    m_dst_width = w;
    m_dst_height = h;
}

bool V4l2Capture::open(const std::string &src)
{
    if (m_opened)
        return false;

    std::string dev_path;
    if (!parse_src_param(src, dev_path, m_src_width, m_src_height, m_fmt, m_fps))
    {
        std::cerr << "parse src failed: " << src << std::endl;
        return false;
    }

    m_fd = ::open(dev_path.c_str(), O_RDWR | O_NONBLOCK, 0);
    if (m_fd < 0)
    {
        perror("open v4l2 failed");
        return false;
    }

    // 1. 设置格式 (强制 YUYV)
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = m_src_width;
    fmt.fmt.pix.height = m_src_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0)
    {
        perror("VIDIOC_S_FMT YUYV failed");
        return false;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = m_fps;
    if (ioctl(m_fd, VIDIOC_S_PARM, &parm) < 0)
    {
        perror("VIDIOC_S_PARM failed");
    }

    // 2. 申请缓冲区 & MMAP & 入队
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0)
    {
        perror("VIDIOC_REQBUFS failed");
        return false;
    }

    m_buffers.resize(req.count);
    for (int i = 0; i < req.count; ++i)
    {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0)
            return false;

        m_buffers[i].length = buf.length;
        m_buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, buf.m.offset);

        if (m_buffers[i].start == MAP_FAILED)
            return false;

        if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
            return false;
    }

    // 3. 开启视频流
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("VIDIOC_STREAMON failed");
        return false;
    }

    // 4. 初始化 sws_ctx (YUV 转换用)
    m_sws_ctx = sws_getContext(
        m_src_width, m_src_height, AV_PIX_FMT_YUYV422,
        m_dst_width, m_dst_height, AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    printf("open init m_sws_cts  src-h:%d , src-w:%d ,"
           "dst-h::%d , dst-w:%d\n",
           m_src_height, m_src_width,
           m_dst_height, m_dst_width);
    m_opened = true;
    return true;
}

// ==========================================
// 【核心】同步读取一帧
// ==========================================
bool V4l2Capture::read_frame(FrameBuffer *frame)
{
    if (!m_opened || !frame)
        return false;

    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    auto frame_start_time = std::chrono::steady_clock::now();
    // 1. 尝试出队
    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        // 如果没数据，为了不卡死外层循环，稍微睡一下
        usleep(2000);
        // printf("not ioctl frame\n");
        return false;
    }
    auto frame_end_time = std::chrono::steady_clock::now();
    // 【修复】直接相减，不进行 duration_cast，让 auto 自动推导类型
    auto process_cost = frame_end_time - frame_start_time;
    // 1. 转换成毫秒（浮点数，可以保留小数，比如 1.23 ms）
    auto cost_ms = std::chrono::duration_cast<std::chrono::microseconds>(process_cost).count() / 1000.0;
    // printf(" -- 1 [ioctl Cost] %.2f ms\n", cost_ms);
    // 2. 此时拿到了图像数据，在 m_buffers[buf.index].start 里
    uint8_t *yuyv_data = (uint8_t *)m_buffers[buf.index].start;

    // 3. 转码写入 FrameBuffer
    const uint8_t *src_data[4] = {yuyv_data, nullptr, nullptr, nullptr};
    int src_linesize[4] = {m_src_width * 2, 0, 0, 0}; // YUYV 一像素2字节

    uint8_t *dst_data[4] = {frame->data[0], frame->data[1], nullptr, nullptr};
    int dst_linesize[4] = {frame->stride[0], frame->stride[1], 0, 0};

    int ret_h = sws_scale(m_sws_ctx, src_data, src_linesize, 0, m_src_height, dst_data, dst_linesize);
    // printf("swscale ret_h:%d  h:%d\n", ret_h, m_dst_height);
    // 4. 更新元数据
    frame->width = m_dst_width;
    frame->height = m_dst_height;
    frame->fmt = m_fmt;
    frame->data_size = m_dst_width * m_dst_height * 3 / 2;
    frame->timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    frame_end_time = std::chrono::steady_clock::now();
    // 【修复】直接相减，不进行 duration_cast，让 auto 自动推导类型
    process_cost = frame_end_time - frame_start_time;
    // 1. 转换成毫秒（浮点数，可以保留小数，比如 1.23 ms）
    cost_ms = std::chrono::duration_cast<std::chrono::microseconds>(process_cost).count() / 1000.0;
    // printf(" -- 2 [ioctl Cost] %.2f ms\n", cost_ms);
    // 5. 重新入队，把 buffer 还给内核
    if (ioctl(m_fd, VIDIOC_QBUF, &buf) < 0)
    {
        perror("VIDIOC_QBUF failed");
        return false;
    }
    frame_end_time = std::chrono::steady_clock::now();
    // 【修复】直接相减，不进行 duration_cast，让 auto 自动推导类型
    process_cost = frame_end_time - frame_start_time;
    // 1. 转换成毫秒（浮点数，可以保留小数，比如 1.23 ms）
    cost_ms = std::chrono::duration_cast<std::chrono::microseconds>(process_cost).count() / 1000.0;
    // printf(" -- 3 [ioctl Cost] %.2f ms\n", cost_ms);
    return true;
}

void V4l2Capture::close()
{
    if (m_sws_ctx)
    {
        sws_freeContext(m_sws_ctx);
        m_sws_ctx = nullptr;
    }

    for (auto &b : m_buffers)
    {
        munmap(b.start, b.length);
    }
    m_buffers.clear();

    if (m_fd > 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_opened = false;
}

bool V4l2Capture::is_opened() const { return m_opened; }