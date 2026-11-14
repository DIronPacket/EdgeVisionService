#ifndef NODE_FACTORY_H
#define NODE_FACTORY_H

// ============================================================
// NodeFactory —— 节点类型注册表（M1 / T5，见 docs/design/DESIGN_MULTI_PIPELINE.md §5.3）
//
// 职责：把「配置里写的 type 字符串」翻译成「一个刚构造好的节点对象」。
// 不做：灌配置（set_data）、连线（link_to）、起线程（start）—— 那些在
//       PipelineBuilder 里，顺序不能变（见 pipeline_builder.h 顶部说明）。
//
// 为什么是「集中注册」而不是 DESIGN 草案里的「各 moudle 的 cpp 里静态注册」：
//   pipeline_api 默认编成 .so（顶层 CMakeLists 的 BUILD_SHARED_MODULE=ON），
//   但同一份源码也可能被配成 STATIC。静态库形态下，某个 .cpp 里的静态
//   注册对象若没有任何符号被引用，链接器会整块丢掉该目标文件，注册就
//   静默失效（现象是"类型明明注册过却报未知类型"）。由本文件 include 四个
//   节点的头并显式注册，两种库形态行为一致，也不依赖静态初始化顺序。
//
// 新增一种节点类型时改两处：
//   1) 本文件 register_builtin() 里加一行 register_type("xxx", &CreateXxx)；
//   2) 配置里写 "type": "xxx"。
// ============================================================

#include "pipeline_node.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// 节点创建函数：只造对象，不传参（channel / name 由 builder 紧接着调
// create(channel_id, name) 设置，队列名 = name + channel）。
typedef std::shared_ptr<PipelineNode> (*NodeCreator)();

class NodeFactory {
public:
  // 进程级单例（C++11 起函数内静态变量初始化线程安全）
  static NodeFactory &getInstance();

  // 注册一种 type。重复注册（同名已存在）返回 false，且**不覆盖**已有实现，
  // 避免启动顺序不同导致同名类型实现漂移。
  bool register_type(const std::string &type, NodeCreator creator);

  bool has_type(const std::string &type) const;

  // 未知 type → 返回 nullptr，**不抛异常、不退出进程**：
  // 由调用方（PipelineBuilder）决定是该链跳过还是整体失败（N3：单链失败不影响其他链）。
  // 失败原因见 last_error()，形如：
  //   NodeFactory: 未知节点类型 "vpsss"（已注册：print, push, vi, vpss）
  std::shared_ptr<PipelineNode> create(const std::string &type);

  // 已注册的 type 列表（升序）。日志与报错共用，新增类型自动出现。
  std::vector<std::string> types() const;

  // 最近一次 create() 失败原因；成功后清空。
  std::string last_error() const;

private:
  NodeFactory();
  ~NodeFactory();
  NodeFactory(const NodeFactory &);
  NodeFactory &operator=(const NodeFactory &);

  // 内置四类节点：vi / vpss / push / print
  void register_builtin();

  mutable std::mutex mtx_;
  std::map<std::string, NodeCreator> creators_;
  std::string error_;
};

#endif // NODE_FACTORY_H
