#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <cstdint>
#include "media_block.h"

enum class PixelFormat {
    FMT_UNKNOWN,
    FMT_BGR888,
    FMT_NV12,
    FMT_YUV420P
};

// 仅作为MediaBlock的视图，不拥有内存，不负责释放
struct FrameBuffer {
    uint8_t* data = nullptr;
    size_t data_size = 0;

    int width = 0;
    int height = 0;
    PixelFormat fmt = PixelFormat::FMT_UNKNOWN;
    int stride = 0;
    uint64_t timestamp_ms = 0;

    MediaBlock* block_ref = nullptr; // 关联的物理块

    // 从 MediaBlock 绑定视图，不拷贝数据！
    bool bind_from_media_block(MediaBlock* block, const std::string& slot_name) {
        MemorySlot* slot = block->get_slot(slot_name);
        if (slot && slot->actual_size > 0) {
            data = slot->data;
            data_size = slot->actual_size;
            timestamp_ms = block->pts;
            block_ref = block;
            return true;
        }
        return false;
    }
};

#endif