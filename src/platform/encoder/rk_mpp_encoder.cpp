#include "rk_mpp_encoder.h"
#include <cstring>
#include <unistd.h>
#include <cstring>
#include <cstdio>

MppEncoder::MppEncoder() = default;
MppEncoder::~MppEncoder()
{
    close();
}
CodingType MppEncoder::switchCodingType(MppCodingType type)
{
    switch (type)
    {
    case MPP_VIDEO_CodingAVC:
        return VIDEO_CodingH264;
    case MPP_VIDEO_CodingHEVC:
        return VIDEO_CodingH265;
    default:
        break;
    }
    return VIDEO_CodingH264;
}
MppCodingType MppEncoder::switchCodingType(CodingType type)
{
    switch (type)
    {
    case VIDEO_CodingH264:
        return MPP_VIDEO_CodingAVC;
    case VIDEO_CodingH265:
        return MPP_VIDEO_CodingHEVC;
    default:
        break;
    }
    return MPP_VIDEO_CodingAVC;
}
bool MppEncoder::open(int w, int h, CodingType type, int fps, int bitrate_kbps)
{
    if (m_opened)
        return false;
    MPP_RET ret = MPP_OK;
    m_width = w;
    m_height = h;
    m_fps = fps;
    m_bitrate = (uint64_t)bitrate_kbps*1000;

    ret = mpp_create(&m_mpp_ctx, &m_mpp_api);
    if (ret != MPP_OK)
    {
        fprintf(stderr, "MPP Encoder Error: mpp_create fail ret:%d\n", ret);
        return false;
    }

    m_encoding_type=switchCodingType(type);
    ret = mpp_init(m_mpp_ctx, MPP_CTX_ENC, m_encoding_type);
    if (ret != MPP_OK)
    {
        fprintf(stderr, "MPP Encoder Error: mpp_init fail ret:%d\n", ret);
        mpp_destroy(m_mpp_ctx);
        m_mpp_ctx = nullptr;
        m_mpp_api = nullptr;
        return false;
    }

    MppEncCfg cfg = nullptr;
    mpp_enc_cfg_init(&cfg);
    ret = m_mpp_api->control(m_mpp_ctx, MPP_ENC_GET_CFG, cfg);
    if (ret != MPP_OK)
    {
        fprintf(stderr, "MPP_ENC_GET_CFG fail ret:%d\n", ret);
        mpp_destroy(m_mpp_ctx);
        m_mpp_ctx = nullptr;
        m_mpp_api = nullptr;
        mpp_enc_cfg_deinit(cfg);
        return false;
    }

    // prep配置
    int stride = ((w + 127)/128)*128;
    mpp_enc_cfg_set_s32(cfg, "prep:width", w);
    mpp_enc_cfg_set_s32(cfg, "prep:height", h);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", h);
    mpp_enc_cfg_set_s32(cfg, "prep:format", MPP_FMT_YUV420SP);

    // // rc CBR配置
    // uint64_t target_bps = (uint64_t)bitrate_kbps * 1000;
    // mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR); //恒定码率
    // mpp_enc_cfg_set_u64(cfg, "rc:bps_target", target_bps);
    // mpp_enc_cfg_set_u64(cfg, "rc:bps_min", target_bps * 8 / 10);
    // mpp_enc_cfg_set_u64(cfg, "rc:bps_max", target_bps * 12 / 10);


    // // // rc VBR配置
    // uint64_t target_bps = (uint64_t)bitrate_kbps * 1000;
    // mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_VBR); // VBR动态码率
    // mpp_enc_cfg_set_u64(cfg, "rc:bps_target", target_bps);      // 期望平均码率
    // mpp_enc_cfg_set_u64(cfg, "rc:bps_min", target_bps * 5 / 10); // 最低平均码率
    // mpp_enc_cfg_set_u64(cfg, "rc:bps_max", target_bps * 12 / 10);// 瞬时码率硬上限，防止爆码

    // // 新版帧率配置，替代旧的 rc:fps_num / rc:fps_den
    // mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    // mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    // mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    // mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    // mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    // mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    // mpp_enc_cfg_set_s32(cfg, "rc:gop", fps * 5);
    // // ========== 新增画质QP参数 ==========
    // mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 32);    // 初始QP
    // mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 26);    // 最小QP（最好画质上限）
    // mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 40);    // 最大QP（最差画质上限）
    // mpp_enc_cfg_set_s32(cfg, "rc:qp_step", 4);    // 相邻帧QP最大变化步长，防止画面剧烈闪烁

    // - QP=30：画质更好，码率高，文件更大（你现在情况）
    // - QP=32：画质略降，码率下降
    // - QP=34：画质≈原 H264，预期码率 420~520kb，10 秒视频裸流大概 530~650KB，就能小于原来 H264 的 856KB
    // - QP=36：画质低于源 H264，码率更低
    // rc FIXQP 配置，本地存储 1080*1920 30fps
    mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_init", 33);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 33);
    mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 33);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", fps *5); //30fps →150帧，5秒I帧
    mpp_enc_cfg_set_s32(cfg, "prep:split_sps_pps", 1);


    //帧率配置保留
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);

    ret = m_mpp_api->control(m_mpp_ctx, MPP_ENC_SET_CFG, cfg);
    mpp_enc_cfg_deinit(cfg);

    if (ret != MPP_OK)
    {
        fprintf(stderr, "MPP_ENC_SET_CFG failed ret:%d\n", ret);
        mpp_destroy(m_mpp_ctx);
        m_mpp_ctx = nullptr;
        m_mpp_api = nullptr;
        return false;
    }

    m_opened = true;
    fprintf(stdout, "MPP Encoder Info: open success w=%d h=%d fps=%d bitrate=%llu\n",
            w, h, fps, (unsigned long long)m_bitrate);
    return true;
}

bool MppEncoder::encode(const FrameBuffer& fb)
{
    if (!m_opened || !m_mpp_ctx || !m_mpp_api)
        return false;
    if (fb.fmt != PixelFormat::FMT_NV12)
    {
        fprintf(stderr, "MPP Encoder Error: only support FMT_NV12\n");
        return false;
    }

    MPP_RET ret = MPP_OK;
    MppFrame frame = nullptr;
    MppPacket packet = nullptr;

    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK)
    {
        fprintf(stderr, "MPP Encoder Error: mpp_frame_init fail\n");
        return false;
    }

    // 设置帧信息
    mpp_frame_set_width(frame, fb.width);
    mpp_frame_set_height(frame, fb.height);
    // 重要：如果你的fb带硬件padding，这里hor_stride要传入对齐后的stride，不是fb.width
    mpp_frame_set_hor_stride(frame, fb.stride);
    mpp_frame_set_ver_stride(frame, fb.height);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_pts(frame, fb.timestamp_ms * 1000); //单位微秒

    // 分配MppBuffer，把FrameBuffer的数据拷贝进MppBuffer
    MppBuffer buf = nullptr;
    size_t frame_size = fb.data_size;
    ret = mpp_buffer_get(NULL, &buf, frame_size);
    if (ret != MPP_OK)
    {
        fprintf(stderr, "MPP Encoder Error: mpp_buffer_get fail\n");
        mpp_frame_deinit(&frame);
        return false;
    }
    void* dst_ptr = mpp_buffer_get_ptr(buf);
    memcpy(dst_ptr, fb.data, frame_size);
    mpp_frame_set_buffer(frame, buf);
    mpp_buffer_put(buf);
    // 送入编码
    ret = m_mpp_api->encode_put_frame(m_mpp_ctx, frame);
    if (ret != MPP_OK)
    {
        fprintf(stderr, "MPP Encoder Error: encode_put_frame fail ret:%d\n", ret);
        // mpp_buffer_put(buf);
        mpp_frame_deinit(&frame);
        return false;
    }

    // 循环取出编码后的码流包
    do
    {
        ret = m_mpp_api->encode_get_packet(m_mpp_ctx, &packet);
        if (ret != MPP_OK)
        {
            fprintf(stderr, "MPP Encoder Error: encode_get_packet ret:%d\n", ret);
            break;
        }
        if (!packet)
            break;

        void* stream_ptr = mpp_packet_get_pos(packet);
        size_t stream_len = mpp_packet_get_length(packet);
        if (m_cb && stream_len > 0)
        {
            uint64_t pts_us = fb.timestamp_ms * 1000;
            m_cb(reinterpret_cast<uint8_t*>(stream_ptr), stream_len,switchCodingType(m_encoding_type), pts_us, m_user_arg);
        }

        mpp_packet_deinit(&packet);
    } while (ret == MPP_OK);
        
    mpp_frame_deinit(&frame);
    return true;
}

void MppEncoder::set_encode_callback(EncodeCallback cb, void* user_arg)
{
    m_cb = cb;
    m_user_arg = user_arg;
}

void MppEncoder::close()
{
    if(m_mpp_ctx)
    {
        mpp_destroy(m_mpp_ctx);
        m_mpp_ctx = nullptr;
    }
    m_cb = nullptr;
    m_user_arg = nullptr;
    m_opened = false;
}

bool MppEncoder::is_opened() const
{
    return m_opened;
}
