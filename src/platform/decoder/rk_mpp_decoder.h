#ifndef RK_MPP_DECODER_H
#define RK_MPP_DECODER_H
#include "decoder_interface.h"
#include "rockchip/rk_mpi.h"
#include "rockchip/mpp_log.h"
#include "rockchip/mpp_frame.h"
#include "rockchip/mpp_packet.h"
#include <unistd.h>


class MppDecoder : public DecoderInterface
{
public:
    MppDecoder();
    ~MppDecoder() override;

    bool open(bool is_h265) override;
    bool decode(const uint8_t* pkt_data, size_t pkt_len) override;
    void set_decode_callback(DecodeFrameCallback cb, void* user_arg) override;
    void close() override;
    bool is_opened() const override;

private:
    bool m_opened{false};
    DecodeFrameCallback m_cb{nullptr};
    void* m_user_arg{nullptr};

    MppApi* m_mpp_api{nullptr};
    MppCtx* m_mpp_ctx{nullptr};
};
#endif
