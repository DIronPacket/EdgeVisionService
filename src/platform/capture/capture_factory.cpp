#include "capture_factory.h"
#include "ffmpeg_capture.h"

// 只有RK平台才包含V4L头文件
#ifdef TARGET_PLATFORM_RK3588
#include "v4l2_capture.h"
#endif

std::unique_ptr<CaptureInterface> CreateCapture(CaptureType type)
{
    switch(type)
    {
    case CaptureType::CAPTURE_FFMPEG:
        return std::make_unique<FfmpegCapture>();
#ifdef TARGET_PLATFORM_RK3588
    case CaptureType::CAPTURE_V4L2:
        return std::make_unique<V4l2Capture>();
#endif
    default:
        return nullptr;
    }
}
