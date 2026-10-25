#include "zlm_push_server.h"
#include <ZLMediaKit/mk_common.h>
#include <ZLMediaKit/mk_media.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <sstream>
#include <vector>

static std::mutex g_mtx;
static bool g_env_ok = false;

struct RtspServerCtx {
    std::string bind_ip;
    uint16_t port;
    int ref_cnt = 0;
};
using RtspServerCtxPtr = std::shared_ptr<RtspServerCtx>;
static std::unordered_map<std::string, RtspServerCtxPtr> g_server_map;

struct OneKeyStreamCtx {
    RtspServerCtxPtr server;
    std::string app;
    std::string stream;
    mk_media media = nullptr;
    // ZlmServerStreamEventCb cb = nullptr;
    // void* cb_arg = nullptr;
};
using OneKeyStreamCtxPtr = std::shared_ptr<OneKeyStreamCtx>;
static std::unordered_map<std::string, OneKeyStreamCtxPtr> g_stream_map;

static std::string ServerKey(const char* ip, uint16_t p) {
    std::ostringstream oss;
    oss << ip << ":" << p;
    return oss.str();
}
static std::string StreamKey(const char* ip, uint16_t p, const char* app, const char* stream) {
    std::ostringstream oss;
    oss << ip << ":" << p << "/" << app << "/" << stream;
    return oss.str();
}
// 独立函数
bool zlm_hevc_send_param_set(ZlmServerStreamHandle handle,
    const std::vector<uint8_t>& vps,
    const std::vector<uint8_t>& sps,
    const std::vector<uint8_t>& pps)
{
    if (vps.empty() || sps.empty() || pps.empty())
        return false;
    zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, vps.data(), vps.size(), 0, 0);
    zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, sps.data(), sps.size(), 0, 0);
    zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, pps.data(), pps.size(), 0, 0);
    return true;
}
ZlmServerStreamHandle zlm_rtsp_onekey_start(uint16_t rtsp_port,ZlmFrameCodec type,
                                            const char* app, const char* stream,
                                            int w, int h, int fps,
                                            ZlmOutputCb event_cb, void* user_data,
                                            int* out_code)
{
    if (!app || !stream || !out_code) {
        *out_code = ZLM_ERR_INVALID_PARAM;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    // env全局一次性初始化，用你现在这个mk_env_init2参数
    if (!g_env_ok) {
        mk_env_init2(4,2,LOG_CONSOLE,NULL,0,0,NULL,0,NULL,NULL);
        g_env_ok = true;
    }

    // 服务key只使用端口，不再带ip
    std::string srv_key = std::to_string(rtsp_port);
    RtspServerCtxPtr srv;
    auto it_srv = g_server_map.find(srv_key);
    if (it_srv != g_server_map.end()) {
        srv = it_srv->second;
        srv->ref_cnt ++;
    } else {
        srv = std::make_shared<RtspServerCtx>();
        srv->port = rtsp_port;
        srv->ref_cnt = 1;
        uint16_t real_p = mk_rtsp_server_start(rtsp_port, 0);
        if (real_p == 0) {
            *out_code = ZLM_ERR_INTERNAL;
            return nullptr;
        }
        srv->port = real_p;
        g_server_map[srv_key] = srv;
    }

    // 流key：端口/app/stream
    std::string st_key = std::to_string(srv->port) + "/" + app + "/" + stream;
    auto it_st = g_stream_map.find(st_key);
    if (it_st != g_stream_map.end()) {
        *out_code = ZLM_ERR_STREAM_EXIST;
        return it_st->second.get();
    }

    auto st = std::make_shared<OneKeyStreamCtx>();
    st->server = srv;
    st->app = app;
    st->stream = stream;
    // st->cb = event_cb;
    // st->cb_arg = user_data;
    // 沿用你现在这个mk_media_create原型
    st->media = mk_media_create("__defaultVhost__", app, stream,0.0f,0,0);
    if (!st->media) {
        *out_code = ZLM_ERR_INTERNAL;
        srv->ref_cnt --;
        if (srv->ref_cnt <= 0) {
            mk_stop_all_server();
            g_server_map.erase(srv_key);
        }
        return nullptr;
    }
    // 沿用你这边 mk_media_init_video 参数
    int codeId=(type==ZlmFrameCodec::ZLM_FRAME_VIDEO_H264)?0:1;
    mk_media_init_video(st->media,codeId, w, h, fps,2000000);

    g_stream_map[st_key] = st;
    *out_code = ZLM_ERR_OK;
    return st.get();
}

int zlm_rtsp_input_encodeData(ZlmServerStreamHandle h,ZlmFrameCodec type, const uint8_t* data, int len, uint64_t dts, uint64_t pts)
{
    if (!h || !data || len <= 0) return ZLM_ERR_INVALID_PARAM;
    auto* raw = static_cast<OneKeyStreamCtx*>(h);
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!raw->media) return ZLM_ERR_NOT_FOUND;
    int sendSize=0;
    if(type==ZlmFrameCodec::ZLM_FRAME_VIDEO_H264)
    {
        sendSize=mk_media_input_h264(raw->media, data, len, dts, pts);
    }
    else if(type==ZlmFrameCodec::ZLM_FRAME_VIDEO_H265)
    {
        sendSize=mk_media_input_h265(raw->media, data, len, dts, pts);
    }
    printf("send %s size:%d pts:%u\n", type == ZLM_FRAME_VIDEO_H265 ? "h265" : "h264", sendSize,pts);
    return ZLM_ERR_OK;
}

void zlm_rtsp_stop(ZlmServerStreamHandle h)
{
    if (!h) return;
    auto* raw = static_cast<OneKeyStreamCtx*>(h);
    std::lock_guard<std::mutex> lk(g_mtx);
    std::string st_key = StreamKey(raw->server->bind_ip.c_str(), raw->server->port, raw->app.c_str(), raw->stream.c_str());
    auto it_st = g_stream_map.find(st_key);
    if (it_st == g_stream_map.end()) return;
    auto st = it_st->second;
    if (st->media) {
        mk_media_release(st->media);
        st->media = nullptr;
    }
    g_stream_map.erase(it_st);
    // 服务引用计数递减，0则停服务
    auto srv = st->server;
    srv->ref_cnt --;
    std::string srv_key = ServerKey(srv->bind_ip.c_str(), srv->port);
    if (srv->ref_cnt <= 0) {
        mk_stop_all_server();
        g_server_map.erase(srv_key);
    }
}
