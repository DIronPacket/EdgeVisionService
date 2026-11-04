#include "pipeline_node.h"

// 解除所有的上下游链接（关闭流时调用）
void PipelineNode::unlink_all() {
  std::lock_guard<std::mutex> lock(link_mtx_);
  next_nodes_.clear();
  parent_nodes_.clear();
}

void PipelineNode::link_to(PipelineNode* next) {
  if (!next || next == this)
    return;
  std::lock_guard<std::mutex> lock(link_mtx_);
  for(auto iter=next_nodes_.begin();iter!=next_nodes_.end();++iter)
  {
    if(*iter==next) return;
  }
  next_nodes_.push_back(next);
  printf("src:%s dst:%s \n",this->m_name.c_str(),next->m_name.c_str());
  next->parent_nodes_.push_back(this);
}
void PipelineNode::push_data(pool::MemBlock* block)
{
  m_queue.pushFrame(block);
}
void PipelineNode::push_input_pipe(pool::MemBlock* block)
{
    //将buffer 推送到下一节点的队列中。
    for(auto iter = next_nodes_.begin(); iter != next_nodes_.end(); iter ++){
        PipelineNode* mq = *iter;
        //判断推送源是否在父节点列表中
        for(auto ptr = mq->parent_nodes_.begin(); ptr != mq->parent_nodes_.end(); ptr ++){
            PipelineNode* src = *ptr;
            if(src == this){
                mq->push_data(block);
                // printf("src:%s push dst:%s\n",src->m_name.c_str(),)
            }
        }
    }
}