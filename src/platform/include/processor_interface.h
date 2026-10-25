#ifndef PROCESSOR_INTERFACE_H
#define PROCESSOR_INTERFACE_H

#include "frame_buffer.h"

/**
 * @brief 图像预处理抽象接口
 * RK平台用RGA实现，x86平台用OpenCV实现
 */
class ProcessorInterface
{
public:
    ProcessorInterface() = default;
    virtual ~ProcessorInterface() = default;

    ProcessorInterface(const ProcessorInterface&) = delete;
    ProcessorInterface& operator=(const ProcessorInterface&) = delete;

    /**
     * @brief 图像预处理：缩放、裁剪、格式转换
     * @param src 输入帧
     * @param dst 输出帧
     * @return true成功
     */
    virtual bool process(const FrameBuffer& src, FrameBuffer& dst) = 0;
};

#endif
