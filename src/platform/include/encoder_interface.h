#ifndef ENCODER_INTERFACE_H
#define ENCODER_INTERFACE_H
#include "frame_buffer.h"
#include <cstdint>

/**
 * @brief 编码器抽象接口（回调版本）
 * RK平台 MPP硬编码；x86 FFMPEG软编码
 */
typedef enum {
    VIDEO_CodingH264=0,            
    VIDEO_CodingH265=1
} CodingType;
// 编码码流回调：编码输出H264/H265码流
// pkt_data:码流起始地址，pkt_len:码流字节长度，user_arg:用户自定义上下文
using EncodeCallback = void(*)(const uint8_t* pkt_data, size_t pkt_len,CodingType type,uint64_t pts_us, void* user_arg);

class EncoderInterface
{
public:
    EncoderInterface() = default;
    virtual ~EncoderInterface() = default;
    EncoderInterface(const EncoderInterface&) = delete;
    EncoderInterface& operator=(const EncoderInterface&) = delete;

    /**
     * @brief 打开编码器
     * @param w 宽
     * @param h 高
     * @param fmt 帧像素格式（NV12等，PixelFormat在frame_buffer.h定义）
     * @param fps 帧率
     * @param bitrate_kbps 码率，单位 kbps
     */
    virtual bool open(int w, int h, CodingType fmt, int fps, int bitrate_kbps) = 0;

    /**
     * @brief 送入一帧原始图像进行编码
     * @param frame 输入原始YUV帧
     * @return true 送入成功；false失败
     */
    virtual bool encode(const FrameBuffer& frame) = 0;

    /**
     * @brief 注册码流回调，编码完成后触发
     * @param cb 回调函数指针，nullptr取消回调
     * @param user_arg 用户自定义参数，回调时原样带回
     */
    virtual void set_encode_callback(EncodeCallback cb, void* user_arg) = 0;

    virtual void close() = 0;
    virtual bool is_opened() const = 0;
};
#endif
