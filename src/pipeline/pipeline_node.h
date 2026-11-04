#ifndef PIPELINE_NODE_H
#define PIPELINE_NODE_H

#include "MemPool.h"
#include <iostream>
#include <json.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <list>

using json = nlohmann::json;

class PipelineNode : public std::enable_shared_from_this<PipelineNode> {
public:
  PipelineNode() {}

  virtual ~PipelineNode() = default;

  virtual void start() = 0;
  virtual void stop() = 0;
  virtual void create(int channel_id, const std::string &name) = 0;
  virtual void set_data(const char *config) = 0;

  // 解除所有的上下游链接（关闭流时调用）
  void unlink_all();
  void link_to(PipelineNode* next);
  void push_data(pool::MemBlock* block);

  std::string get_full_name() const { return m_name; }
  int get_channel_id() const { return channel_id_; }

  void push_input_pipe(pool::MemBlock* block);

  std::list<PipelineNode*> next_nodes_;
  std::list<PipelineNode*> parent_nodes_;
protected:
  int channel_id_;
  std::string m_name;
  pool::MemQueue m_queue;

private:

  std::mutex link_mtx_;
};

#endif