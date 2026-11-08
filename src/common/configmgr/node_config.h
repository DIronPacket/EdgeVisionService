#ifndef EVS_NODE_CONFIG_H
#define EVS_NODE_CONFIG_H

// ============================================================
// 多链路配置模型（v1 schema，见 docs/DESIGN_MULTI_PIPELINE.md §5.4）
// 只放"纯数据"，不含解析与校验逻辑（那些在 NodeConfigManager）。
// 所有字符串/数值都用 C++14 可用的类型；除 common/json.hpp 外不引入新依赖。
// ============================================================

#include <string>
#include <vector>

namespace evs
{
namespace config
{

// 池内一种图像规格（对应 global.pool.images[]）。
// 与 MemManager::ImgConfig 保持字段同义，但不直接复用：configmgr 不依赖 MemManager，
// 由 T10b 在装配阶段转成 MemManager::ImgConfig。
struct ImgSpec
{
    std::string name; // 槽位名，如 "nv12"
    int width = 0;
    int height = 0;
    std::string fmt; // "nv12" / "rgb24" / "bgr24" …
};

// 一个节点（pipelines[].nodes[]）。
struct NodeDesc
{
    std::string type;        // vi | vpss | push | print（非空，未知类型由 T5 builder 报错）
    std::string name;        // 节点名，缺省 = type；队列名 = name + channel
    int channel = 0;         // 传给 create(channel_id, name)
    std::string config_json; // 已包成 {"<type>": {...}} 的字符串，可直接喂 set_data()
    std::string path;        // 错误定位用，如 "pipelines[0].nodes[2]"
};

// 一条链路（pipelines[]）。nodes 顺序即连线顺序。
struct PipelineDesc
{
    std::string name;
    int channel = 0;
    bool enable = true;
    std::vector<NodeDesc> nodes;
    std::string path; // "pipelines[0]"
};

// global.media.rtsp
struct MediaDesc
{
    int port = 8550;
    std::string bind_ip = "0.0.0.0";
    std::string app = "live";
};

// global.log
struct LogDesc
{
    std::string level = "info";
    std::string file;
};

// global.pool（块数 / 队列深度 / 每路配额，D9 = 方案 A）
struct PoolDesc
{
    // 默认值取 DESIGN §7.1 T10a 的目标值（现状代码是 50 / 20，由 T10a 改成读这里）
    int blocks = 64;
    int queue_depth = 8;
    // 每路最多同时占用的块数；0 或负数 = 不限制（T10a 生效）
    int quota_per_channel = 0;
    std::vector<ImgSpec> images;
};

// global 段：进程级资源，与链路数量无关。
struct GlobalDesc
{
    PoolDesc pool;
    MediaDesc media;
    LogDesc log;
};

} // namespace config
} // namespace evs

#endif // EVS_NODE_CONFIG_H
