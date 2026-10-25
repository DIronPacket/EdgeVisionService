#ifndef RK_MPP_ENCODER_H
#define RK_MPP_ENCODER_H
#include "encoder_interface.h"
#include "rockchip/rk_mpi.h"
#include "rockchip/mpp_frame.h"
#include "rockchip/mpp_packet.h"
#include <vector>
#include <mutex>
typedef enum {
Mpp_FRAME_VIDEO_H264 = 0,
Mpp_FRAME_VIDEO_H265,
Mpp_FRAME_AUDIO_AAC
} MppFrameCodec;
class MppEncoder : public EncoderInterface
{
public:
    MppEncoder();
    ~MppEncoder() override;

    bool open(int w, int h, CodingType type, int fps, int bitrate_kbps) override;
    bool encode(const FrameBuffer& frame) override;
    void set_encode_callback(EncodeCallback cb, void* user_arg) override;
    void close() override;
    bool is_opened() const override;

    static CodingType switchCodingType(MppCodingType type);
    static MppCodingType switchCodingType(CodingType type);

private:
    MppCtx          m_mpp_ctx = nullptr;
    MppApi*         m_mpp_api = nullptr;
    MppEncCfg       m_enc_cfg = nullptr;
    bool            m_opened = false;
    MppCodingType   m_encoding_type = MPP_VIDEO_CodingAVC;

    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
    uint64_t m_bitrate = 0;

    EncodeCallback m_cb;
    void*                m_user_arg = nullptr;

};
#endif
