#ifndef PIPELINE_NODE_H
#define PIPELINE_NODE_H

#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <iostream>
#include <json.hpp>
#include "media_block.h"
#include "media_pool.h"
#include "global_pool_manager.h"

using json = nlohmann::json;

class PipelineNode : public std::enable_shared_from_this<PipelineNode> {
public:
    PipelineNode(int channel_id, const std::string& name)
        : channel_id_(channel_id), name_(name) {
        full_name_ = "Ch" + std::to_string(channel_id_) + "_" + name_;
    }

    virtual ~PipelineNode() = default;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void set_data(const json& config) = 0;
    virtual void push_input(MediaBlock* block) = 0;

    // ==========================================
    // 【新增】由 Application 统一注入池子指针
    // ==========================================
    void bind_pool(MediaPool* pool) {
        bound_pool_ = pool;
    }

    // 解除所有的上下游链接（关闭流时调用）
    void unlink_all() {
        std::lock_guard<std::mutex> lock(link_mtx_);
        next_nodes_.clear();
        parent_nodes_.clear();
    }

    void link_to(std::shared_ptr<PipelineNode> next) {
        if (!next || next.get() == this) return;
        std::lock_guard<std::mutex> lock(link_mtx_);
        for (auto& w : next_nodes_) if (w.lock() == next) return;
        next_nodes_.push_back(next);
        next->parent_nodes_.push_back(std::weak_ptr<PipelineNode>(shared_from_this()));
    }

    std::string get_full_name() const { return full_name_; }
    int get_channel_id() const { return channel_id_; }

protected:
    // ==========================================
    // 【修改】直接使用绑定的池子，不再查全局表
    // ==========================================
    MediaBlock* acquire_block() {
        if (!bound_pool_) {
            std::cerr << "[" << full_name_ << "] Error: No pool bound to this node!" << std::endl;
            return nullptr;
        }
        
        MediaBlock* block = bound_pool_->acquire();
        if (block) {
            block->owner_pool = bound_pool_; 
            block->channel_id = channel_id_;
        }
        return block;
    }

    void release_block(MediaBlock* block) {
        if (!block) return;
        if (block->refcnt.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (block->owner_pool) {
                block->owner_pool->release(block);
            }
        }
    }

    void dispatch_to_children(MediaBlock* block) {
        std::vector<std::shared_ptr<PipelineNode>> nexts;
        {
            std::lock_guard<std::mutex> lock(link_mtx_);
            for (auto& w : next_nodes_) {
                if (auto n = w.lock()) nexts.push_back(n);
            }
        }

        if (!nexts.empty()) {
            for (size_t i = 0; i < nexts.size(); ++i) {
                block->refcnt.fetch_add(1, std::memory_order_relaxed);
            }
            try {
                for (auto& n : nexts) {
                    n->push_input(block);
                }
            } catch (const std::exception& e) {
                std::cerr << "[PipelineNode] Exception in downstream: " << e.what() << std::endl;
            }
        }

        release_block(block); 
    }

    int channel_id_;
    std::string name_;
    std::string full_name_;

private:
    // 【新增】绑定的物理池子（裸指针，不拥有所有权）
    MediaPool* bound_pool_ = nullptr;

    std::vector<std::weak_ptr<PipelineNode>> next_nodes_;
    std::vector<std::weak_ptr<PipelineNode>> parent_nodes_;
    std::mutex link_mtx_;
};

#endif