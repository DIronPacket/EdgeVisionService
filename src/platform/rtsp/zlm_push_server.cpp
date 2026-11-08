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

struct RtspServerCtx
{
    // P0-6：注册与注销共用这一个键。原实现用 bind_ip 拼接，而 bind_ip 从未被赋值，
    // 导致 stop 拼出 ":8550" 与注册的 "8550" 不相等，停止路径静默失效。
    std::string key;
    uint16_t port; // 实际监听端口（mk_rtsp_server_start 的返回值）
    int ref_cnt = 0;
};
using RtspServerCtxPtr = std::shared_ptr<RtspServerCtx>;
static std::unordered_map<std::string, RtspServerCtxPtr> g_server_map;

struct OneKeyStreamCtx
{
    RtspServerCtxPtr server;
    // P0-6：注册时写入、注销时直接读，避免 start / stop 各拼一次字符串
    std::string key;
    std::string app;
    std::string stream;
    mk_media media = nullptr;
    // ZlmServerStreamEventCb cb = nullptr;
    // void* cb_arg = nullptr;
};
using OneKeyStreamCtxPtr = std::shared_ptr<OneKeyStreamCtx>;
static std::unordered_map<std::string, OneKeyStreamCtxPtr> g_stream_map;

// ============================================================
// key 的唯一生成入口（P0-6）
// onekey_start 注册与 zlm_rtsp_stop 注销必须使用同一组函数；
// 任何"另一处自己拼字符串"都会让停止路径再次静默失效。
// 服务键 = 请求端口；流键 = 实际端口/app/stream。
// ============================================================
static std::string ServerKey(uint16_t port)
{
    return std::to_string(port);
}
static std::string StreamKey(uint16_t port, const std::string &app, const std::string &stream)
{
    return ServerKey(port) + "/" + app + "/" + stream;
}

// ============================================================
// 服务引用释放的唯一入口（P0-6 后半）
// ref_cnt 减到 0 就停服务并从 g_server_map 注销。
// 新增失败回滚、重复注册回滚、正常 stop 三条路径都必须走这里：
// 原实现在"重复注册（STREAM_EXIST）"分支已经 ref_cnt++ 却直接返回，
// 每重复注册一次就漏一个引用，最后一次 stop 归不到 0 —— server 永久滞留
// （表现为 streams=0 而 servers=1，mk_stop_all_server 再也不被调用）。
// 调用方必须已持有 g_mtx。
// ============================================================
static void ReleaseServerRef(const RtspServerCtxPtr &srv)
{
    if (!srv)
        return;
    if (srv->ref_cnt > 0)
        srv->ref_cnt--;
    if (srv->ref_cnt <= 0)
    {
        // 注意：mk_stop_all_server() 停的是本进程所有 RTSP 服务，
        // 单进程多端口场景会误伤其他端口 —— 由 M1 T4 的 MediaServer 收敛。
        // 另注：监听 socket 由 ZLM 的 TcpServer 析构异步关闭，stop 返回瞬间端口
        // 可能仍被占用，同端口立刻重新 start 会 bind 失败（实测见 docs/TESTLOG.md T1）。
        mk_stop_all_server();
        g_server_map.erase(srv->key);
    }
}
// 独立函数
bool zlm_hevc_send_param_set(ZlmServerStreamHandle handle,
                             const std::vector<uint8_t> &vps,
                             const std::vector<uint8_t> &sps,
                             const std::vector<uint8_t> &pps)
{
    if (vps.empty() || sps.empty() || pps.empty())
        return false;
    zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, vps.data(), vps.size(), 0, 0);
    zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, sps.data(), sps.size(), 0, 0);
    zlm_rtsp_input_encodeData(handle, ZLM_FRAME_VIDEO_H265, pps.data(), pps.size(), 0, 0);
    return true;
}
ZlmServerStreamHandle zlm_rtsp_onekey_start(uint16_t rtsp_port, ZlmFrameCodec type,
                                            const char *app, const char *stream,
                                            int w, int h, int fps,
                                            ZlmOutputCb event_cb, void *user_data,
                                            int *out_code)
{
    // P0-7：out_code 是唯一回传错误码的出口，必须最先判空并直接返回。
    // 原实现先判空、紧接着又写 *out_code，传 nullptr 时必崩。
    if (!out_code)
        return nullptr;
    if (!app || !stream || app[0] == '\0' || stream[0] == '\0' ||
        w <= 0 || h <= 0 || fps <= 0)
    {
        *out_code = ZLM_ERR_INVALID_PARAM;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    // env全局一次性初始化，用你现在这个mk_env_init2参数
    if (!g_env_ok)
    {
        mk_env_init2(4, 2, LOG_CONSOLE, NULL, 0, 0, NULL, 0, NULL, NULL);
        g_env_ok = true;
    }

    // 服务 key 只使用请求端口（不带 ip）
    const std::string srv_key = ServerKey(rtsp_port);

    // 判重必须放在"取/建 server 并 ref_cnt++"之前：重复注册时若先加引用再返回，
    // 最后一个流 stop 时 ref_cnt 归不到 0，server 会永久滞留在 g_server_map。
    const std::string probe_key = StreamKey(rtsp_port, app, stream);
    auto it_st0 = g_stream_map.find(probe_key);
    if (it_st0 != g_stream_map.end())
    {
        *out_code = ZLM_ERR_STREAM_EXIST;
        return it_st0->second.get();
    }

    RtspServerCtxPtr srv;
    auto it_srv = g_server_map.find(srv_key);
    if (it_srv != g_server_map.end())
    {
        srv = it_srv->second;
        srv->ref_cnt++;
    }
    else
    {
        srv = std::make_shared<RtspServerCtx>();
        srv->key = srv_key;
        srv->port = rtsp_port;
        srv->ref_cnt = 1;
        uint16_t real_p = mk_rtsp_server_start(rtsp_port, 0);
        if (real_p == 0)
        {
            *out_code = ZLM_ERR_INTERNAL;
            return nullptr;
        }
        srv->port = real_p;
        g_server_map[srv_key] = srv;
    }

    // 流 key：实际端口/app/stream（与 stop 使用同一个生成函数，P0-6）
    const std::string st_key = StreamKey(srv->port, app, stream);
    auto it_st = g_stream_map.find(st_key);
    if (it_st != g_stream_map.end())
    {
        // 只有 rtsp_port == 0（由 ZLM 分配随机端口）时才会走到这里：实际端口与
        // 上面 probe_key 用的请求端口不同。此时 server 引用已经 +1，必须回滚。
        ReleaseServerRef(srv);
        *out_code = ZLM_ERR_STREAM_EXIST;
        return it_st->second.get();
    }

    auto st = std::make_shared<OneKeyStreamCtx>();
    st->server = srv;
    st->key = st_key;
    st->app = app;
    st->stream = stream;
    // st->cb = event_cb;
    // st->cb_arg = user_data;
    // 沿用你现在这个mk_media_create原型
    st->media = mk_media_create("__defaultVhost__", app, stream, 0.0f, 0, 0);
    if (!st->media)
    {
        *out_code = ZLM_ERR_INTERNAL;
        ReleaseServerRef(srv);
        return nullptr;
    }
    // 沿用你这边 mk_media_init_video 参数
    int codeId = (type == ZlmFrameCodec::ZLM_FRAME_VIDEO_H264) ? 0 : 1;
    mk_media_init_video(st->media, codeId, w, h, fps, 2000000);

    g_stream_map[st_key] = st;
    *out_code = ZLM_ERR_OK;
    return st.get();
}

int zlm_rtsp_input_encodeData(ZlmServerStreamHandle h, ZlmFrameCodec type, const uint8_t *data, int len, uint64_t dts, uint64_t pts)
{
    if (!h || !data || len <= 0)
        return ZLM_ERR_INVALID_PARAM;
    std::lock_guard<std::mutex> lk(g_mtx);
    // 先按句柄在表内定位，再访问 ctx 成员：句柄被 stop 之后 ctx 已释放，
    // 直接 static_cast 解引用就是 use-after-free（修掉 P0-6 后 stop 真的会销毁 ctx）
    OneKeyStreamCtx *raw = nullptr;
    for (auto it = g_stream_map.begin(); it != g_stream_map.end(); ++it)
    {
        if (it->second.get() == static_cast<OneKeyStreamCtx *>(h))
        {
            raw = it->second.get();
            break;
        }
    }
    if (!raw || !raw->media)
        return ZLM_ERR_NOT_FOUND;
    int sendSize = 0;
    if (type == ZlmFrameCodec::ZLM_FRAME_VIDEO_H264)
    {
        sendSize = mk_media_input_h264(raw->media, data, len, dts, pts);
    }
    else if (type == ZlmFrameCodec::ZLM_FRAME_VIDEO_H265)
    {
        sendSize = mk_media_input_h265(raw->media, data, len, dts, pts);
    }
    // printf("send %s size:%d pts:%u\n", type == ZLM_FRAME_VIDEO_H265 ? "h265" : "h264", sendSize, pts);
    return ZLM_ERR_OK;
}

void zlm_rtsp_stop(ZlmServerStreamHandle h)
{
    if (!h)
        return;
    std::lock_guard<std::mutex> lk(g_mtx);
    // P0-6：先按句柄在表内定位，不先解引用 h —— 已经 stop 过的句柄指向已释放的 ctx，
    // 先解引用就是 use-after-free。定位成功后直接用 ctx 里保存的 key 注销，
    // 与注册共用同一串，不再出现"两处各拼一次导致不一致"。
    auto it_st = g_stream_map.end();
    for (auto it = g_stream_map.begin(); it != g_stream_map.end(); ++it)
    {
        if (it->second.get() == static_cast<OneKeyStreamCtx *>(h))
        {
            it_st = it;
            break;
        }
    }
    if (it_st == g_stream_map.end())
        return; // 已注销，或不是本模块发出的句柄：幂等返回

    OneKeyStreamCtxPtr st = it_st->second;
    g_stream_map.erase(it_st);
    if (st->media)
    {
        mk_media_release(st->media);
        st->media = nullptr;
    }
    // 服务引用计数递减，0 则停服务
    RtspServerCtxPtr srv = st->server;
    st->server.reset();
    ReleaseServerRef(srv);
}

// ===== 自检接口（只读）=====
// 存在的理由：T1 的验收标准是"stop 之后 g_stream_map / g_server_map 均为空"，
// 这两张表是文件内 static，外部只能通过下面的函数断言，否则验收无法复现。
// 补充（见 docs/TESTLOG.md T1）：端口监听状态不能当判据 —— stop 之后监听 socket
// 由 ZLM 的 TcpServer 析构异步关闭，短时内 connect 仍可能成功。
int zlm_rtsp_stream_count(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return static_cast<int>(g_stream_map.size());
}

int zlm_rtsp_server_count(void)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    return static_cast<int>(g_server_map.size());
}
