#include <iostream>
#include <chrono>
#include <thread>
#include "capture_factory.h"
#include "frame_buffer.h"
#include "rk_mpp_encoder.h"
#include "zlm_push_server.h"
extern "C" {
#include <libavformat/avformat.h>
}


std::mutex              m_param_mtx;
std::vector<uint8_t>    m_cache_vps;
std::vector<uint8_t>    m_cache_sps;
std::vector<uint8_t>    m_cache_pps;
void printMediaInfo(const std::string& filePath)
{
    AVFormatContext* fmt_ctx = avformat_alloc_context();
    if(!fmt_ctx)
    {
        std::cerr << "avformat_alloc_context fail\n";
        return;
    }
    int ret = avformat_open_input(&fmt_ctx, filePath.c_str(), nullptr, nullptr);
    if(ret < 0)
    {
        std::cerr << "open input failed\n";
        avformat_free_context(fmt_ctx);
        return;
    }
    avformat_find_stream_info(fmt_ctx, nullptr);
    int video_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1,-1,nullptr,0);
    if(video_idx >=0)
    {
        AVStream* st = fmt_ctx->streams[video_idx];
        std::cout << "\n==== Media Info ====\n";
        std::cout << "width: "<< st->codecpar->width <<"\n";
        std::cout << "height: "<< st->codecpar->height <<"\n";
        std::cout << "codec id: "<< avcodec_get_name(st->codecpar->codec_id) <<"\n";
        double fps = av_q2d(st->r_frame_rate);
        std::cout << "fps: "<< fps <<"\n";
        std::cout << "duration(s): "<< (double)fmt_ctx->duration / AV_TIME_BASE <<"\n";
    }
    avformat_close_input(&fmt_ctx);
    avformat_free_context(fmt_ctx);
}
// pkt_data 是带起始码的nalu，返回true并输出sc_len、nalu_type
bool get_hevc_nal_type(const uint8_t *pkt_data, int pkt_len, int &sc_len, uint8_t &nalu_type)
{
    sc_len = 0;
    if (pkt_len < 3)  // 最小起始码是3字节 00 00 01
        return false;

    // 3字节起始码 00 00 01
    if (pkt_data[0] == 0 && pkt_data[1] == 0 && pkt_data[2] == 1)
    {
        sc_len = 3;
    }
    // 4字节起始码 00 00 00 01
    else if (pkt_len >=4 && pkt_data[0] == 0 && pkt_data[1] == 0 && pkt_data[2] == 0 && pkt_data[3] == 1)
    {
        sc_len = 4;
    }
    else
    {
        return false;
    }

    if (pkt_len <= sc_len)
        return false;

    uint8_t hdr = pkt_data[sc_len];
    nalu_type = (hdr & 0x7E) >> 1;
    return true;
}

void sendFrame(const uint8_t* pkt_data, size_t pkt_len,CodingType type, uint64_t pts_us ,void* user_arg)
{
    ZlmServerStreamHandle handle = static_cast<ZlmServerStreamHandle>(user_arg);
    if (!handle || pkt_len == 0)
        return;
    // size_t offset = 0;
    // while (offset < pkt_len)
    // {
    //     int sc = 0;
    //     uint8_t ntype = 0;
    //     const uint8_t* nal_ptr = pkt_data + offset;
    //     int remain = static_cast<int>(pkt_len - offset);

    //     if (!get_hevc_nal_type(nal_ptr, remain, sc, ntype))
    //     {
    //         offset ++;
    //         continue;
    //     }

    //     // 找到当前NAL起始位置，找下一个起始码确定NAL长度
    //     size_t nal_begin = offset;
    //     size_t next_nal_pos = pkt_len;
    //     for (size_t i = offset + sc; i + 3 < pkt_len; i++)
    //     {
    //         if( (pkt_data[i]==0 && pkt_data[i+1]==0 && pkt_data[i+2]==1)
    //             || (pkt_data[i]==0 && pkt_data[i+1]==0 && pkt_data[i+2]==0 && pkt_data[i+3]==1) )
    //         {
    //             next_nal_pos = i;
    //             break;
    //         }
    //     }
    //     size_t nal_total_len = next_nal_pos - nal_begin;

    //     // 缓存VPS SPS PPS
    //     if(ntype == 32)
    //     {
    //         std::lock_guard<std::mutex> lk(m_param_mtx);
    //         m_cache_vps.assign(pkt_data + nal_begin, pkt_data + nal_begin + nal_total_len);
    //         zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, pkt_data+ nal_begin, nal_total_len, pts_us, pts_us);
    //         printf("got VPS\n");
    //     }
    //     else if(ntype == 33)
    //     {
    //         std::lock_guard<std::mutex> lk(m_param_mtx);
    //         m_cache_sps.assign(pkt_data + nal_begin, pkt_data + nal_begin + nal_total_len);
    //         zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, pkt_data+ nal_begin, nal_total_len, pts_us, pts_us);
    //         printf("got SPS\n");
    //     }
    //     else if(ntype == 34)
    //     {
    //         std::lock_guard<std::mutex> lk(m_param_mtx);
    //         m_cache_pps.assign(pkt_data + nal_begin, pkt_data + nal_begin + nal_total_len);
    //         zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, pkt_data+ nal_begin, nal_total_len, pts_us, pts_us);
    //         printf("got PPS\n");
    //     }
        

    //     offset = next_nal_pos;
    // }
    // printf("pre send frame\n");

    // // nal：指向【包含 00 00 00 01 起始码】的buffer，nal_len是总长度
    // int sc = 0;
    // uint8_t ntype = 0;
    // if(get_hevc_nal_type(pkt_data, pkt_len, sc, ntype))
    // {
    //     if(ntype == 32)
    //     {
    //         std::lock_guard<std::mutex> lk(m_param_mtx);
    //         m_cache_vps.assign(pkt_data, pkt_data + pkt_len); // 整包存，带起始码
    //         //**32 = VPS（Video Parameter Set，视频参数集）**：HEVC 独有的，描述整个视频序列的层级、profile、tier、层相关信息，H264 没有 VPS
    //         printf("get_hevc_nal_type - VPS\n");
            
    //     }
    //     else if(ntype == 33)
    //     {
    //         std::lock_guard<std::mutex> lk(m_param_mtx);
    //         m_cache_sps.assign(pkt_data, pkt_data + pkt_len);
    //         //**33 = SPS（Sequence Parameter Set，序列参数集）**：分辨率、色度格式、帧尺寸、CTU 大小、帧序号相关全局配置
    //         printf("get_hevc_nal_type - SPS\n");
    //     }
    //     else if(ntype == 34)
    //     {
    //         std::lock_guard<std::mutex> lk(m_param_mtx);
    //         m_cache_pps.assign(pkt_data, pkt_data + pkt_len);
    //         //**34 = PPS（Picture Parameter Set，图像参数集）**：片、量化、滤波等单图像相关参数
    //         printf("get_hevc_nal_type - PPS\n");
    //     }
    // }
    ZlmFrameCodec codingType=(type==CodingType::VIDEO_CodingH264)?ZLM_FRAME_VIDEO_H264:ZLM_FRAME_VIDEO_H265;
    int ret = zlm_rtsp_input_encodeData(handle,codingType , pkt_data, pkt_len, pts_us, pts_us);
    if(ret != ZLM_ERR_OK)
    {
        std::cerr << "send rtsp frame err:" << ret << "\n";
    }
}


int main(int argc, char** argv)
{
    std::cout << " ZLM RTSP Pusher (LOOP PLAY) ====\n";
    auto cap = CreateCapture(CaptureType::CAPTURE_FFMPEG);
    if (!cap)
    {
        std::cerr << "create capture failed\n";
        return -1;
    }
    std::string fileName="2.mp4";
    printMediaInfo(fileName);

    // =========【只初始化一次 RTSP服务】放在循环外面 =========
    int server_code = 0;
    int outCode=0;
    ZlmServerStreamHandle handle = zlm_rtsp_onekey_start(8550,ZLM_FRAME_VIDEO_H265,"live","ch0",1920,1080,30,
        nullptr,nullptr,&outCode);
    if (!handle)
    {
        std::cerr << "rtsp server create fail, code:" << server_code << std::endl;
        return -1;
    }
    printf("###  start server handle: %p, port=%d\n", handle, 8550);

    // std::cout << "RTSP server start, listen on 8550\n";

    FrameBuffer fb;
    int hw_stride = ((1080 + 127)/128)*128;
    size_t buf_size = (size_t)hw_stride * 1920 * 3 / 2;
    if (!fb.allocate(buf_size))
    {
        return -1;
    }
    MppEncoder enc;
    bool encoder_ready = false;
    enc.set_encode_callback(sendFrame, handle);


    while(true)
    {
        std::cout << "\n========== Reopen file, start new loop ==========\n";


        if (!cap->open(fileName))
        {
            std::cerr << "open file failed:" << fileName << "\n";
            break;
        }
        int cnt = 0;
        while (cap->read_frame(fb))
        {
            auto frame_start = std::chrono::high_resolution_clock::now();
            cnt++;
            // std::cout << "frame " << cnt << " w:" << fb.width << " h:" << fb.height << "\n";
            if(!encoder_ready)
            {
                // std::cout << " -> pre open encoder\n";
                if(enc.open(fb.width, fb.height,CodingType::VIDEO_CodingH265, 30,500))
                {
                    encoder_ready = true;
                    // std::cout << " -> encoder open success\n";
                }else{
                    std::cerr << "encoder open fail!\n";
                    fb.release();
                    cap->close();
                    zlm_rtsp_stop(handle);
                    goto exit_all;
                }
            }
            if(enc.encode(fb))
            {
                // printf(" --encode frame ok, push to rtsp\n");
            }
            auto frame_end = std::chrono::high_resolution_clock::now();
            long long frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
            // std::cout << " --single frame cost: " << std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count() << " ms\n";
        }
        cap->close();
        std::cout << "==== One loop finished, total frames: " << cnt << " ====\n";
        
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
exit_all:
    zlm_rtsp_stop(handle);
    cap->close();
    // 销毁server句柄，如果没有剩余通道，服务关闭（你的引用计数逻辑）
    std::cout << "Exit\n";
    return 0;
}
