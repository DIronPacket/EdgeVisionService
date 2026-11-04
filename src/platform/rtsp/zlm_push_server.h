#ifndef ZLM_PUSH_SERVER_H
#define ZLM_PUSH_SERVER_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

// ===================== 全局错误码 =====================
typedef enum {
ZLM_ERR_OK               = 0,
ZLM_ERR_INVALID_HANDLE   = -1,
ZLM_ERR_INVALID_PARAM    = -2,
ZLM_ERR_ENCODER_INIT     = -3,
ZLM_ERR_NETWORK          = -4,
ZLM_ERR_TIMEOUT          = -5,
ZLM_ERR_REACH_MAX_INST   = -6,
ZLM_ERR_STREAM_EXIST     = -7,
ZLM_ERR_NOT_FOUND        = -8,
ZLM_ERR_INTERNAL         = -99
} ZlmErrorCode;
// ===================== 帧编码类型 =====================
typedef enum {
ZLM_FRAME_VIDEO_H264 = 0,
ZLM_FRAME_VIDEO_H265,
ZLM_FRAME_AUDIO_AAC
} ZlmFrameCodec;



typedef void (*ZlmOutputCb)(const uint8_t* pkt_data, int pkt_len, uint64_t pts_us ,void* user_arg);

// ===================== RTSP协议配置 =====================
typedef enum {
ZLM_PROTO_RTSP_TCP = 0,
ZLM_PROTO_RTSP_UDP
} ZlmPushProtoType;

typedef void* ZlmServerStreamHandle;

// 一键启动RTSP服务+一路流
ZlmServerStreamHandle zlm_rtsp_onekey_start(uint16_t rtsp_port,ZlmFrameCodec type,
                                            const char* app, const char* stream,
                                            int w, int h, int fps,
                                            ZlmOutputCb event_cb, void* user_data,
                                            int* out_code);
// 送H264帧
int zlm_rtsp_input_encodeData(ZlmServerStreamHandle h,ZlmFrameCodec type, const uint8_t* data, int len, uint64_t dts, uint64_t pts);
// 销毁，流全部关完自动停server
void zlm_rtsp_stop(ZlmServerStreamHandle h);
}
#endif
