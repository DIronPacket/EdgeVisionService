#ifndef MEDIA_BLOCK_H
#define MEDIA_BLOCK_H

#include <atomic>
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>

struct MemorySlot {
    uint8_t* data = nullptr;
    size_t capacity = 0;
    size_t actual_size = 0;
};

class MediaPool; // 前置声明

struct MediaBlock {
    std::atomic<int> refcnt{0}; 
    
    // 【核心】记录物理内存的归属池子
    MediaPool* owner_pool = nullptr; 
    
    int channel_id = 0;
    uint64_t pts = 0;
    uint64_t dts = 0;

    uint8_t* bulk_memory_base = nullptr;
    size_t bulk_memory_size = 0;

    // C++14 不支持 string_view，统一使用 std::string
    std::unordered_map<std::string, MemorySlot> slots;

    MemorySlot* get_slot(const std::string& name) {
        auto it = slots.find(name);
        return it != slots.end() ? &it->second : nullptr;
    }

    // 归还前的数据清理
    void clear() {
        pts = 0;
        dts = 0;
        channel_id = 0;
        owner_pool = nullptr; // 【补充】防止野指针残留
        for (auto& pair : slots) {
            pair.second.actual_size = 0; 
        }
    }
};

#endif