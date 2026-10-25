#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <iostream>
#include <cstdint>
#include <cstddef>
#include <utility>

enum class PixelFormat
{
    FMT_UNKNOWN,
    FMT_BGR888,
    FMT_NV12,
    FMT_YUV420P
};

/**
 * @brief 统一帧结构体，全工程采集/预处理/算法/编码共用
 * 尽量减少内存拷贝，支持外部buffer和内部自分配两种模式
 */
struct FrameBuffer
{
    uint8_t* data = nullptr;
    size_t data_size = 0;

    int width = 0;
    int height = 0;
    PixelFormat fmt = PixelFormat::FMT_UNKNOWN;
    int stride = 0;

    uint64_t timestamp_ms = 0; // 时间戳，毫秒

    // 标记是否由本对象管理内存
    bool own_memory = false;

    FrameBuffer() = default;
    ~FrameBuffer()
    {
        if (own_memory && data != nullptr)
        {
            delete[] data;
            data = nullptr;
            data_size = 0;
        }
    }

    // 禁止拷贝，避免内存双重释放；允许移动
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    FrameBuffer(FrameBuffer&& other) noexcept
    {
        *this = std::move(other);
    }
    FrameBuffer& operator=(FrameBuffer&& other) noexcept
    {
        if (this != &other)
        {
            // 释放自身资源
            if (own_memory && data)
                delete[] data;

            data = other.data;
            data_size = other.data_size;
            width = other.width;
            height = other.height;
            fmt = other.fmt;
            timestamp_ms = other.timestamp_ms;
            own_memory = other.own_memory;

            other.data = nullptr;
            other.data_size = 0;
            other.own_memory = false;
        }
        return *this;
    }

    // 分配内存
    bool allocate(size_t sz)
    {
        // printf("allocate req:%zu, old data=%p old datasize=%zu\n", sz, data, data_size);
        if (own_memory && data && data_size >= sz)
        {
            // printf("allocate reuse buffer\n");
            return true;
        }
        if (own_memory && data)
        {
            delete[] data;
            data = nullptr;
        }
        data = new uint8_t[sz];
        data_size = sz;
        own_memory = true;
        printf("allocate new buffer %p size=%zu\n", data, data_size);
        return data != nullptr;
    }

    void release()
    {
        printf("release buffer \n");
        if (own_memory && data)
        {
            delete[] data;
            data = nullptr;
        }
        data_size = 0;
        width = 0;
        height = 0;
        fmt = PixelFormat::FMT_UNKNOWN;
        timestamp_ms = 0;
        own_memory = false;
    }
};

#endif
