#ifndef CAPTURE_INTERFACE_H
#define CAPTURE_INTERFACE_H
#include "frame_buffer.h"
#include <string>
#include <thread>


// 帧回调类型
using CaptureFrameCallback = void(*)(FrameBuffer& frame, void* user_arg);

/**
 * @brief 图像采集源抽象接口
 * 所有采集实现：FfmpegCapture / V4l2Source / DesktopSource 继承此类
 */
class CaptureInterface
{
public:
    CaptureInterface() = default;
    virtual ~CaptureInterface() = default;
    CaptureInterface(const CaptureInterface&) = delete;
    CaptureInterface& operator=(const CaptureInterface&) = delete;
    /**
     * @brief 打开数据源
     * @param src 资源地址：本地文件 / rtsp / udp / http / /dev/video0
     * @return true成功
     */
    virtual bool open(const std::string& src) = 0;
    /**
     * @brief 阻塞读取一帧图像，写入frame
     * @param frame 输出帧
     * @return false表示流结束/断开
     */
    virtual bool read_frame(FrameBuffer* frame) = 0;
    
    virtual void set_capture_callback(CaptureFrameCallback cb, void* user_arg) = 0;
    /**
     * @brief 设置输出（缩放后）分辨率，必须在 open() 之前调用
     * 默认空实现：不支持缩放的采集源保持自身原始尺寸，行为不变
     * @param w 目标宽
     * @param h 目标高
     */
    virtual void set_output_size(int w, int h) { (void)w; (void)h; }
    /**
     * @brief 关闭，释放所有资源
     */
    virtual void close() = 0;
    /**
     * @brief 查询是否打开
     */
    virtual bool is_opened() const = 0;
};
#endif
