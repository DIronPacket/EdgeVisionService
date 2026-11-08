#ifndef EVS_NODE_CONFIG_MANAGER_H
#define EVS_NODE_CONFIG_MANAGER_H

// ============================================================
// NodeConfigManager —— 全局配置对象（M1 / T2）
//
// 职责：读 config/node.json（v1）→ 解析 → 校验 → 展开引用 → 合并 global 默认值，
//       产出一份不可变快照供 builder / main 读取。
// 不做：节点创建、连线、内存分配（T5 / T6）。
//
// 兼容：没有 node.json 时读 config/push.json（v0）并合成一条单链路，
//       行为与今天的 main_node.cpp 一致（见 DESIGN §5.6）。
//
// 校验失败一律**拒绝整份配置**并给出带字段路径的错误（last_error()），
// 如 "pipelines[1].nodes[2].type: 缺失或为空"。
// ============================================================

#include "node_config.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace evs
{
namespace config
{

class NodeConfigManager
{
public:
    static NodeConfigManager &getInstance();

    // 解析 + 校验 + 合并默认值。只在启动阶段调用一次。
    // path 为空直接失败；要"自动找配置文件 + v0 回退"请用 load_with_fallback()。
    bool load(const std::string &path);

    // 首选 node.json（v1），没有则用 push.json（v0 合成单链路）。
    // explicit_path 非空时只尝试该路径（对应 argv[1]，沿用现有候选路径策略）。
    bool load_with_fallback(const std::string &explicit_path = std::string());

    // 以下访问器在 load 成功后有效；load 之前返回空默认值。
    const GlobalDesc &global() const;
    const std::vector<PipelineDesc> &pipelines() const;

    // 按 pipeline 名称查找；找不到返回 nullptr。
    const PipelineDesc *find(const std::string &name) const;

    // 最近一次失败原因（带字段路径）；成功后为空串。
    std::string last_error() const;

    // 实际加载的文件路径（诊断用）。
    std::string source_path() const;

    // 是否走的是 v0（push.json）适配路径。
    bool is_v0() const;

private:
    NodeConfigManager();
    ~NodeConfigManager();
    NodeConfigManager(const NodeConfigManager &);
    NodeConfigManager &operator=(const NodeConfigManager &);

    // 快照定义在 .cpp 里（避免把 nlohmann::json 暴露到公开头文件）
    struct Snapshot;

    // 解析入口：自动判定 v1（node.json）/ v0（push.json）
    bool parse_document(const std::string &text, const std::string &origin);
    bool fail(const std::string &where, const std::string &what);
    void publish(const Snapshot &snap);

    mutable std::mutex mtx_;
    std::shared_ptr<Snapshot> snap_;
    std::string error_;
};

} // namespace config
} // namespace evs

#endif // EVS_NODE_CONFIG_MANAGER_H
