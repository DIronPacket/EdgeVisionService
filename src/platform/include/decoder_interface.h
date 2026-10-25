#ifndef DECODER_INTERFACE_H
#define DECODER_INTERFACE_H
#include "frame_buffer.h"
#include <cstdint>
#include <memory>
/**
 * @brief 解码器抽象接口（回调版本）
 * RK平台 MPP硬解码；x86 FFMPEG软解码
 */

// 解码帧回调：解码成功输出原始YUV帧
using DecodeFrameCallback = void(*)(const FrameBuffer& out_frame, void* user_arg);

class DecoderInterface
{
public:
    DecoderInterface() = default;
    virtual ~DecoderInterface() = default;
    DecoderInterface(const DecoderInterface&) = delete;
    DecoderInterface& operator=(const DecoderInterface&) = delete;

    /**
     * @brief 打开解码器
     * @param is_h265 true:H265; false:H264
     */
    virtual bool open(bool is_h265) = 0;

    /**
     * @brief 送入码流包进行解码
     * @param pkt_data 码流数据指针
     * @param pkt_len 码流字节长度
     * @return true送入成功
     */
    virtual bool decode(const uint8_t* pkt_data, size_t pkt_len) = 0;

    /**
     * @brief 注册解码帧回调，解码完成输出YUV帧触发
     */
    virtual void set_decode_callback(DecodeFrameCallback cb, void* user_arg) = 0;

    virtual void close() = 0;
    virtual bool is_opened() const = 0;
};

std::unique_ptr<DecoderInterface> create_decoder();
#endif
