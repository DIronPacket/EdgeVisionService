// T5 验收：NodeFactory（类型注册表）+ PipelineBuilder（拓扑校验 / 装配 / 连线）
//
// 用法：test_builder [node.json 路径]
//   默认 "config/node.json"；F 组用它验证 4 路真实配置能装配。
//
// 边界（重要）：本测试**只 build，不 start**。
//   build 只做 create()/set_data()/link_to()，不碰硬件、不起线程、不开 RTSP；
//   start_all() 会让 ViNode 去拉 /dev/video0、PushNode 去开 MK 流，属于端到端
//   行为，由 T6/T7 之后在板子上验（见 docs/history/TESTLOG.md §5.8 未做项）。
//
// 记录见 docs/history/TESTLOG.md §5.8（L2 · T5）。
#include "node_config_manager.h"
#include "node_factory.h"
#include "pipeline_builder.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h> // getpid()：临时配置文件名
#include <vector>

using evs::config::NodeConfigManager;
using evs::config::NodeDesc;
using evs::config::PipelineDesc;

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

static bool Contains(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

// ------------------------------------------------------------------
// 测试专用节点：用来观测 builder 的调用顺序与配置透传。
// 只注册成 "dummy"（不覆盖内置类型），放在链路中间位置。
// ------------------------------------------------------------------
static std::vector<std::string> g_dummy_log;
static std::string g_dummy_cfg;

class DummyNode : public PipelineNode
{
public:
    // 基类的 channel_id_ 未初始化（pipeline_node.h 里是裸 int），这里显式置 0：
    // 派生类不能在初始化列表里初始化基类成员，只能进函数体。
    DummyNode() { channel_id_ = 0; }
    void start() override { g_dummy_log.push_back(m_name + ":start"); }
    void stop() override { g_dummy_log.push_back(m_name + ":stop"); }
    void create(int channel_id, const std::string &name) override
    {
        channel_id_ = channel_id;
        m_name = name + std::to_string(channel_id);
        g_dummy_log.push_back(m_name + ":create");
    }
    void set_data(const char *config) override
    {
        g_dummy_cfg = config ? config : "";
        g_dummy_log.push_back(m_name + ":set_data");
    }
};

static std::shared_ptr<PipelineNode> CreateDummy() { return std::make_shared<DummyNode>(); }

// 造一个节点描述；cfg 为空表示该节点不写 config 段（builder 会跳过 set_data）
static NodeDesc MakeNode(const std::string &type, const std::string &name, int channel,
                         const std::string &path, const std::string &cfg = std::string())
{
    NodeDesc n;
    n.type = type;
    n.name = name;
    n.channel = channel;
    n.path = path;
    n.config_json = cfg;
    return n;
}

// 造一条链路描述；path 前缀 "pipelines[i]" 形式的字段路径
static PipelineDesc MakePipeline(const std::string &name, int channel,
                                 const std::string &path)
{
    PipelineDesc p;
    p.name = name;
    p.channel = channel;
    p.enable = true;
    p.path = path;
    return p;
}


// ------------------------------------------------------------------
// A. NodeFactory
// ------------------------------------------------------------------
static void TestFactory()
{
    std::printf("\n== A. NodeFactory: type -> 节点对象 ==\n");
    NodeFactory &f = NodeFactory::getInstance();

    check(f.has_type("vi") && f.has_type("vpss") && f.has_type("push") &&
              f.has_type("print"),
          "A1 内置 4 类节点（vi/vpss/push/print）均已注册");

    std::vector<std::string> ts = f.types();
    std::string joined;
    for (size_t i = 0; i < ts.size(); ++i)
        joined += (i ? "," : "") + ts[i];
    check(ts.size() >= 4 && Contains("," + joined + ",", ",push,"),
          "A2 types() 列出内置类型（报错提示与日志共用）");

    std::shared_ptr<PipelineNode> vi = f.create("vi");
    check(vi != nullptr, "A3 create(\"vi\") 返回 ViNode 对象");

    std::shared_ptr<PipelineNode> none = f.create("nonexistent");
    check(none == nullptr, "A4 未知 type -> 返回 nullptr（不抛异常、不退出进程）");
    check(Contains(f.last_error(), "nonexistent"),
          "A5 未知 type 的报错里带用户写的类型名（一眼看出拼错在哪）");
    check(Contains(f.last_error(), "已注册"),
          "A6 未知 type 的报错里列出已注册类型");

    // 重复注册：不覆盖，保证同名类型实现不随注册顺序漂移
    check(f.register_type("vi", &CreateDummy) == false,
          "A7 重复注册同一 type 返回 false（不覆盖已有实现）");
    std::shared_ptr<PipelineNode> still_vi = f.create("vi");
    check(still_vi && dynamic_cast<DummyNode *>(still_vi.get()) == nullptr,
          "A8 重复注册被拒后，create(\"vi\") 仍是内置 ViNode");

    check(f.register_type("", &CreateDummy) == false, "A9 空 type 注册被拒");
    check(f.register_type("bad_creator", nullptr) == false, "A10 空 creator 注册被拒");

    check(f.register_type("dummy", &CreateDummy) == true, "A11 可注册自定义类型（便于扩展）");
    check(f.has_type("dummy"), "A12 has_type 对自定义类型为 true");
    std::shared_ptr<PipelineNode> d = f.create("dummy");
    check(d && dynamic_cast<DummyNode *>(d.get()) != nullptr,
          "A13 create(\"dummy\") 返回自定义 DummyNode");
    check(f.last_error().empty(), "A14 创建成功后 last_error 清空");
}

// ------------------------------------------------------------------
// B. 拓扑规则（T5 验收的另一半：节点顺序错 / 无末端节点必须报错）
// ------------------------------------------------------------------
static void TestOrderRules()
{
    std::printf("\n== B. PipelineBuilder::check_order 拓扑规则 ==\n");
    std::string err;

    // R1：至少 2 个节点
    PipelineDesc p0 = MakePipeline("r1", 90, "pipelines[0]");
    check(PipelineBuilder::check_order(p0, err) == false && Contains(err, "没有任何节点"),
          "B1 空 nodes -> 报错「链路里没有任何节点」");

    PipelineDesc p1 = MakePipeline("r1b", 91, "pipelines[1]");
    p1.nodes.push_back(MakeNode("vi", "vi", 91, "pipelines[1].nodes[0]"));
    check(PipelineBuilder::check_order(p1, err) == false && Contains(err, "只有 1 个节点"),
          "B2 单节点链路 -> 报错「只有 1 个节点」");

    // R2：首个节点必须是源
    PipelineDesc p2 = MakePipeline("r2", 92, "pipelines[2]");
    p2.nodes.push_back(MakeNode("push", "rtsp", 92, "pipelines[2].nodes[0]"));
    p2.nodes.push_back(MakeNode("print", "print", 92, "pipelines[2].nodes[1]"));
    check(PipelineBuilder::check_order(p2, err) == false &&
              Contains(err, "首个节点必须是源节点") && Contains(err, "pipelines[2].nodes[0]"),
          "B3 首节点不是 vi -> 报错并带字段路径 pipelines[2].nodes[0]");

    // R3：末尾节点必须是终点（T5 明写的「无末端节点」）
    PipelineDesc p3 = MakePipeline("r3", 93, "pipelines[3]");
    p3.nodes.push_back(MakeNode("vi", "vi", 93, "pipelines[3].nodes[0]"));
    p3.nodes.push_back(MakeNode("vpss", "vpss", 93, "pipelines[3].nodes[1]"));
    check(PipelineBuilder::check_order(p3, err) == false &&
              Contains(err, "链路没有末端节点") && Contains(err, "pipelines[3].nodes[1]"),
          "B4 末节点不是 push/print -> 报错「链路没有末端节点」并带字段路径");

    // R4：vi 只能首位
    PipelineDesc p4 = MakePipeline("r4", 94, "pipelines[4]");
    p4.nodes.push_back(MakeNode("vi", "vi", 94, "pipelines[4].nodes[0]"));
    p4.nodes.push_back(MakeNode("vi", "vi2", 94, "pipelines[4].nodes[1]"));
    p4.nodes.push_back(MakeNode("push", "rtsp", 94, "pipelines[4].nodes[2]"));
    check(PipelineBuilder::check_order(p4, err) == false && Contains(err, "只能出现在首位"),
          "B5 vi 出现在非首位 -> 报错");

    // R5：print 只能末位
    PipelineDesc p5 = MakePipeline("r5", 95, "pipelines[5]");
    p5.nodes.push_back(MakeNode("vi", "vi", 95, "pipelines[5].nodes[0]"));
    p5.nodes.push_back(MakeNode("print", "print", 95, "pipelines[5].nodes[1]"));
    p5.nodes.push_back(MakeNode("push", "rtsp", 95, "pipelines[5].nodes[2]"));
    check(PipelineBuilder::check_order(p5, err) == false && Contains(err, "只能出现在末位"),
          "B6 print 出现在中间 -> 报错（它是终点，否则块不会归还内存池）");

    // 合法链：vi -> vpss -> push -> print（与 config/node.json 的 ch0 一致）
    PipelineDesc p6 = MakePipeline("ok", 96, "pipelines[6]");
    p6.nodes.push_back(MakeNode("vi", "vi", 96, "pipelines[6].nodes[0]"));
    p6.nodes.push_back(MakeNode("vpss", "vpss", 96, "pipelines[6].nodes[1]"));
    p6.nodes.push_back(MakeNode("push", "rtsp", 96, "pipelines[6].nodes[2]"));
    p6.nodes.push_back(MakeNode("print", "print", 96, "pipelines[6].nodes[3]"));
    err.clear();
    check(PipelineBuilder::check_order(p6, err) == true && err.empty(),
          "B7 vi -> vpss -> push -> print 顺序合法");

    // 只有 push 结尾、没有 print 也合法（config/node.json 的 ch1~ch3 就是这样）
    PipelineDesc p7 = MakePipeline("ok2", 97, "pipelines[7]");
    p7.nodes.push_back(MakeNode("vi", "vi", 97, "pipelines[7].nodes[0]"));
    p7.nodes.push_back(MakeNode("push", "rtsp", 97, "pipelines[7].nodes[1]"));
    check(PipelineBuilder::check_order(p7, err) == true,
          "B8 vi -> push（无 print）也合法：push 本身就是终点");

    check(PipelineBuilder::is_source_type("vi") && !PipelineBuilder::is_source_type("push"),
          "B9 is_source_type 只认 vi");
    check(PipelineBuilder::is_sink_type("push") && PipelineBuilder::is_sink_type("print") &&
              !PipelineBuilder::is_sink_type("vi"),
          "B10 is_sink_type 认 push / print");
}


// ------------------------------------------------------------------
// C. build_one：装配结果（对象 / 名称 / 连线）
// ------------------------------------------------------------------
static void TestBuildOne()
{
    std::printf("\n== C. build_one: 装配结果 ==\n");
    NodeConfigManager &cfg = NodeConfigManager::getInstance();
    PipelineBuilder builder(cfg);

    PipelineDesc d = MakePipeline("chx", 5, "pipelines[9]");
    d.nodes.push_back(MakeNode("vi", "vi", 5, "pipelines[9].nodes[0]",
                               "{\"vi\":{\"src\":\"dev:/dev/video0\",\"width\":640,"
                               "\"height\":480,\"fps\":30}}"));
    d.nodes.push_back(MakeNode("vpss", "vpss", 5, "pipelines[9].nodes[1]",
                               "{\"vpss\":{\"enable\":false}}"));
    d.nodes.push_back(MakeNode("push", "rtsp", 5, "pipelines[9].nodes[2]",
                               "{\"push\":{\"enable\":false,\"width\":640,"
                               "\"height\":480,\"fps\":30,\"stream\":\"chx\"}}"));
    d.nodes.push_back(MakeNode("print", "print", 5, "pipelines[9].nodes[3]",
                               "{\"print\":{\"enable\":false}}"));

    PipelineSet set;
    check(builder.build_one(d, set) == true, "C1 build_one 成功（vi->vpss->push->print）");
    check(set.nodes.size() == 4, "C2 链路节点数 = 4");
    check(set.name == "chx" && set.channel == 5, "C3 PipelineSet 带上 name/channel");
    check(set.nodes[0]->get_full_name() == "vi5" &&
              set.nodes[2]->get_full_name() == "rtsp5",
          "C4 节点全名 = name + channel（队列名同源，T2 已校验唯一）");
    check(set.nodes[0]->get_channel_id() == 5, "C5 节点 channel 由 builder 灌下去");
    check(set.nodes[0]->next_nodes_.size() == 1 &&
              set.nodes[0]->next_nodes_.front() == set.nodes[1].get(),
          "C6 连线 0->1 建立");
    check(set.nodes[1]->parent_nodes_.size() == 1 &&
              set.nodes[1]->parent_nodes_.front() == set.nodes[0].get(),
          "C7 反向链接同步建立（push_input_pipe 靠它判来源）");
    check(set.nodes[3]->next_nodes_.empty(), "C8 链尾没有下游");
    check(Contains(set.describe(), "chx[5]") && Contains(set.describe(), " -> "),
          "C9 describe() 能打印整条链（日志定位用）");

    // 未知 type：build 阶段必须报错（T5 验收项），且不留半条链
    PipelineDesc bad = MakePipeline("bad", 6, "pipelines[10]");
    bad.nodes.push_back(MakeNode("vi", "vi", 6, "pipelines[10].nodes[0]"));
    bad.nodes.push_back(MakeNode("vpsss", "x", 6, "pipelines[10].nodes[1]"));
    bad.nodes.push_back(MakeNode("push", "rtsp", 6, "pipelines[10].nodes[2]"));

    PipelineSet bad_set;
    check(builder.build_one(bad, bad_set) == false, "C10 未知 type -> build_one 失败");
    check(Contains(builder.last_error(), "pipelines[10].nodes[1].type"),
          "C11 未知 type 的错误带字段路径 pipelines[10].nodes[1].type");
    check(Contains(builder.last_error(), "未知节点类型"),
          "C12 错误说明是「未知节点类型」");
    check(bad_set.nodes.empty(), "C13 失败后不留半条链（已建节点全部释放并 unlink）");
}

// ------------------------------------------------------------------
// D. build_one：create -> set_data 顺序 + 配置原文透传
// ------------------------------------------------------------------
static void TestCallOrder()
{
    std::printf("\n== D. build_one: 调用顺序与配置透传 ==\n");
    NodeConfigManager &cfg = NodeConfigManager::getInstance();
    PipelineBuilder builder(cfg);

    g_dummy_log.clear();
    g_dummy_cfg.clear();

    const std::string want = "{\"dummy\":{\"k\":1}}";
    PipelineDesc d = MakePipeline("ord", 7, "pipelines[11]");
    d.nodes.push_back(MakeNode("vi", "vi", 7, "pipelines[11].nodes[0]",
                               "{\"vi\":{\"src\":\"dev:/dev/video0\"}}"));
    d.nodes.push_back(MakeNode("dummy", "mid", 7, "pipelines[11].nodes[1]", want));
    d.nodes.push_back(MakeNode("push", "rtsp", 7, "pipelines[11].nodes[2]",
                               "{\"push\":{\"enable\":false}}"));

    PipelineSet set;
    check(builder.build_one(d, set) == true, "D1 中间放自定义类型（dummy）也能装配");
    check(g_dummy_log.size() == 2, "D2 dummy 被调 create + set_data 各一次（build 不 start）");
    if (g_dummy_log.size() == 2)
    {
        check(g_dummy_log[0] == "mid7:create", "D3 create() 先执行");
        check(g_dummy_log[1] == "mid7:set_data", "D4 set_data() 在 create() 之后");
    }
    check(g_dummy_cfg == want,
          "D5 set_data 收到配置原文（T2 已包成 {\"<type>\":{...}}，节点侧不用改）");
    check(set.nodes.size() == 3 && set.nodes[1]->get_full_name() == "mid7",
          "D6 dummy 全名 = mid + channel");
}


// ------------------------------------------------------------------
// E. build_all：真实 config/node.json（4 路）
// ------------------------------------------------------------------
static void TestBuildAllReal(const std::string &path, std::vector<PipelineSet> &sets)
{
    std::printf("\n== E. build_all: 真实配置 %s ==\n", path.c_str());
    NodeConfigManager &cfg = NodeConfigManager::getInstance();
    if (!cfg.load(path))
    {
        bad("E1 加载配置失败: " + cfg.last_error());
        return;
    }
    ok("E1 配置加载成功（NodeConfigManager，T2 产物）");

    const std::vector<PipelineDesc> &ps = cfg.pipelines();
    check(ps.size() == 4, "E2 配置里 4 条链路（ch0 + ch1~ch3）");

    PipelineBuilder builder(cfg);
    check(builder.build_all(sets) == true, "E3 build_all 成功");
    check(builder.built_count() == 4 && builder.failed_count() == 0 &&
              builder.skipped_count() == 0,
          "E4 4 条链路全部建成，失败 0、跳过 0");
    check(sets.size() == 4, "E5 out 里拿到 4 条链路");
    check(sets.size() == ps.size() && !sets.empty() && sets[0].name == ps[0].name,
          "E6 链路顺序与配置一致（sets[i] 对应 pipelines[i]）");

    bool same_cnt = sets.size() == ps.size();
    for (size_t i = 0; same_cnt && i < sets.size(); ++i)
        same_cnt = sets[i].nodes.size() == ps[i].nodes.size();
    check(same_cnt, "E7 每条链的节点数与配置一致");

    bool names_ok = sets.size() == ps.size();
    for (size_t i = 0; names_ok && i < sets.size(); ++i)
    {
        for (size_t j = 0; j < sets[i].nodes.size(); ++j)
        {
            const std::string want =
                ps[i].nodes[j].name + std::to_string(ps[i].nodes[j].channel);
            if (!sets[i].nodes[j] || sets[i].nodes[j]->get_full_name() != want)
            {
                names_ok = false;
                break;
            }
        }
    }
    check(names_ok, "E8 每个节点全名 = 配置 name + channel");

    // 连线：每条链 n 个节点 → n-1 条边
    size_t edges = 0, want_edges = 0;
    for (size_t i = 0; i < sets.size(); ++i)
    {
        for (size_t j = 0; j < sets[i].nodes.size(); ++j)
            edges += sets[i].nodes[j]->next_nodes_.size();
        if (!sets[i].nodes.empty())
            want_edges += sets[i].nodes.size() - 1;
    }
    check(edges == want_edges, "E9 连线数 = Σ(节点数 - 1)");

    bool shape_ok = sets.size() == 4 && sets[0].nodes.size() == 4 &&
                    sets[1].nodes.size() == 3 && sets[2].nodes.size() == 3 &&
                    sets[3].nodes.size() == 3;
    check(shape_ok, "E10 ch0 是 4 节点（含 vpss），ch1~ch3 各 3 节点（vi->push->print）");

    for (size_t i = 0; i < sets.size(); ++i)
        std::printf("       %s\n", sets[i].describe().c_str());
}

// ------------------------------------------------------------------
// F. build_all：单链失败不影响其他链（N3）
// ------------------------------------------------------------------
static bool WriteText(const std::string &path, const std::string &text)
{
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << text;
    return ofs.good();
}

static void TestPartialFailure()
{
    std::printf("\n== F. build_all: 单链失败不影响其他链（N3）==\n");
    const std::string tmp = std::string("/tmp/t5_partial_") + std::to_string(::getpid()) + ".json";
    const std::string text =
        "{ \"version\": 1,"
        "  \"global\": { \"log\": { \"level\": \"info\", \"file\": \"\" },"
        "    \"pool\": { \"blocks\": 8, \"queue_depth\": 4, \"quota_per_channel\": 3,"
        "      \"images\": [ { \"name\": \"nv12\", \"width\": 640, \"height\": 480, \"fmt\": \"nv12\" } ] },"
        "    \"media\": { \"rtsp\": { \"port\": 8550, \"bind_ip\": \"0.0.0.0\", \"app\": \"live\" } } },"
        "  \"pipelines\": ["
        "    { \"name\": \"good\", \"channel\": 20, \"enable\": true, \"nodes\": ["
        "        { \"type\": \"vi\",   \"name\": \"vi\",   \"config\": { \"src\": \"dev:/dev/video0\" } },"
        "        { \"type\": \"push\", \"name\": \"rtsp\", \"config\": { \"enable\": false } } ] },"
        "    { \"name\": \"bad\", \"channel\": 21, \"enable\": true, \"nodes\": ["
        "        { \"type\": \"vi\",    \"name\": \"vi\",   \"config\": { \"src\": \"dev:/dev/video0\" } },"
        "        { \"type\": \"vpsss\", \"name\": \"x\",    \"config\": { } },"
        "        { \"type\": \"push\",  \"name\": \"rtsp\", \"config\": { \"enable\": false } } ] },"
        "    { \"name\": \"norear\", \"channel\": 22, \"enable\": true, \"nodes\": ["
        "        { \"type\": \"vi\",   \"name\": \"vi\",   \"config\": { \"src\": \"dev:/dev/video0\" } },"
        "        { \"type\": \"vpss\", \"name\": \"vpss\", \"config\": { \"enable\": false } } ] } ] }";

    check(WriteText(tmp, text), "F1 写出临时配置（1 条合法 + 1 条未知 type + 1 条无末端节点）");

    NodeConfigManager &cfg = NodeConfigManager::getInstance();
    if (!cfg.load(tmp))
    {
        bad("F2 加载临时配置失败: " + cfg.last_error());
        std::remove(tmp.c_str());
        return;
    }
    ok("F2 T2 接受未知 type（合法性由 T5 的 NodeFactory 判定，职责边界正确）");

    PipelineBuilder builder(cfg);
    std::vector<PipelineSet> sets;
    check(builder.build_all(sets) == true, "F3 build_all 返回 true（至少一条链建成）");
    check(builder.built_count() == 1 && builder.failed_count() == 2 &&
              builder.skipped_count() == 0,
          "F4 计数正确：建成 1 / 失败 2 / 跳过 0");
    check(sets.size() == 1 && sets[0].name == "good", "F5 失败的两条链没有污染 out");
    check(builder.errors().size() == 2, "F6 两条失败各留一条错误");
    check(builder.errors().size() == 2 && Contains(builder.errors()[0], "pipelines[1].nodes[1].type") &&
              Contains(builder.errors()[0], "未知节点类型"),
          "F7 未知 type 的错误带字段路径 pipelines[1].nodes[1].type");
    check(builder.errors().size() == 2 && Contains(builder.errors()[1], "链路没有末端节点"),
          "F8 无末端节点的错误在 build_all 里同样被拦下");

    sets.clear();
    std::remove(tmp.c_str());
}

// ------------------------------------------------------------------
// G. start_all / stop_all 边界
// ------------------------------------------------------------------
static void TestStartStopBoundary(std::vector<PipelineSet> &sets)
{
    std::printf("\n== G. start_all / stop_all ==\n");
    // 未 start 的节点 stop() 里 running_ 为 false，直接返回，可以安全验证
    PipelineBuilder::stop_all(sets);
    check(true, "G1 stop_all 对未启动的链路是空操作（不崩、不阻塞）");
    skip("G2 start_all 的启停顺序（终点先起 / 源头先停）-> 会真拉 /dev/video0 并开 RTSP，属端到端，T6/T7 后在板端验");
    skip("G3 Ctrl+C 后 stop_all -> MediaServer::shutdown -> FreeMem 的退出顺序 -> T6 验收项");
}

// ------------------------------------------------------------------
// H. 反复 build / 释放
// ------------------------------------------------------------------
static void TestRepeatBuild(const std::string &path)
{
    std::printf("\n== H. 反复 build / 释放 ==\n");
    NodeConfigManager &cfg = NodeConfigManager::getInstance();
    if (!cfg.load(path))
    {
        bad("H1 重新加载配置失败: " + cfg.last_error());
        return;
    }

    PipelineBuilder b1(cfg);
    std::vector<PipelineSet> s1;
    const bool r1 = b1.build_all(s1);
    PipelineBuilder b2(cfg);
    std::vector<PipelineSet> s2;
    // 同名队列第二次 registerQueue 会返回 false（只记日志），属于已知行为：
    // 进程里同一条链路只该装配一次，这里只证明"再来一次不会崩"。
    const bool r2 = b2.build_all(s2);
    check(r1 && r2 && s1.size() == 4 && s2.size() == 4,
          "H1 同一配置连续 build 两次都成功（同名队列注册失败只记日志）");
    s1.clear();
    s2.clear();
    check(true, "H2 链路释放（shared_ptr 析构 -> stop() 空操作）不崩");
}

int main(int argc, char **argv)
{
    const std::string path = (argc > 1 && argv[1][0]) ? argv[1] : "config/node.json";
    std::printf("T5 验收：NodeFactory + PipelineBuilder（只 build，不 start）\n配置: %s\n",
                path.c_str());

    TestFactory();
    TestOrderRules();
    TestBuildOne();
    TestCallOrder();

    std::vector<PipelineSet> real_sets;
    TestBuildAllReal(path, real_sets);
    TestPartialFailure();
    TestStartStopBoundary(real_sets);
    TestRepeatBuild(path);

    std::printf("\n==== T5 builder 自检结果: passes=%d fails=%d skips=%d ====\n",
                g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}

