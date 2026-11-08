#include "node_factory.h"

// 内置节点的实现（本文件是唯一 include 全部 moudle 头的地方）
#include "moudle_print.h"
#include "moudle_push.h"
#include "moudle_vi.h"
#include "moudle_vpss.h"

#include <iostream>
#include <sstream>

namespace {

// 各节点的构造入口。返回 shared_ptr<派生类> 可隐式转成 shared_ptr<PipelineNode>，
// 与 NodeCreator 的函数指针类型匹配。
std::shared_ptr<PipelineNode> CreateVi() { return std::make_shared<ViNode>(); }
std::shared_ptr<PipelineNode> CreateVpss() { return std::make_shared<VpssNode>(); }
std::shared_ptr<PipelineNode> CreatePush() { return std::make_shared<PushNode>(); }
std::shared_ptr<PipelineNode> CreatePrint() { return std::make_shared<PrintNode>(); }

} // namespace

NodeFactory &NodeFactory::getInstance() {
  static NodeFactory inst;
  return inst;
}

NodeFactory::NodeFactory() { register_builtin(); }

NodeFactory::~NodeFactory() {}

void NodeFactory::register_builtin() {
  register_type("vi", &CreateVi);
  register_type("vpss", &CreateVpss);
  register_type("push", &CreatePush);
  register_type("print", &CreatePrint);
}

bool NodeFactory::register_type(const std::string &type, NodeCreator creator) {
  if (type.empty() || !creator) {
    std::cerr << "[NodeFactory] register_type: type 为空或 creator 为空，已忽略"
              << std::endl;
    return false;
  }

  std::lock_guard<std::mutex> lock(mtx_);
  if (creators_.count(type) > 0) {
    std::cerr << "[NodeFactory] register_type: 类型 \"" << type
              << "\" 已注册，保留原实现" << std::endl;
    return false;
  }
  creators_[type] = creator;
  return true;
}

bool NodeFactory::has_type(const std::string &type) const {
  std::lock_guard<std::mutex> lock(mtx_);
  return creators_.count(type) > 0;
}

std::shared_ptr<PipelineNode> NodeFactory::create(const std::string &type) {
  NodeCreator creator = nullptr;
  std::string err;

  {
    std::lock_guard<std::mutex> lock(mtx_);
    std::map<std::string, NodeCreator>::const_iterator it = creators_.find(type);
    if (it == creators_.end() || !it->second) {
      // 报错时把所有已注册类型列出来：配置里写错一个字母也能一眼定位
      std::ostringstream oss;
      oss << "未知节点类型 \"" << type << "\"（已注册：";
      bool first = true;
      for (std::map<std::string, NodeCreator>::const_iterator i =
               creators_.begin();
           i != creators_.end(); ++i) {
        if (!first)
          oss << ", ";
        oss << i->first;
        first = false;
      }
      oss << "）";
      error_ = oss.str();
      std::cerr << "[NodeFactory] " << error_ << std::endl;
      return std::shared_ptr<PipelineNode>();
    }
    creator = it->second;
    error_.clear();
  }

  // 构造放在锁外：creator 里将来可能做耗时初始化，不应阻塞其他类型的 create
  std::shared_ptr<PipelineNode> node = creator();
  if (!node) {
    std::lock_guard<std::mutex> lock(mtx_);
    error_ = "节点构造失败，类型 \"" + type + "\"";
    std::cerr << "[NodeFactory] " << error_ << std::endl;
  }
  return node;
}

std::vector<std::string> NodeFactory::types() const {
  std::lock_guard<std::mutex> lock(mtx_);
  std::vector<std::string> out;
  out.reserve(creators_.size());
  for (std::map<std::string, NodeCreator>::const_iterator it = creators_.begin();
       it != creators_.end(); ++it) {
    out.push_back(it->first);
  }
  return out; // std::map 迭代天然升序
}

std::string NodeFactory::last_error() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return error_;
}
