#ifndef MEMORY_APPLICATION_H
#define MEMORY_APPLICATION_H

#include <memory>
#include <vector>
#include <mutex>
#include <iostream>
#include <string>
#include "type_registry.h"
#include "global_pool_manager.h"
#include "pipeline_node.h"

class MemoryApplication {
public:
    static MemoryApplication& get_instance() {
        static MemoryApplication instance;
        return instance;
    }

    // 注册类型（全程序一次）
    void register_types() {
        TypeRegistry::instance().register_image("nv12", 1.5f);
        TypeRegistry::instance().register_image("rgb", 3.0f);
        TypeRegistry::instance().register_struct("YoloRes", 1024);
    }

    // 初始化常驻内存池
    void init_pools() {
        NodePoolConfig cfg1;
        cfg1.frame_count = 30;
        cfg1.images = {{"nv12", 1920, 1080}};
        cfg1.structs = {{"YoloRes", 1024}};
        GlobalPoolManager::get_instance().register_node_pool("Ch1_FileSrc", cfg1);
    }

    // ==========================================
    // 【修改】只负责登记节点，不干涉拓扑和构造
    // ==========================================
    void register_node(std::shared_ptr<PipelineNode> node) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (node) {
            nodes_.push_back(node);
            std::cout << "[App] Node registered: " << node->get_full_name() << std::endl;
        }
    }

    // 启动所有已登记的节点
    void start_all() {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& node : nodes_) {
            // 启动前，自动分配它所属通道的池子
            auto pool = GlobalPoolManager::get_instance().get_pool(node->get_full_name());
            if (pool) {
                node->bind_pool(pool.get());
            }
            node->start();
        }
    }

    // 停止并销毁所有节点（保留池子）
    void stop_and_destroy_nodes() {
        std::lock_guard<std::mutex> lock(mtx_);
        // 1. 逆序停止
        for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it) {
            (*it)->stop();
        }
        // 2. 清空容器，节点被彻底销毁
        nodes_.clear();
        std::cout << "[App] All nodes destroyed. Pools remain intact." << std::endl;
    }

    // 程序退出时，彻底销毁池子
    void shutdown() {
        stop_and_destroy_nodes();
        GlobalPoolManager::get_instance().clear();
    }

private:
    MemoryApplication() = default;
    std::vector<std::shared_ptr<PipelineNode>> nodes_;
    std::mutex mtx_;
};

#endif