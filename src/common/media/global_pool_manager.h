#ifndef GLOBAL_POOL_MANAGER_H
#define GLOBAL_POOL_MANAGER_H

#include <memory>
#include <unordered_map>
#include <mutex>
#include <iostream>
#include <string>
#include <vector>
#include "media_pool.h"
// #include "type_registry.h" // 如果你用到了里面的东西可以带上

// ==========================================
// 新增：打包池子的配置
// ==========================================
struct NodePoolConfig {
    size_t frame_count;
    std::vector<ImageConfig> images;
    std::vector<std::pair<std::string, size_t>> structs;
};

class GlobalPoolManager {
public:
    static GlobalPoolManager& get_instance() {
        static GlobalPoolManager instance;
        return instance;
    }

    // 注册节点专属池子
    void register_node_pool(const std::string& node_name, const NodePoolConfig& config) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (pools_.find(node_name) != pools_.end()) {
            std::cerr << "[GlobalPoolManager] Warning: Pool for node " << node_name << " already exists!" << std::endl;
            return;
        }

        // 内部调用 MediaPool 的构造函数
        auto pool = std::make_shared<MediaPool>(config.frame_count, config.images, config.structs);
        pools_[node_name] = pool;
        std::cout << "[GlobalPoolManager] Created pool for node: " << node_name << std::endl;
    }

    std::shared_ptr<MediaPool> get_pool(const std::string& node_name) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = pools_.find(node_name);
        if (it != pools_.end()) return it->second;
        return nullptr;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& pair : pools_) {
            if (!pair.second->is_all_returned()) {
                std::cerr << "[GlobalPoolManager] Warning: " << pair.first 
                          << " still has blocks in use during clear!" << std::endl;
            }
        }
        pools_.clear();
        std::cout << "[GlobalPoolManager] All pools cleared." << std::endl;
    }

private:
    GlobalPoolManager() = default;
    std::unordered_map<std::string, std::shared_ptr<MediaPool>> pools_;
    std::mutex mtx_;
};

#endif