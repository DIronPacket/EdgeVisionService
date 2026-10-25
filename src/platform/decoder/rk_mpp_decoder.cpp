#include "rk_mpp_decoder.h"
#include <cstring>

MppDecoder::MppDecoder() = default;
MppDecoder::~MppDecoder()
{
    close();
}

bool MppDecoder::open(bool is_h265)
{
    if(m_opened) return false;
    MPP_RET ret = mpp_create(m_mpp_ctx, &m_mpp_api);
    if (ret != MPP_OK)
    {
        mpp_err("mpp_create fail ret:%d\n", ret);
        return false;
    }

    // 解码器配置：开启自动帧分割，裸流必备
    MppDecCfg cfg = nullptr;
    mpp_dec_cfg_init(&cfg);
    ret = m_mpp_api->control(m_mpp_ctx, MPP_DEC_GET_CFG, cfg);
    if (ret == MPP_OK)
    {
        // base:split_parse = 1 自动分割H265裸流
        mpp_dec_cfg_set_u32(cfg, "base:split_parse", 1);
        m_mpp_api->control(m_mpp_ctx, MPP_DEC_SET_CFG, cfg);
    }
    mpp_dec_cfg_deinit(cfg);

    // 【重要】强制关闭FBC压缩输出，输出原生NV12，防止拿到压缩图像
    MppFrameFormat out_fmt = MPP_FMT_YUV420SP;
    m_mpp_api->control(m_mpp_ctx, MPP_DEC_SET_OUTPUT_FORMAT, &out_fmt);

    m_opened = true;
    mpp_log("MppDecoder open success, HEVC\n");
    return true;
}

bool MppDecoder::decode(const uint8_t* data, size_t len)
{
    if (!m_opened || !m_mpp_ctx || !m_mpp_api)
        return false;

    MPP_RET ret = MPP_OK;
    MppPacket pkt = nullptr;
    // 初始化packet，装入裸码流
    ret = mpp_packet_init(&pkt, const_cast<uint8_t*>(data), len);
    if (ret != MPP_OK)
    {
        mpp_err("mpp_packet_init fail\n");
        return false;
    }

    // 送入码流包
    ret = m_mpp_api->decode_put_packet(m_mpp_ctx, pkt);
    mpp_packet_deinit(&pkt); // 送完即可释放packet

    if (ret != MPP_OK)
    {
        mpp_err("decode_put_packet ret:%d\n", ret);
        return false;
    }

    // 循环取出解码帧
    MppFrame frame = nullptr;
    do
    {
        ret = m_mpp_api->decode_get_frame(m_mpp_ctx, &frame);
        if (MPP_ERR_TIMEOUT == ret)
        {
            usleep(1000);
            continue;
        }
        if (ret != MPP_OK)
            break;

        if (frame)
        {
            // 检测分辨率变更（官方demo核心逻辑）
            if (mpp_frame_get_info_change(frame))
            {
                RK_U32 w = mpp_frame_get_width(frame);
                RK_U32 h = mpp_frame_get_height(frame);
                // 简易版本这里暂时先不处理，正式产品必须实现buffer重分配
                mpp_log("info change event\n");
                mpp_frame_deinit(&frame);
                continue;
            }

            // 拿到宽高、stride
            RK_U32 w = mpp_frame_get_width(frame);
            RK_U32 h = mpp_frame_get_height(frame);
            RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);

            // ⚠️ 重要：如果开启FBC，不能直接读buffer！
            // 简易方案：项目初始化时关闭FBC输出，强制输出非压缩NV12
            MppBuffer buf = mpp_frame_get_buffer(frame);
            void* buf_ptr = mpp_buffer_get_ptr(buf);

            // NV12 总大小 = Y平面(hor_stride*h) + UV平面(hor_stride * h/2)
            size_t total_size = hor_stride * h * 3 / 2;
            FrameBuffer fb{};
            fb.allocate(total_size);
            memcpy(fb.data, buf_ptr, total_size);
            fb.width = static_cast<int>(w);
            fb.height = static_cast<int>(h);
            fb.fmt = PixelFormat::FMT_NV12;

            if(m_cb)
            {
                m_cb(fb, m_user_arg);
            }

            mpp_frame_deinit(&frame);
        }
    } while (ret == MPP_OK);

    return true;
}

void MppDecoder::set_decode_callback(DecodeFrameCallback cb, void* user_arg)
{
    m_cb = std::move(cb);
    m_user_arg = user_arg;
}

void MppDecoder::close()
{
    if(m_mpp_ctx)
    {
        mpp_destroy(m_mpp_ctx);
        m_mpp_ctx = nullptr;
        m_mpp_api = nullptr;
    }
    m_cb = nullptr;
    m_user_arg = nullptr;
    m_opened = false;
}

bool MppDecoder::is_opened() const
{
    return m_opened;
}
