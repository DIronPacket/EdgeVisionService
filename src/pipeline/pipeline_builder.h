#ifndef PIPELINE_BUILDER_H
#define PIPELINE_BUILDER_H

// ============================================================
// PipelineBuilder —— 把配置里的 pipelines[] 变成「已就绪、可 start」的节点集合
// （M1 / T5，见 docs/design/DESIGN_MULTI_PIPELINE.md §5.3）
//
//   config/node.json ──NodeConfigManager──> vector<PipelineDesc>
//                                                  │  PipelineBuilder
//                                                  ▼
//                         vector<PipelineSet>（已 create / set_data / link）
//                                                  │  PipelineBuilder::start_all()
//                                                  ▼
//                                              运行中 ──stop_all()──> 停止
//
// 每条链的装配顺序是固定的（改之前先读 HANDOFF §2.4）：
//   1. NodeFactory::create(type)      → 造对象，未知 type 即失败
//   2. node->create(channel, name)    → 建队列，队列名 = name + channel
//   3. node->set_data(config_json)    → **必须在 start() 之前**（PushNode 靠它
//                                        决定端口/app/分辨率，ViNode 靠它决定源）
//   4. prev->link_to(next)            → 连线，顺序 = 配置里 nodes 的顺序
//   5. start()/stop()                 → 交给 start_all()/stop_all()，main 不直接碰节点
//
// 链路顺序的硬规则（T5 验收项；违反 → 整链拒绝，其余链路照跑，见 N3）：
//   R1 nodes 至少 2 个节点（一个节点连不成"源 → 终点"）
//   R2 首个节点必须是源节点（vi）
//   R3 末个节点必须是终点节点（push / print）——「没有末端节点」的链路不允许
//   R4 源节点（vi）只能出现在首位
//   R5 print 是终点（要把块归还内存池），只能出现在末位
// 这些规则在 NodeConfigManager（T2）里**没有**做：T2 只管单节点字段合法性
// （type 非空、队列名唯一、端口只来自 global），跨节点的拓扑合法性归 builder。
// ============================================================

#include "node_config.h"
#include "node_config_manager.h"
#include "pipeline_node.h"

#include <memory>
#include <string>
#include <vector>

// 一条已装配好的链路。nodes 顺序 = 数据流顺序。
struct PipelineSet {
  std::string name;   // 配置里的 pipelines[].name，如 "ch0"
  int channel = 0;    // 配置里的 pipelines[].channel
  std::vector<std::shared_ptr<PipelineNode> > nodes;

  bool empty() const { return nodes.empty(); }

  // "ch0[0] vi0 -> vpss0 -> rtsp0 -> print0"（日志定位用，节点名来自 get_full_name()）
  std::string describe() const;
};

class PipelineBuilder {
public:
  // cfg 必须在 build 期间保持有效（PipelineBuilder 只持有引用，不拷贝配置）
  explicit PipelineBuilder(const evs::config::NodeConfigManager &cfg);
  ~PipelineBuilder();

  // 装配配置里的**所有** enable=true 的链路，成功的一条塞进 out（不清空 out）。
  // 单链失败 → 只清理该链（析构已建节点 + 解除连线），其余照跑（N3）。
  // 返回 true = 至少有一条链路建成；false = 一条都没建成（out 里不会留半条链）。
  bool build_all(std::vector<PipelineSet> &out);

  // 装配单条链路。失败时 out 被清空，原因见 last_error()（带字段路径）。
  bool build_one(const evs::config::PipelineDesc &d, PipelineSet &out);

  // 启动所有链路的节点。**逐链逆序**（终点先起、源头最后，与旧 main_node 的
  // node3→node2→node1 一致）：源开始产帧时下游线程与队列都已就绪，避免启动
  // 瞬间队列堆块。返回 true = 至少启动了一个节点。
  static bool start_all(std::vector<PipelineSet> &sets);

  // 停止所有链路的节点。**逐链正序**（源头先停、终点最后）：先掐断入口，
  // 再让下游把队列里已到的块处理完。
  static void stop_all(std::vector<PipelineSet> &sets);

  // 拓扑规则（T5 验收的另一半）。对外暴露是为了让测试与文档共用同一份定义，
  // 不要在别处再抄一遍规则。err 为带字段路径的失败原因。
  static bool check_order(const evs::config::PipelineDesc &d, std::string &err);
  static bool is_source_type(const std::string &type); // "vi"
  static bool is_sink_type(const std::string &type);   // "push" / "print"

  // ---- 诊断计数（build_all 每次调用前清零）----
  int built_count() const { return built_; }
  int skipped_count() const { return skipped_; }  // enable=false 被跳过
  int failed_count() const { return failed_; }
  const std::vector<std::string> &errors() const { return errors_; }

  // 最近一次失败原因（build_one / build_all 共用）
  std::string last_error() const { return last_error_; }

private:
  PipelineBuilder(const PipelineBuilder &);
  PipelineBuilder &operator=(const PipelineBuilder &);

  // 解除连线 + 释放该链已建的节点（shared_ptr 析构会调 stop()，未 start 时是空操作）
  static void destroy_set(PipelineSet &set);

  // 日志用标签："ch0[0]"，path 为空时退回 name
  static std::string label(const evs::config::PipelineDesc &d);

  const evs::config::NodeConfigManager &cfg_;

  std::string last_error_;
  std::vector<std::string> errors_;
  int built_;
  int skipped_;
  int failed_;
};

#endif // PIPELINE_BUILDER_H
