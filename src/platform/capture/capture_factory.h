#ifndef CAPTURE_FACTORY_H
#define CAPTURE_FACTORY_H
#include "capture_interface.h"
#include <memory>
#include <string>

// 采集类型枚举
enum class CaptureType
{
    CAPTURE_FFMPEG,
    CAPTURE_V4L2
};

// 工厂：运行时创建采集实例
std::unique_ptr<CaptureInterface> CreateCapture(CaptureType type);

#endif
