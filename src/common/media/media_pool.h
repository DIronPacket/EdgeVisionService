#ifndef MEDIA_POOL_H
#define MEDIA_POOL_H

#include <iostream>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <string>
#include <vector>
#include <mutex>
#include "media_block.h"
#include "type_registry.h" // 【必须引入】你写的注册表

struct ImageConfig {
    std::string format_name;
    int width;
    int height;
};

// 8字节对齐
inline size_t AlignUp(size_t size, size_t alignment = 8) {
    return (size + alignment - 1) & ~(alignment - 1);
}

class MediaPool {
public:
    MediaPool(size_t init_count, 
              const std::vector<ImageConfig>& images,
              const std::vector<std::pair<std::string, size_t>>& structs) 
    {
        size_t total_payload_size = 0;
        std::unordered_map<std::string, size_t> image_sizes;
        
        // ==========================================
        // 【修改点 1】通过 TypeRegistry 动态计算图像大小
        // ==========================================
        for (const auto& img : images) {
            auto desc = TypeRegistry::instance().get(img.format_name);
            if (!desc || !desc->is_image) {
                std::cerr << "[MediaPool] Error: Image format " << img.format_name << " not registered!" << std::endl;
                continue; // 或者直接抛异常
            }
            // 根据注册的 bytes_per_pixel 自动算，不再写 if (nv12)...
            size_t sz = static_cast<size_t>(img.width * img.height * desc->bytes_per_pixel);
            
            image_sizes[img.format_name] = AlignUp(sz);
            total_payload_size += AlignUp(sz);
        }

        // ==========================================
        // 【修改点 2】增加对结构体的校验（可选）
        // ==========================================
        for (const auto& st : structs) {
            auto desc = TypeRegistry::instance().get(st.first);
            if (!desc || desc->is_image) {
                std::cerr << "[MediaPool] Error: Struct " << st.first << " not registered or is image!" << std::endl;
                continue;
            }
            total_payload_size += AlignUp(st.second);
        }

        // 下面的切片逻辑、分配逻辑完全保持不变
        for (size_t i = 0; i < init_count; ++i) {
            MediaBlock* block = new MediaBlock();
            block->bulk_memory_base = new uint8_t[total_payload_size];
            block->bulk_memory_size = total_payload_size;
            
            uint8_t* bulk_memory = block->bulk_memory_base;
            size_t offset = 0;

            for (const auto& img : images) {
                size_t sz = image_sizes[img.format_name];
                block->slots[img.format_name] = { bulk_memory + offset, sz, 0 };
                offset += sz;
            }
            for (const auto& st : structs) {
                size_t sz = AlignUp(st.second);
                block->slots[st.first] = { bulk_memory + offset, sz, 0 };
                offset += sz;
            }
            
            all_blocks_.push_back(block);
            free_list_.push_back(block);
        }
        std::cout << "[MediaPool] Initialized " << init_count << " blocks. Total size: " 
                  << total_payload_size * init_count / (1024 * 1024) << " MB" << std::endl;
    }

    ~MediaPool() {
        for (auto* block : all_blocks_) {
            delete[] block->bulk_memory_base;
            delete block;
        }
    }

    MediaBlock* acquire() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (free_list_.empty()) return nullptr; 
        
        MediaBlock* block = free_list_.back();
        free_list_.pop_back();
        block->refcnt.store(1, std::memory_order_relaxed);
        return block;
    }

    void release(MediaBlock* block) {
        if (!block) return;
        block->clear();
        std::lock_guard<std::mutex> lock(mtx_);
        free_list_.push_back(block);
    }

    bool is_all_returned() {
        std::lock_guard<std::mutex> lock(mtx_);
        return free_list_.size() == all_blocks_.size();
    }

private:
    std::vector<MediaBlock*> all_blocks_; 
    std::vector<MediaBlock*> free_list_;  
    std::mutex mtx_;
};

#endif