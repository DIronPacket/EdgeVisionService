#include <iostream>
#include <chrono>
#include "capture_factory.h"
#include "frame_buffer.h"
#include "rk_mpp_encoder.h" // 引入RK MPP编码器头文件

#include <cstdio>  // fopen fwrite fclose

extern "C" {
#include <libavformat/avformat.h>
}
// 读取视频文件媒体信息
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
        // 帧率 r_frame_rate 分数
        double fps = av_q2d(st->r_frame_rate);
        std::cout << "fps: "<< fps <<"\n";
        std::cout << "duration(s): "<< (double)fmt_ctx->duration / AV_TIME_BASE <<"\n";
    }

    avformat_close_input(&fmt_ctx);
    avformat_free_context(fmt_ctx);
}
void sendFrame(const uint8_t* pkt_data, size_t pkt_len,CodingType type,uint64_t pts_us ,void* user_arg)
{
    FILE* fp = static_cast<FILE*>(user_arg);
    if (fp && pkt_len > 0)
    {
        fwrite(pkt_data, 1, pkt_len, fp);
        printf("write pkt-size:%d\n",pkt_len);
    }
}
int main(int argc, char** argv)
{
    auto cap = CreateCapture(CaptureType::CAPTURE_FFMPEG);
    if (!cap)
    {
        std::cerr << "create capture failed\n";
        return -1;
    }
    std::string fileName="1.mp4";
    printMediaInfo(fileName);
    if (!cap->open(fileName))
    {
        std::cerr << "open file failed:" << fileName << "\n";
        return -1;
    }
    std::cout << "open mp4 success\n";

    FrameBuffer fb;
    int hw_stride = ((1080 + 127)/128)*128; // 1080 →1152
    size_t buf_size = (size_t)hw_stride * 1920 * 3 / 2;
    if (!fb.allocate(buf_size))
        return -1;

    // ========= 打开h265输出文件 =========
    FILE* h265_fp = fopen("out.h265", "wb");
    if (!h265_fp)
    {
        std::cerr << "cannot open out.h265\n";
        return -1;
    }

    int cnt = 0;
    long long total_process_ms = 0;
    // 整个循环的总开始时间（全部30帧总耗时）
    auto total_start = std::chrono::high_resolution_clock::now();

    // ========== 初始化RK MPP H265编码器 ==========
    MppEncoder enc;
    // 先用第一帧分辨率打开编码器，先读到一帧再初始化编码器
    bool encoder_ready = false;
    enc.set_encode_callback(sendFrame,(void*)h265_fp);

    while (cap->read_frame(fb))
    {
        auto frame_start = std::chrono::high_resolution_clock::now();
        cnt++;
        std::cout << "frame " << cnt << " w:" << fb.width << " h:" << fb.height << "\n";
        
#if 1
        // 第一次拿到图像，初始化编码器
        if(!encoder_ready)
        {
            std::cout << " -> pre open encoder\n";
            // 25fps，你可以改成视频原生fps
            if(enc.open(fb.width, fb.height,CodingType::VIDEO_CodingH264, 30,500))
            {
                encoder_ready = true;
                std::cout << " -> encoder open success\n";
            }else{
                std::cerr << "encoder open fail!\n";
                fb.release();
                break;
            }
        }

        // ========== 送入帧进行H265编码 ==========
        std::string h265_bitstream;
        if(enc.encode(fb))
        {
            printf(" --successfully encode frame\n");
            // 把h265裸流写入文件
            // fwrite(h265_bitstream.data(), 1, h265_bitstream.size(), h265_fp);
        }
#endif

        auto frame_end = std::chrono::high_resolution_clock::now();
        long long frame_ms = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
        total_process_ms += frame_ms;

        // std::cout << " --single frame cost: " << frame_ms << " ms\n";

        // // 测试到第30帧直接退出，方便快速验证
        // if(cnt >=30)
        //     break;
    }

    cap->close();
    fclose(h265_fp);
    std::cout << "read frame end, total:" << cnt << "\n";
    return 0;
}
