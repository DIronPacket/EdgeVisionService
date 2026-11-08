#include "pipeline_builder.h"

#include "node_factory.h"

#include <iostream>
#include <sstream>

std::string PipelineSet::describe() const {
  std::ostringstream oss;
  oss << name << "[" << channel << "] ";
  if (nodes.empty()) {
    oss << "(空)";
    return oss.str();
  }
  for (size_t i = 0; i < nodes.size(); ++i) {
    if (i > 0)
      oss << " -> ";
    oss << (nodes[i] ? nodes[i]->get_full_name() : std::string("(null)"));
  }
  return oss.str();
}

PipelineBuilder::PipelineBuilder(const evs::config::NodeConfigManager &cfg)
    : cfg_(cfg), built_(0), skipped_(0), failed_(0) {}

PipelineBuilder::~PipelineBuilder() {}

// ------------------------------------------------------------------
// 拓扑规则
// ------------------------------------------------------------------

bool PipelineBuilder::is_source_type(const std::string &type) {
  return type == "vi";
}

bool PipelineBuilder::is_sink_type(const std::string &type) {
  return type == "push" || type == "print";
}

bool PipelineBuilder::check_order(const evs::config::PipelineDesc &d,
                                  std::string &err) {
  const std::string where = label(d);
  const size_t n = d.nodes.size();

  // R1：至少两个节点
  if (n == 0) {
    err = where + ".nodes: 链路里没有任何节点";
    return false;
  }
  if (n < 2) {
    err = where + ".nodes: 只有 1 个节点（" + d.nodes[0].type +
          "），构不成「源 → 终点」的链路（至少 2 个节点）";
    return false;
  }

  // R2：首个节点必须是源
  if (!is_source_type(d.nodes.front().type)) {
    err = d.nodes.front().path + ".type: 首个节点必须是源节点（vi），实际是 \"" +
          d.nodes.front().type + "\" —— 节点顺序写错了（数据从哪来？）";
    return false;
  }

  // R3：末尾节点必须是终点（T5 验收：「节点顺序错（无末端节点）报错」）
  if (!is_sink_type(d.nodes.back().type)) {
    std::ostringstream oss;
    oss << d.nodes.back().path << ".type: 链路没有末端节点 —— 最后一个节点必须是"
        << "输出终点（push 或 print），实际是 \"" << d.nodes.back().type
        << "\"（数据到这儿没人消费，块不会归还内存池）";
    err = oss.str();
    return false;
  }

  // R4：源节点只能出现在首位
  for (size_t i = 1; i < n; ++i) {
    if (is_source_type(d.nodes[i].type)) {
      std::ostringstream oss;
      oss << d.nodes[i].path << ".type: 源节点（" << d.nodes[i].type
          << "）只能出现在首位，这里是第 " << i + 1 << " 个节点";
      err = oss.str();
      return false;
    }
  }

  // R5：print 是终点，只能出现在末位
  for (size_t i = 0; i + 1 < n; ++i) {
    if (d.nodes[i].type == "print") {
      std::ostringstream oss;
      oss << d.nodes[i].path << ".type: print 是终点节点（负责把内存块归还"
          << "内存池），只能出现在末位，这里是第 " << i + 1 << " 个节点";
      err = oss.str();
      return false;
    }
  }

  return true;
}

std::string PipelineBuilder::label(const evs::config::PipelineDesc &d) {
  std::ostringstream oss;
  if (!d.name.empty())
    oss << d.name;
  if (d.name.empty() || !d.path.empty())
    oss << "[" << d.channel << "]";
  if (!d.path.empty())
    oss << "(" << d.path << ")";
  if (oss.str().empty())
    oss << "(未命名链路)";
  return oss.str();
}

// ------------------------------------------------------------------
// 装配
// ------------------------------------------------------------------

void PipelineBuilder::destroy_set(PipelineSet &set) {
  for (size_t i = 0; i < set.nodes.size(); ++i) {
    if (set.nodes[i])
      set.nodes[i]->unlink_all();
  }
  set.nodes.clear();
}

bool PipelineBuilder::build_one(const evs::config::PipelineDesc &d,
                                PipelineSet &out) {
  out.nodes.clear();
  out.name = d.name;
  out.channel = d.channel;
  last_error_.clear();

  std::string err;
  if (!check_order(d, err)) {
    last_error_ = err;
    return false;
  }

  // 1) 造对象：任何一步失败都不留半条链
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    const evs::config::NodeDesc &nd = d.nodes[i];
    std::shared_ptr<PipelineNode> node = NodeFactory::getInstance().create(nd.type);
    if (!node) {
      last_error_ = nd.path + ".type: " + NodeFactory::getInstance().last_error();
      destroy_set(out);
      return false;
    }
    // 队列名 = name + channel（各节点 create() 里拼），T2 已校验全局唯一
    node->create(nd.channel, nd.name);
    out.nodes.push_back(node);
  }

  // 2) 灌配置：必须在 start() 之前。空 config 表示该节点全用内置默认值。
  for (size_t i = 0; i < d.nodes.size(); ++i) {
    if (out.nodes[i] && !d.nodes[i].config_json.empty())
      out.nodes[i]->set_data(d.nodes[i].config_json.c_str());
  }

  // 3) 连线：按配置顺序 i -> i+1
  for (size_t i = 0; i + 1 < out.nodes.size(); ++i) {
    out.nodes[i]->link_to(out.nodes[i + 1].get());
  }

  std::cout << "[builder] 链路就绪: " << out.describe() << std::endl;
  return true;
}

bool PipelineBuilder::build_all(std::vector<PipelineSet> &out) {
  last_error_.clear();
  errors_.clear();
  built_ = 0;
  skipped_ = 0;
  failed_ = 0;

  const std::vector<evs::config::PipelineDesc> &list = cfg_.pipelines();
  if (list.empty()) {
    last_error_ = "配置里没有任何链路（pipelines 为空）";
    return false;
  }

  for (size_t i = 0; i < list.size(); ++i) {
    const evs::config::PipelineDesc &d = list[i];

    if (!d.enable) {
      ++skipped_;
      std::cout << "[builder] 跳过 " << label(d) << "（enable=false）" << std::endl;
      continue;
    }

    PipelineSet set;
    if (!build_one(d, set)) {
      // N3：单链失败不影响其他链
      ++failed_;
      errors_.push_back(last_error_);
      std::cerr << "[builder] 链路构建失败: " << last_error_
                << "（该链已跳过，其余链路继续）" << std::endl;
      continue;
    }

    ++built_;
    out.push_back(set);
  }

  std::cout << "[builder] 汇总: 建成 " << built_ << " 条, 失败 " << failed_
            << " 条, 跳过 " << skipped_ << " 条" << std::endl;

  if (built_ == 0) {
    if (last_error_.empty())
      last_error_ = "没有任何链路构建成功";
    return false;
  }
  last_error_.clear();
  return true;
}

// ------------------------------------------------------------------
// 起停
// ------------------------------------------------------------------

bool PipelineBuilder::start_all(std::vector<PipelineSet> &sets) {
  bool started_any = false;
  for (size_t i = 0; i < sets.size(); ++i) {
    PipelineSet &s = sets[i];
    // 逆序：终点先起。源开始产帧时，下游线程已经在等队列里的块了。
    for (size_t k = s.nodes.size(); k > 0; --k) {
      if (!s.nodes[k - 1])
        continue;
      s.nodes[k - 1]->start();
      started_any = true;
    }
  }
  return started_any;
}

void PipelineBuilder::stop_all(std::vector<PipelineSet> &sets) {
  for (size_t i = 0; i < sets.size(); ++i) {
    PipelineSet &s = sets[i];
    // 正序：源头先停。掐断入口后再让下游把队列里已到的块处理完。
    for (size_t k = 0; k < s.nodes.size(); ++k) {
      if (s.nodes[k])
        s.nodes[k]->stop();
    }
  }
}
