// T2 验收程序：NodeConfigManager（v1 解析 / 校验 / v0 适配 / 引用展开 / 快照语义）
// 用 scripts/tests/run_t2_node_config.sh 编译并运行（不碰硬件、不起 RTSP 服务）。
// 记录见 docs/history/TESTLOG.md §5.6（L2 · T2）。
#include "node_config_manager.h"

#include <json.hpp>

#include <cstdio>
#include <string>
#include <vector>

using evs::config::NodeConfigManager;
using json = nlohmann::json;

static int g_pass = 0;
static int g_fail = 0;

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

static void check(bool cond, const std::string &what)
{
    if (cond)
        ok(what);
    else
        bad(what);
}

// 取 {"<seg>": {...}} 里的段；解析失败或用例写错时返回空对象（断言会自然失败）
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

// 断言"必须被拒绝"，且错误信息里出现指定字段路径
static void expect_reject(const std::string &path, const std::string &expect_in_error,
                          const std::string &what)
{
    NodeConfigManager &m = NodeConfigManager::getInstance();
    if (m.load(path))
    {
        bad(what + "：本应被拒绝，却加载成功了");
        return;
    }
    const std::string err = m.last_error();
    if (err.find(expect_in_error) == std::string::npos)
    {
        bad(what + "：错误信息缺少 \"" + expect_in_error + "\"，实际：" + err);
        return;
    }
    ok(what + " -> " + err);
}

int main(int argc, char **argv)
{
    const std::string dir = (argc > 1) ? std::string(argv[1]) : std::string("scripts/tests/fixtures");
    NodeConfigManager &mgr = NodeConfigManager::getInstance();

    std::printf("=== A. v1 正常解析 + 引用展开 + global 注入 ===\n");
    {
        const std::string path = dir + "/node_v1_ok_ref.json";
        check(mgr.load(path), "v1 样例加载成功");
        check(!mgr.is_v0(), "is_v0() = false（走 v1 分支）");
        check(mgr.source_path() == path, "source_path() 指向实际加载的文件");
        check(mgr.pipelines().size() == 2, "解析出 2 条链路");
        check(mgr.last_error().empty(), "成功后 last_error() 为空");

        if (mgr.pipelines().size() == 2)
        {
            const evs::config::PipelineDesc &p0 = mgr.pipelines()[0];
            const evs::config::PipelineDesc &p1 = mgr.pipelines()[1];
            check(p0.name == "ch0" && p0.channel == 0, "pipelines[0] = ch0 / channel 0");
            check(p0.enable && !p1.enable, "enable 生效（ch0=true / ch1=false）");
            check(p0.nodes.size() == 3 && p1.nodes.size() == 2, "节点数 = 3 / 2");
            check(p0.nodes[0].type == "vi" && p0.nodes[0].name == "vi", "name 缺省 = type");
            check(p0.nodes[0].path == "pipelines[0].nodes[0]", "NodeDesc.path 记录了字段路径");
            check(p0.nodes[0].channel == 0 && p1.nodes[0].channel == 1, "节点继承所属链路的 channel");

            json seg;
            if (GetSeg(p0.nodes[1].config_json, "push", seg))
            {
                ok("config_json 已包成 {\"push\":{...}}");
                check(seg.value("port", 0) == 18500, "${global.media.rtsp.port} 展开为数字 18500");
                check(seg.value("stream", std::string()) == "ch0", "stream 原样保留");
            }
            else
            {
                bad("config_json 结构不是 {\"push\":{...}}");
            }

            if (GetSeg(p1.nodes[1].config_json, "push", seg))
            {
                check(seg.value("port", 0) == 18500, "ch1 未写 port -> 注入 global.media.rtsp.port");
                check(seg.value("app", std::string()) == "live", "ch1 未写 app -> 注入 global.media.rtsp.app");
            }
            else
            {
                bad("ch1 config_json 结构异常");
            }

            if (GetSeg(p0.nodes[0].config_json, "vi", seg))
                check(seg.value("fps", 0) == 25, "vi 段字段原样透传");
            else
                bad("vi config_json 结构异常");
        }

        check(mgr.global().pool.blocks == 32, "global.pool.blocks = 32");
        check(mgr.global().pool.queue_depth == 4, "global.pool.queue_depth = 4");
        check(mgr.global().pool.quota_per_channel == 6, "global.pool.quota_per_channel = 6");
        check(mgr.global().media.port == 18500 && mgr.global().media.app == "live", "global.media 生效");
        check(mgr.global().pool.images.size() == 1 && mgr.global().pool.images[0].name == "nv12",
              "images 缺省时补默认 nv12（1080p）");
        check(mgr.find("ch1") != nullptr && mgr.find("nope") == nullptr, "find() 命中 / 未命中正确");
    }

    std::printf("=== B. 必须被拒绝，且错误信息带字段路径 ===\n");
    expect_reject(dir + "/node_v1_missing_type.json", "pipelines[0].nodes[0].type", "缺 type");
    expect_reject(dir + "/node_v1_dup_channel.json", "pipelines[1].channel", "channel 重复");
    expect_reject(dir + "/node_v1_port_conflict.json", "nodes[1].config.port", "端口冲突（节点值与 global 不一致）");
    expect_reject(dir + "/node_v1_fps_zero.json", "nodes[0].config.fps", "fps = 0");
    expect_reject(dir + "/node_v1_dup_node_name.json", "nodes[1].name", "同名节点导致队列名撞车");
    expect_reject(dir + "/node_v1_broken.json", "JSON 语法错误", "JSON 语法错误");
    expect_reject(dir + "/does_not_exist.json", "不存在", "文件不存在");

    std::printf("=== C. 拒绝后不破坏已有快照 ===\n");
    check(mgr.pipelines().size() == 2, "连续 7 次加载失败后，仍保留上一次成功的快照（2 条链路）");
    check(mgr.global().media.port == 18500, "global 仍是上次成功的值（18500）");

    std::printf("=== D. v0 适配（push.json -> 单链路）===\n");
    {
        check(mgr.load("config/push.json"), "v0 push.json 加载成功");
        check(mgr.is_v0(), "is_v0() = true（走了 v0 适配）");
        check(mgr.pipelines().size() == 1, "v0 合成 1 条链路");
        if (mgr.pipelines().size() == 1)
        {
            const evs::config::PipelineDesc &p = mgr.pipelines()[0];
            check(p.nodes.size() == 3, "vi + push + print 三个节点");
            check(p.nodes[0].type == "vi" && p.nodes[0].name == "vi", "首节点 vi（name=vi，与今天 main_node 一致）");
            check(p.nodes[1].type == "push" && p.nodes[1].name == "rtsp", "次节点 push（name=rtsp）");
            check(p.nodes[2].type == "print", "末节点 print");

            json viSeg, pushSeg;
            const bool okVi = GetSeg(p.nodes[0].config_json, "vi", viSeg);
            const bool okPush = GetSeg(p.nodes[1].config_json, "push", pushSeg);
            check(okVi && okPush, "两个 config_json 结构正确");
            if (okVi && okPush)
            {
                check(viSeg.value("width", 0) == pushSeg.value("width", 0) &&
                          viSeg.value("width", 0) > 0,
                      "vi 宽高来自 push 段（与今天行为一致）");
                check(pushSeg.value("port", 0) == mgr.global().media.port, "push 段端口 = global.media.port");
                check(pushSeg.value("app", std::string()) == mgr.global().media.app, "push 段 app = global.media.app");
            }
            check(mgr.global().pool.blocks == 50 && mgr.global().pool.queue_depth == 20,
                  "v0 池参数与现状代码默认一致（50 / 20）");
        }
    }

    std::printf("=== E. 仓库内真实配置 ===\n");
    {
        const bool real = mgr.load("config/node.json");
        check(real, "config/node.json 可加载（失败时看上面的拒绝原因）");
        if (real)
        {
            check(!mgr.pipelines().empty(), "至少 1 条链路");
            bool all_have_nodes = true;
            for (size_t i = 0; i < mgr.pipelines().size(); ++i)
            {
                if (mgr.pipelines()[i].nodes.empty())
                    all_have_nodes = false;
            }
            check(all_have_nodes, "每条链路都至少 1 个节点");
            std::printf("info : config/node.json -> %zu 条链路, blocks=%d, queue_depth=%d, "
                        "quota=%d, port=%d, images=%zu\n",
                        mgr.pipelines().size(), mgr.global().pool.blocks, mgr.global().pool.queue_depth,
                        mgr.global().pool.quota_per_channel, mgr.global().media.port,
                        mgr.global().pool.images.size());
        }
    }

    std::printf("=== F. load_with_fallback()（仓库根默认候选路径）===\n");
    {
        check(mgr.load_with_fallback(), "load_with_fallback() 无参调用成功");
        check(mgr.source_path() == "config/node.json", "node.json 与 push.json 同时存在时优先 node.json");
        check(!mgr.is_v0(), "走 v1 分支（未被 v0 抢走）");
        // 显式传路径时只认它（argv[1] 语义），不再回退
        check(mgr.load_with_fallback("config/push.json"), "load_with_fallback(显式 push.json) 成功");
        check(mgr.source_path() == "config/push.json" && mgr.is_v0(),
              "显式路径生效且只认它（参数被当作 v0 加载）");
        check(mgr.load_with_fallback("config/no_such_file.json") == false,
              "显式路径不存在时失败（不回退到默认候选）");
    }

    std::printf("==== %s (fails=%d) ====\n", (g_fail == 0) ? "ALL PASS" : "HAS FAILURES", g_fail);
    return (g_fail == 0) ? 0 : 1;
}
