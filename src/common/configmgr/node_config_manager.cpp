#include "node_config_manager.h"

#include <json.hpp>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace evs
{
namespace config
{

// 快照：一次 load 产出的不可变结果。整体替换，避免读到半成品。
struct NodeConfigManager::Snapshot
{
    GlobalDesc global;
    std::vector<PipelineDesc> pipelines;
    std::string source;
    bool from_v0 = false;
};

namespace
{

// ---------- 小工具 ----------

bool ReadWholeFile(const std::string &path, std::string &out)
{
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open())
        return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return !out.empty();
}

std::string Int2Str(long long v)
{
    std::ostringstream oss;
    oss << v;
    return oss.str();
}

// 取整数并做范围校验；字段缺失视为"不校验"，返回 true。
bool GetIntInRange(const json &obj, const char *key, const std::string &where,
                   long long lo, long long hi, int &out, std::string &err)
{
    if (!obj.is_object() || !obj.contains(key))
        return true;
    const json &v = obj[key];
    if (!v.is_number_integer())
    {
        err = where + ": 必须是整数（实际类型不是 integer）";
        return false;
    }
    const long long n = v.get<long long>();
    if (n < lo || n > hi)
    {
        err = where + ": 超出允许范围 [" + Int2Str(lo) + ", " + Int2Str(hi) + "]，实际 " + Int2Str(n);
        return false;
    }
    out = static_cast<int>(n);
    return true;
}

// 把节点的 config 包成 {"<type>": {...}}，这样各节点 set_data() 一行都不用改
// （它们本来就在找自己的段，见 DESIGN §5.4 末段）。
std::string WrapAsSegment(const std::string &type, const json &cfg)
{
    json wrapper = json::object();
    wrapper[type] = cfg;
    return wrapper.dump();
}

// 在 root 上按 "a.b.c" 取子节点；取不到返回 false
bool FollowPath(const json &root, const std::string &path, json &out)
{
    json cur = root;
    size_t begin = 0;
    while (begin <= path.size())
    {
        size_t dot = path.find('.', begin);
        const std::string seg = path.substr(begin, (dot == std::string::npos) ? std::string::npos : dot - begin);
        if (seg.empty())
            return false;
        if (!cur.is_object() || !cur.contains(seg))
            return false;
        cur = cur[seg];
        if (dot == std::string::npos)
            break;
        begin = dot + 1;
    }
    out = cur;
    return true;
}

// 递归展开 ${global.xxx} 引用（DESIGN §5.4「引用语法」：解析不出来就报错，不静默）。
// 整个值就是单个引用时保留原类型（数字仍是数字），否则按字符串拼接。
bool ExpandRefs(json &value, const json &root, const std::string &where, std::string &err)
{
    if (value.is_array())
    {
        for (size_t i = 0; i < value.size(); ++i)
        {
            json child = value[i];
            if (!ExpandRefs(child, root, where + "[" + Int2Str(static_cast<long long>(i)) + "]", err))
                return false;
            value[i] = child;
        }
        return true;
    }
    if (value.is_object())
    {
        for (json::iterator it = value.begin(); it != value.end(); ++it)
        {
            if (!ExpandRefs(it.value(), root, where + "." + it.key(), err))
                return false;
        }
        return true;
    }
    if (!value.is_string())
        return true;

    const std::string src = value.get<std::string>();
    // 整体就是一个引用：保留目标值类型（int 仍是 int，节点用 value<int>() 才取得到）
    if (src.size() > 4 && src.compare(0, 2, "${") == 0 && src[src.size() - 1] == '}' &&
        src.find("${", 2) == std::string::npos && src.find('}') == src.size() - 1)
    {
        const std::string ref = src.substr(2, src.size() - 3);
        json target;
        if (!FollowPath(root, ref, target))
        {
            err = where + ": 无法解析引用 ${" + ref + "}（路径不存在）";
            return false;
        }
        value = target;
        return true;
    }

    // 行内引用：按字符串替换
    std::string text = src;
    size_t pos = 0;
    while ((pos = text.find("${", pos)) != std::string::npos)
    {
        const size_t end = text.find('}', pos + 2);
        if (end == std::string::npos)
        {
            err = where + ": 引用语法缺少 '}'：" + src;
            return false;
        }
        const std::string ref = text.substr(pos + 2, end - pos - 2);
        json target;
        if (!FollowPath(root, ref, target))
        {
            err = where + ": 无法解析引用 ${" + ref + "}（路径不存在）";
            return false;
        }
        const std::string repl = target.is_string() ? target.get<std::string>() : target.dump();
        text.replace(pos, end - pos + 1, repl);
        pos += repl.size();
    }
    value = text;
    return true;
}

// ---------- global 段 ----------

bool ParseGlobal(const json &g, GlobalDesc &out, std::string &err)
{
    if (!g.is_object())
    {
        err = "global: 必须是对象";
        return false;
    }

    if (g.contains("log"))
    {
        const json &l = g["log"];
        if (!l.is_object())
        {
            err = "global.log: 必须是对象";
            return false;
        }
        if (l.contains("level"))
        {
            if (!l["level"].is_string())
            {
                err = "global.log.level: 必须是字符串";
                return false;
            }
            out.log.level = l["level"].get<std::string>();
        }
        if (l.contains("file"))
        {
            if (!l["file"].is_string())
            {
                err = "global.log.file: 必须是字符串";
                return false;
            }
            out.log.file = l["file"].get<std::string>();
        }
    }

    if (g.contains("pool"))
    {
        const json &p = g["pool"];
        if (!p.is_object())
        {
            err = "global.pool: 必须是对象";
            return false;
        }
        if (!GetIntInRange(p, "blocks", "global.pool.blocks", 1, 4096, out.pool.blocks, err))
            return false;
        if (!GetIntInRange(p, "queue_depth", "global.pool.queue_depth", 1, 1024, out.pool.queue_depth, err))
            return false;
        if (!GetIntInRange(p, "quota_per_channel", "global.pool.quota_per_channel", 0, 4096,
                           out.pool.quota_per_channel, err))
            return false;

        if (p.contains("images"))
        {
            if (!p["images"].is_array())
            {
                err = "global.pool.images: 必须是数组";
                return false;
            }
            std::set<std::string> names;
            for (size_t i = 0; i < p["images"].size(); ++i)
            {
                const json &im = p["images"][i];
                const std::string w = "global.pool.images[" + Int2Str(static_cast<long long>(i)) + "]";
                if (!im.is_object())
                {
                    err = w + ": 必须是对象";
                    return false;
                }
                if (!im.contains("name") || !im["name"].is_string() || im["name"].get<std::string>().empty())
                {
                    err = w + ".name: 缺失或为空";
                    return false;
                }
                ImgSpec spec;
                spec.name = im["name"].get<std::string>();
                if (!names.insert(spec.name).second)
                {
                    err = w + ".name: 重复：" + spec.name;
                    return false;
                }
                if (!im.contains("width") || !im.contains("height"))
                {
                    err = w + ": width / height 必填";
                    return false;
                }
                if (!GetIntInRange(im, "width", w + ".width", 16, 8192, spec.width, err))
                    return false;
                if (!GetIntInRange(im, "height", w + ".height", 16, 8192, spec.height, err))
                    return false;
                if (im.contains("fmt"))
                {
                    if (!im["fmt"].is_string())
                    {
                        err = w + ".fmt: 必须是字符串";
                        return false;
                    }
                    spec.fmt = im["fmt"].get<std::string>();
                }
                if (spec.fmt.empty())
                    spec.fmt = spec.name; // 常见写法：name 本身就是格式名（nv12）
                out.pool.images.push_back(spec);
            }
        }
    }

    if (g.contains("media"))
    {
        const json &m = g["media"];
        if (!m.is_object())
        {
            err = "global.media: 必须是对象";
            return false;
        }
        if (m.contains("rtsp"))
        {
            const json &r = m["rtsp"];
            if (!r.is_object())
            {
                err = "global.media.rtsp: 必须是对象";
                return false;
            }
            if (!GetIntInRange(r, "port", "global.media.rtsp.port", 1, 65535, out.media.port, err))
                return false;
            if (r.contains("bind_ip"))
            {
                if (!r["bind_ip"].is_string())
                {
                    err = "global.media.rtsp.bind_ip: 必须是字符串";
                    return false;
                }
                out.media.bind_ip = r["bind_ip"].get<std::string>();
            }
            if (r.contains("app"))
            {
                if (!r["app"].is_string())
                {
                    err = "global.media.rtsp.app: 必须是字符串";
                    return false;
                }
                out.media.app = r["app"].get<std::string>();
            }
            if (out.media.app.empty())
            {
                err = "global.media.rtsp.app: 不能为空";
                return false;
            }
        }
    }

    // images 缺省时补一条 1080p nv12（与现状内存池的硬编码一致）
    if (out.pool.images.empty())
    {
        ImgSpec def;
        def.name = "nv12";
        def.width = 1920;
        def.height = 1080;
        def.fmt = "nv12";
        out.pool.images.push_back(def);
    }
    return true;
}

// 节点 config 内数值字段的合理性校验（fps<=0 / 离谱值必须拒绝，见 DESIGN §11.3 反例）。
// where 是报错前缀（v1: "pipelines[0].nodes[2].config"，v0: "push"）。
bool CheckNodeNumbers(const json &cfg, const std::string &where, std::string &err)
{
    int dummy = 0;
    if (!GetIntInRange(cfg, "fps", where + ".fps", 1, 240, dummy, err))
        return false;
    if (!GetIntInRange(cfg, "width", where + ".width", 16, 8192, dummy, err))
        return false;
    if (!GetIntInRange(cfg, "height", where + ".height", 16, 8192, dummy, err))
        return false;
    if (!GetIntInRange(cfg, "bitrate_kbps", where + ".bitrate_kbps", 16, 200000, dummy, err))
        return false;
    if (!GetIntInRange(cfg, "interval", where + ".interval", 1, 100000, dummy, err))
        return false;
    if (!GetIntInRange(cfg, "gop", where + ".gop", 1, 10000, dummy, err))
        return false;

    // 宽高必须是偶数（4:2:0 编码要求；push.json 里也是这么写的）
    static const char *kEvenKeys[] = {"width", "height"};
    for (size_t i = 0; i < sizeof(kEvenKeys) / sizeof(kEvenKeys[0]); ++i)
    {
        const char *k = kEvenKeys[i];
        if (!cfg.contains(k) || !cfg[k].is_number_integer())
            continue;
        if (cfg[k].get<int>() % 2 != 0)
        {
            err = where + "." + k + ": 必须是偶数（4:2:0 编码要求），实际 " + Int2Str(cfg[k].get<int>());
            return false;
        }
    }
    return true;
}

// ---------- v1：global + pipelines[] ----------

bool ParseV1(const json &root, GlobalDesc &global, std::vector<PipelineDesc> &out, std::string &err)
{
    if (root.contains("version"))
    {
        if (!root["version"].is_number_integer())
        {
            err = "version: 必须是整数";
            return false;
        }
        if (root["version"].get<int>() != 1)
        {
            err = "version: 只支持 1，实际 " + Int2Str(root["version"].get<int>());
            return false;
        }
    }

    if (!root.contains("global"))
    {
        err = "global: 缺失（v1 必须提供 global 段）";
        return false;
    }
    if (!ParseGlobal(root["global"], global, err))
        return false;

    if (!root.contains("pipelines"))
    {
        err = "pipelines: 缺失";
        return false;
    }
    if (!root["pipelines"].is_array())
    {
        err = "pipelines: 必须是数组";
        return false;
    }
    if (root["pipelines"].empty())
    {
        err = "pipelines: 至少要有一条链路";
        return false;
    }

    std::set<std::string> pipe_names;
    std::set<int> channels;
    std::set<std::string> queue_names; // name + channel 必须全局唯一（N4）

    for (size_t i = 0; i < root["pipelines"].size(); ++i)
    {
        const json &pj = root["pipelines"][i];
        const std::string where = "pipelines[" + Int2Str(static_cast<long long>(i)) + "]";
        if (!pj.is_object())
        {
            err = where + ": 必须是对象";
            return false;
        }

        PipelineDesc pd;
        pd.path = where;

        if (pj.contains("name"))
        {
            if (!pj["name"].is_string())
            {
                err = where + ".name: 必须是字符串";
                return false;
            }
            pd.name = pj["name"].get<std::string>();
        }
        if (pd.name.empty())
            pd.name = "ch" + Int2Str(static_cast<long long>(i));
        if (!pipe_names.insert(pd.name).second)
        {
            err = where + ".name: 重复：" + pd.name;
            return false;
        }

        // channel 缺省 = 数组下标；多路必须不同（否则队列名 / 日志前缀撞车）
        pd.channel = static_cast<int>(i);
        if (pj.contains("channel"))
        {
            if (!GetIntInRange(pj, "channel", where + ".channel", 0, 1023, pd.channel, err))
                return false;
        }
        if (!channels.insert(pd.channel).second)
        {
            err = where + ".channel: 重复：" + Int2Str(pd.channel);
            return false;
        }

        if (pj.contains("enable"))
        {
            if (!pj["enable"].is_boolean())
            {
                err = where + ".enable: 必须是 true / false";
                return false;
            }
            pd.enable = pj["enable"].get<bool>();
        }

        if (!pj.contains("nodes"))
        {
            err = where + ".nodes: 缺失";
            return false;
        }
        if (!pj["nodes"].is_array())
        {
            err = where + ".nodes: 必须是数组";
            return false;
        }
        if (pj["nodes"].empty())
        {
            err = where + ".nodes: 至少要有一个节点";
            return false;
        }

        for (size_t k = 0; k < pj["nodes"].size(); ++k)
        {
            const json &nj = pj["nodes"][k];
            const std::string nw = where + ".nodes[" + Int2Str(static_cast<long long>(k)) + "]";
            if (!nj.is_object())
            {
                err = nw + ": 必须是对象";
                return false;
            }

            NodeDesc nd;
            nd.path = nw;
            nd.channel = pd.channel;

            // type 必填（"未知 type" 是否合法由 T5 的 NodeFactory 判定，这里只管非空）
            if (!nj.contains("type"))
            {
                err = nw + ".type: 缺失（必填：vi / vpss / push / print）";
                return false;
            }
            if (!nj["type"].is_string())
            {
                err = nw + ".type: 必须是字符串";
                return false;
            }
            nd.type = nj["type"].get<std::string>();
            if (nd.type.empty())
            {
                err = nw + ".type: 不能为空";
                return false;
            }

            // name 缺省 = type
            nd.name = nd.type;
            if (nj.contains("name"))
            {
                if (!nj["name"].is_string())
                {
                    err = nw + ".name: 必须是字符串";
                    return false;
                }
                const std::string n = nj["name"].get<std::string>();
                if (!n.empty())
                    nd.name = n;
            }
            const std::string qname = nd.name + Int2Str(nd.channel);
            if (!queue_names.insert(qname).second)
            {
                err = nw + ".name: 队列名重复 \"" + qname + "\"（队列名 = name + channel，必须全局唯一）";
                return false;
            }

            // config 段可省略
            json cfg = json::object();
            if (nj.contains("config"))
            {
                if (!nj["config"].is_object())
                {
                    err = nw + ".config: 必须是对象";
                    return false;
                }
                cfg = nj["config"];
            }

            if (!ExpandRefs(cfg, root, nw + ".config", err))
                return false;

            // 端口只允许在 global 配（D3）：节点写了且不一致 → 拒绝
            if (cfg.contains("port"))
            {
                int port = global.media.port;
                if (!GetIntInRange(cfg, "port", nw + ".config.port", 1, 65535, port, err))
                    return false;
                if (port != global.media.port)
                {
                    err = nw + ".config.port: " + Int2Str(port) + " 与 global.media.rtsp.port " +
                          Int2Str(global.media.port) +
                          " 冲突（端口只允许在 global.media.rtsp 配置，见 D3）";
                    return false;
                }
                cfg.erase("port"); // 与 global 一致 → 忽略，改用下面注入的值
            }

            if (!CheckNodeNumbers(cfg, nw + ".config", err))
                return false;

            // global 默认值注入：推送端缺 port / app 时用 global.media.rtsp
            if (nd.type == "push")
            {
                if (!cfg.contains("port"))
                    cfg["port"] = global.media.port;
                if (!cfg.contains("app"))
                    cfg["app"] = global.media.app;
            }

            nd.config_json = WrapAsSegment(nd.type, cfg);
            pd.nodes.push_back(nd);
        }
        out.push_back(pd);
    }
    return true;
}

// ---------- v0：push.json 合成单链路（DESIGN §5.6） ----------

bool ParseV0(const json &root, GlobalDesc &global, std::vector<PipelineDesc> &out, std::string &err)
{
    // 池参数保持现状默认值（MemInit(size=50) / InitQueue(name, 20)），v0 没有配额概念
    global.pool.blocks = 50;
    global.pool.queue_depth = 20;
    global.pool.quota_per_channel = 0;
    {
        ImgSpec def;
        def.name = "nv12";
        def.width = 1920;
        def.height = 1080;
        def.fmt = "nv12";
        global.pool.images.push_back(def);
    }

    if (!root.contains("push") || !root["push"].is_object())
    {
        err = "push: 缺失或不是对象（v0 必须有 push 段）";
        return false;
    }
    const json &push = root["push"];

    if (!GetIntInRange(push, "port", "push.port", 1, 65535, global.media.port, err))
        return false;
    if (push.contains("app"))
    {
        if (!push["app"].is_string())
        {
            err = "push.app: 必须是字符串";
            return false;
        }
        global.media.app = push["app"].get<std::string>();
    }
    if (global.media.app.empty())
    {
        err = "push.app: 不能为空";
        return false;
    }
    if (!CheckNodeNumbers(push, "push", err))
        return false;

    PipelineDesc pd;
    pd.path = "pipelines[0]";
    pd.name = push.value("stream", std::string("ch0"));
    pd.channel = 0;
    pd.enable = true;

    // vi 段：从 push 段取宽高与帧率（与今天 main_node.cpp 的行为一致）
    json vi = json::object();
    if (push.contains("width"))
        vi["width"] = push["width"];
    if (push.contains("height"))
        vi["height"] = push["height"];
    if (push.contains("fps"))
        vi["fps"] = push["fps"];

    NodeDesc n_vi;
    n_vi.type = "vi";
    n_vi.name = "vi";
    n_vi.channel = 0;
    n_vi.path = "pipelines[0].nodes[0]";
    n_vi.config_json = WrapAsSegment("vi", vi);

    NodeDesc n_push;
    n_push.type = "push";
    n_push.name = "rtsp";
    n_push.channel = 0;
    n_push.path = "pipelines[0].nodes[1]";
    n_push.config_json = WrapAsSegment("push", push);

    pd.nodes.push_back(n_vi);
    pd.nodes.push_back(n_push);

    if (root.contains("print"))
    {
        if (!root["print"].is_object())
        {
            err = "print: 必须是对象";
            return false;
        }
        NodeDesc n_print;
        n_print.type = "print";
        n_print.name = "print";
        n_print.channel = 0;
        n_print.path = "pipelines[0].nodes[2]";
        n_print.config_json = WrapAsSegment("print", root["print"]);
        pd.nodes.push_back(n_print);
    }

    out.push_back(pd);
    return true;
}

} // namespace

NodeConfigManager::NodeConfigManager() {}
NodeConfigManager::~NodeConfigManager() {}

NodeConfigManager &NodeConfigManager::getInstance()
{
    static NodeConfigManager inst;
    return inst;
}

bool NodeConfigManager::fail(const std::string &where, const std::string &what)
{
    std::lock_guard<std::mutex> lk(mtx_);
    error_ = where.empty() ? what : (where + ": " + what);
    std::printf("[configmgr] 配置被拒绝 -> %s\n", error_.c_str());
    return false;
}

void NodeConfigManager::publish(const Snapshot &snap)
{
    std::lock_guard<std::mutex> lk(mtx_);
    snap_ = std::shared_ptr<Snapshot>(new Snapshot(snap));
    error_.clear();
    std::printf("[configmgr] 已加载 %s：%zu 条链路 / pool.blocks=%d / queue_depth=%d / rtsp 端口 %d\n",
                snap_->source.c_str(), snap_->pipelines.size(), snap_->global.pool.blocks,
                snap_->global.pool.queue_depth, snap_->global.media.port);
}

bool NodeConfigManager::parse_document(const std::string &text, const std::string &origin)
{
    json root;
    try
    {
        root = json::parse(text);
    }
    catch (const std::exception &e)
    {
        return fail(origin, std::string("JSON 语法错误：") + e.what());
    }
    if (!root.is_object())
        return fail(origin, "顶层必须是 JSON 对象");

    Snapshot snap;
    snap.source = origin;

    const bool looks_v1 = root.contains("pipelines") || root.contains("global");
    std::string err;
    if (looks_v1)
    {
        if (!ParseV1(root, snap.global, snap.pipelines, err))
            return fail(origin, err);
        snap.from_v0 = false;
    }
    else if (root.contains("push"))
    {
        if (!ParseV0(root, snap.global, snap.pipelines, err))
            return fail(origin, err);
        snap.from_v0 = true;
        std::printf("[configmgr] %s 按 v0（push.json）适配为单链路，stream=%s\n", origin.c_str(),
                    snap.pipelines.empty() ? "-" : snap.pipelines[0].name.c_str());
    }
    else
    {
        return fail(origin, "既不是 v1（缺 pipelines / global），也不是 v0（缺 push 段）");
    }

    publish(snap);
    return true;
}

bool NodeConfigManager::load(const std::string &path)
{
    if (path.empty())
        return fail("", "配置路径为空");
    std::string text;
    if (!ReadWholeFile(path, text))
        return fail(path, "文件不存在或内容为空");
    return parse_document(text, path);
}

bool NodeConfigManager::load_with_fallback(const std::string &explicit_path)
{
    std::vector<std::string> v1;
    std::vector<std::string> v0;
    if (!explicit_path.empty())
    {
        v1.push_back(explicit_path); // argv[1] 优先，且只认它（沿用现有候选路径策略）
    }
    else
    {
        v1.push_back("config/node.json");
        v1.push_back("../config/node.json");
        v0.push_back("config/push.json");
        v0.push_back("../config/push.json");
    }

    std::string text;
    for (size_t i = 0; i < v1.size(); ++i)
    {
        if (!ReadWholeFile(v1[i], text))
            continue;
        // 两者都在 → 优先 node.json，并提示已忽略 push.json（DESIGN §5.6）
        for (size_t j = 0; j < v0.size(); ++j)
        {
            std::string ignored;
            if (ReadWholeFile(v0[j], ignored))
            {
                std::printf("[configmgr] 检测到 v0 配置 %s，因 %s 存在已忽略\n", v0[j].c_str(), v1[i].c_str());
                break;
            }
        }
        return parse_document(text, v1[i]);
    }

    for (size_t i = 0; i < v0.size(); ++i)
    {
        if (!ReadWholeFile(v0[i], text))
            continue;
        return parse_document(text, v0[i]);
    }

    if (!explicit_path.empty())
        return fail("", "找不到配置：" + explicit_path + " 不存在或内容为空（显式路径不回退）");
    return fail("", "找不到配置：已尝试 config/node.json、../config/node.json、config/push.json、"
                    "../config/push.json");
}

const GlobalDesc &NodeConfigManager::global() const
{
    static const GlobalDesc kDefault;
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_ ? snap_->global : kDefault;
}

const std::vector<PipelineDesc> &NodeConfigManager::pipelines() const
{
    static const std::vector<PipelineDesc> kEmpty;
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_ ? snap_->pipelines : kEmpty;
}

const PipelineDesc *NodeConfigManager::find(const std::string &name) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!snap_)
        return nullptr;
    for (size_t i = 0; i < snap_->pipelines.size(); ++i)
    {
        if (snap_->pipelines[i].name == name)
            return &snap_->pipelines[i];
    }
    return nullptr;
}

std::string NodeConfigManager::last_error() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return error_;
}

std::string NodeConfigManager::source_path() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_ ? snap_->source : std::string();
}

bool NodeConfigManager::is_v0() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    return snap_ ? snap_->from_v0 : false;
}

} // namespace config
} // namespace evs
