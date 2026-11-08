// T3 验收：config/node.json（v1）结构自检 + vi/push 参数一致性 + 与 v0 push.json 等价 + 与素材实测比对
//
// 用法：t3_node_json <node.json> [expect_w expect_h expect_fps]
//   后三个参数由 run_t3_node_json.sh 用 ffprobe 探测 ch1~ch3 的 mp4 后传入；
//   文件不存在时脚本不传，相关断言打印 SKIP。
// 记录见 docs/TESTLOG.md §5.7（L2 · T3）。
#include "node_config_manager.h"

#include <json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using evs::config::NodeConfigManager;
using evs::config::NodeDesc;
using evs::config::PipelineDesc;
using json = nlohmann::json;

static int g_pass = 0;
static int g_fail = 0;
static int g_skip = 0;

static void ok(const std::string &what)
{
    ++g_pass;
    std::printf("ok   : %s\n", what.c_str());
}

static void bad(const std::string &what)
{
    ++g_fail;
    std::printf("FAIL : %s\n", what.c_str());
}

static void skip(const std::string &what)
{
    ++g_skip;
    std::printf("SKIP : %s\n", what.c_str());
}

static void check(bool cond, const std::string &what)
{
    if (cond)
        ok(what);
    else
        bad(what);
}

static bool GetSeg(const std::string &wrapped, const std::string &seg, json &out)
{
    try
    {
        const json j = json::parse(wrapped);
        if (!j.is_object() || !j.contains(seg) || !j[seg].is_object())
            return false;
        out = j[seg];
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// 取某条链路里指定 type 的第一个节点；找不到返回 nullptr
static const NodeDesc *FindNode(const PipelineDesc &p, const std::string &type)
{
    for (size_t i = 0; i < p.nodes.size(); ++i)
    {
        if (p.nodes[i].type == type)
            return &p.nodes[i];
    }
    return nullptr;
}

static bool StartsWith(const std::string &s, const std::string &prefix)
{
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// 把 pipeline 里每个节点的队列名（name + channel）收集起来查重
static void CheckQueueNames(const std::vector<PipelineDesc> &ps)
{
    std::vector<std::string> all;
    for (size_t i = 0; i < ps.size(); ++i)
    {
        for (size_t k = 0; k < ps[i].nodes.size(); ++k)
            all.push_back(ps[i].nodes[k].name + std::to_string(ps[i].nodes[k].channel));
    }
    bool dup = false;
    std::string dup_name;
    for (size_t i = 0; i < all.size() && !dup; ++i)
    {
        for (size_t k = i + 1; k < all.size(); ++k)
        {
            if (all[i] == all[k])
            {
                dup = true;
                dup_name = all[i];
                break;
            }
        }
    }
    check(!dup, "队列名（name+channel）全局唯一" + (dup ? "，冲突：" + dup_name : std::string()));
    std::printf("info : 共 %zu 个节点队列名\n", all.size());
}

// 直接读配置文件原文，确认 push 节点里没写 port / app（D3：端口与 app 只由 global 提供，
// configmgr 会在快照里把 global 的值注入 push 段，所以只能查原文）
static void CheckRawNoPushPort(const std::string &path)
{
    std::ifstream ifs(path.c_str());
    if (!ifs)
    {
        bad("无法读取配置文件原文：" + path);
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    try
    {
        const json j = json::parse(text);
        int offending = 0;
        std::string where;
        const json &ps = j.at("pipelines");
        for (size_t i = 0; i < ps.size(); ++i)
        {
            if (!ps[i].contains("nodes"))
                continue;
            const json &ns = ps[i]["nodes"];
            for (size_t k = 0; k < ns.size(); ++k)
            {
                if (ns[k].value("type", std::string()) != "push")
                    continue;
                if (!ns[k].contains("config") || !ns[k]["config"].is_object())
                    continue;
                const json &c = ns[k]["config"];
                if (c.contains("port") || c.contains("app"))
                {
                    ++offending;
                    where += " pipelines[" + std::to_string(i) + "].nodes[" + std::to_string(k) + "]";
                }
            }
        }
        check(offending == 0,
              "配置文件原文里 push 节点不写 port/app（端口与 app 只来自 global）" + where);
    }
    catch (const std::exception &e)
    {
        bad(std::string("解析配置文件原文失败：") + e.what());
    }
}

int main(int argc, char **argv)
{
    const std::string path = (argc > 1) ? std::string(argv[1]) : std::string("config/node.json");
    const bool has_expect = (argc >= 5);
    const int expect_w = has_expect ? std::atoi(argv[2]) : 0;
    const int expect_h = has_expect ? std::atoi(argv[3]) : 0;
    const int expect_fps = has_expect ? std::atoi(argv[4]) : 0;

    NodeConfigManager &mgr = NodeConfigManager::getInstance();

    std::printf("=== A. 加载与 global 段 ===\n");
    check(mgr.load(path), "node.json 通过 NodeConfigManager 校验");
    check(!mgr.is_v0(), "按 v1 解析（不是 v0 适配）");
    check(mgr.pipelines().size() == 4, "4 条链路");
    if (mgr.pipelines().size() != 4)
    {
        std::printf("==== 结构不符，后续断言无法进行 (fails=%d) ====\n", g_fail);
        return 1;
    }

    const evs::config::GlobalDesc &g = mgr.global();
    check(g.media.port == 8550, "global.media.rtsp.port = 8550");
    check(g.media.app == "live", "global.media.rtsp.app = live");
    check(g.media.bind_ip == "0.0.0.0", "global.media.rtsp.bind_ip = 0.0.0.0");
    check(g.pool.blocks == 64, "global.pool.blocks = 64");
    check(g.pool.queue_depth == 8, "global.pool.queue_depth = 8");
    check(g.pool.quota_per_channel == 10, "global.pool.quota_per_channel = 10");
    check(g.log.level == "info", "global.log.level = info");
    check(g.pool.images.size() == 1 && g.pool.images[0].name == "nv12" &&
              g.pool.images[0].width == 1920 && g.pool.images[0].height == 1080,
          "global.pool.images = 1x nv12 1920x1080 (D9 共享池)");

    std::printf("=== B. 每条链路的结构 ===\n");
    for (size_t i = 0; i < mgr.pipelines().size(); ++i)
    {
        const PipelineDesc &p = mgr.pipelines()[i];
        const std::string tag = "pipelines[" + std::to_string(i) + "](" + p.name + ")";
        check(p.name == "ch" + std::to_string(i), tag + " name = ch<i>");
        check(p.channel == static_cast<int>(i), tag + " channel = 数组下标");
        check(p.enable, tag + " enable = true");
    }
    check(mgr.pipelines()[0].nodes.size() == 4, "ch0 有 4 个节点（vi/vpss/push/print）");
    for (int i = 1; i < 4; ++i)
        check(mgr.pipelines()[i].nodes.size() == 3,
              "ch" + std::to_string(i) + " 有 3 个节点（vi/push/print）");
    check(mgr.pipelines()[0].nodes[1].type == "vpss", "ch0 第 2 个节点是 vpss");

    json seg;
    if (GetSeg(mgr.pipelines()[0].nodes[1].config_json, "vpss", seg))
        check(seg.value("enable", true) == false, "ch0 的 vpss 当前为 enable=false（不在链路上）");
    else
        bad("ch0 vpss config_json 结构异常");

    std::printf("=== C. vi.src 前缀与 loop（DESIGN §5.4）===\n");
    {
        const NodeDesc *vi0 = FindNode(mgr.pipelines()[0], "vi");
        check(vi0 != nullptr, "ch0 有 vi 节点");
        if (vi0 && GetSeg(vi0->config_json, "vi", seg))
        {
            check(StartsWith(seg.value("src", std::string()), "dev:"), "ch0 vi.src 以 dev: 开头");
            check(seg.value("src", std::string()) == "dev:/dev/video0", "ch0 vi.src = dev:/dev/video0");
            check(!seg.contains("loop"), "ch0 vi 不写 loop（设备源无意义）");
        }
        else
            bad("ch0 vi config_json 结构异常");

        for (int i = 1; i < 4; ++i)
        {
            const NodeDesc *vi = FindNode(mgr.pipelines()[i], "vi");
            const std::string tag = "ch" + std::to_string(i);
            if (!vi || !GetSeg(vi->config_json, "vi", seg))
            {
                bad(tag + " vi config_json 结构异常");
                continue;
            }
            check(StartsWith(seg.value("src", std::string()), "file:"), tag + " vi.src 以 file: 开头");
            check(seg.value("loop", false) == true, tag + " vi.loop = true（T7b 前必须显式写）");
        }
    }

    std::printf("=== D. vi 与 push 参数一致 + 端口只在 global（D3）===\n");
    CheckRawNoPushPort(path);
    for (size_t i = 0; i < mgr.pipelines().size(); ++i)
    {
        const PipelineDesc &p = mgr.pipelines()[i];
        const std::string tag = "ch" + std::to_string(i);
        const NodeDesc *vi = FindNode(p, "vi");
        const NodeDesc *pu = FindNode(p, "push");
        if (!vi || !pu)
        {
            bad(tag + " 缺少 vi 或 push 节点");
            continue;
        }
        json viSeg, puSeg;
        if (!GetSeg(vi->config_json, "vi", viSeg) || !GetSeg(pu->config_json, "push", puSeg))
        {
            bad(tag + " config_json 结构异常");
            continue;
        }
        // FfmpegCapture 不缩放（T7b 前）：vi 与 push 尺寸不一致 → 花屏
        check(viSeg.value("width", 0) == puSeg.value("width", 0), tag + " vi.width == push.width");
        check(viSeg.value("height", 0) == puSeg.value("height", 0), tag + " vi.height == push.height");
        check(viSeg.value("fps", 0) == puSeg.value("fps", 0), tag + " vi.fps == push.fps");
        check(viSeg.value("width", 0) % 2 == 0 && viSeg.value("height", 0) % 2 == 0,
              tag + " 宽高为偶数");
        // configmgr 会把 global.media.rtsp.port / app 注入 push 段（D3：端口唯一来源）
        check(puSeg.value("port", 0) == g.media.port, tag + " push.port = global 端口（注入）");
        check(puSeg.value("app", std::string()) == g.media.app, tag + " push.app = global app（注入）");
        check(puSeg.value("stream", std::string()) == p.name, tag + " push.stream == 链路名");
        check(puSeg.value("codec", std::string()) == "h265", tag + " push.codec = h265");
        check(puSeg.value("protocol", std::string()) == "rtsp", tag + " push.protocol = rtsp");
    }
    CheckQueueNames(mgr.pipelines());

    std::printf("=== E. 与素材实测参数比对（防花屏）===\n");
    if (!has_expect)
    {
        skip("未提供 ffprobe 期望值（素材不存在），跳过与实测的比对");
    }
    else
    {
        std::printf("info : 期望 宽=%d 高=%d fps=%d（ffprobe 探测 ch1~ch3 得到）\n", expect_w, expect_h,
                    expect_fps);
        for (int i = 1; i < 4; ++i)
        {
            const NodeDesc *vi = FindNode(mgr.pipelines()[i], "vi");
            const std::string tag = "ch" + std::to_string(i);
            if (!vi || !GetSeg(vi->config_json, "vi", seg))
            {
                bad(tag + " vi config_json 结构异常");
                continue;
            }
            check(seg.value("width", 0) == expect_w, tag + " vi.width 与素材一致");
            check(seg.value("height", 0) == expect_h, tag + " vi.height 与素材一致");
            check(seg.value("fps", 0) == expect_fps, tag + " vi.fps 与素材一致（±1 取整）");
        }
    }

    std::printf("=== F. 与 v0 config/push.json 等价（ch0 = 今天的单链路）===\n");
    {
        json v0_push, v0_print;
        bool got_v0 = false;
        check(mgr.load("config/push.json"), "v0 push.json 可加载（对照组）");
        if (mgr.is_v0() && mgr.pipelines().size() == 1)
        {
            const PipelineDesc &p0 = mgr.pipelines()[0];
            const NodeDesc *pu = FindNode(p0, "push");
            const NodeDesc *pr = FindNode(p0, "print");
            got_v0 = (pu && pr && GetSeg(pu->config_json, "push", v0_push) &&
                      GetSeg(pr->config_json, "print", v0_print));
        }
        check(got_v0, "取到 v0 的 push/print 段");

        json v1_push, v1_print;
        bool got_v1 = false;
        check(mgr.load(path), "重新加载 v1 node.json");
        if (mgr.pipelines().size() == 4)
        {
            const PipelineDesc &p0 = mgr.pipelines()[0];
            const NodeDesc *pu = FindNode(p0, "push");
            const NodeDesc *pr = FindNode(p0, "print");
            got_v1 = (pu && pr && GetSeg(pu->config_json, "push", v1_push) &&
                      GetSeg(pr->config_json, "print", v1_print));
        }
        check(got_v1, "取到 v1 ch0 的 push/print 段");

        if (got_v0 && got_v1)
        {
            check(v1_push.value("width", 0) == v0_push.value("width", 0), "push.width 与 v0 一致");
            check(v1_push.value("height", 0) == v0_push.value("height", 0), "push.height 与 v0 一致");
            check(v1_push.value("fps", 0) == v0_push.value("fps", 0), "push.fps 与 v0 一致");
            check(v1_push.value("bitrate_kbps", 0) == v0_push.value("bitrate_kbps", 0),
                  "push.bitrate_kbps 与 v0 一致");
            check(v1_push.value("gop", 0) == v0_push.value("gop", 0), "push.gop 与 v0 一致");
            check(v1_push.value("codec", std::string()) == v0_push.value("codec", std::string()),
                  "push.codec 与 v0 一致");
            check(v1_push.value("protocol", std::string()) == v0_push.value("protocol", std::string()),
                  "push.protocol 与 v0 一致");
            check(v1_push.value("transport", std::string()) == v0_push.value("transport", std::string()),
                  "push.transport 与 v0 一致");
            check(v1_push.value("stream", std::string()) == v0_push.value("stream", std::string()),
                  "push.stream 与 v0 一致");
            check(v1_push.value("enable", false) == v0_push.value("enable", false), "push.enable 与 v0 一致");
            // v1 的 ch0 没写 port/app，应被 configmgr 从 global 注入，值必须与 v0 相同
            check(v1_push.value("port", 0) == v0_push.value("port", 0), "push.port 由 global 注入后与 v0 一致");
            check(v1_push.value("app", std::string()) == v0_push.value("app", std::string()),
                  "push.app 由 global 注入后与 v0 一致");
            check(v1_print.value("interval", 0) == v0_print.value("interval", 0), "print.interval 与 v0 一致");
            check(v1_print.value("enable", false) == v0_print.value("enable", false),
                  "print.enable 与 v0 一致");
        }
    }

    std::printf("==== T3 node.json 自检结果: passes=%d fails=%d skips=%d ====\n", g_pass, g_fail, g_skip);
    return (g_fail == 0) ? 0 : 1;
}
