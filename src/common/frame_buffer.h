#ifndef FRAME_BUFFER_H
#define FRAME_BUFFER_H

#include <cstdint>
#include <iostream>
#include <cstddef>
#include "frame_mem.h"

enum class PixelFormat
{
  FMT_UNKNOWN = -1,
  FMT_BGR888 = 0,
  FMT_NV12 = 1,
  FMT_YUV420P = 2,
  FMT_YUV422 = 3
};

// MediaBlock 的视图：data[0..2] 只是指针，内存归谁由 owned_mem / native 决定。
//   1) Allocate()   → 调 pool::FrameMemAlloc() 拿内存（rk3588 上是 MPP DRM 缓冲，
//                     其他平台退化 new[]），owned_mem 记 CPU 基址、native 记平台句柄；
//                     Release() 调 pool::FrameMemFree() 还回去。
//   2) BindMemory() → 绑定外部内存（不写 owned_mem/native），Release() 不会去动别人的内存。
// 析构调 Release()：new 出来的对象 delete 即归还（MemMeta 槽位就是这么放的）；
// 栈上的视图 owned_mem==nullptr 且 native==nullptr，析构是 no-op。
// 禁拷贝：拷一份等于两个对象各自 Release() 同一块内存（MPP 情况下是同一个 MppBuffer
// 被 put 两次，引用计数直接下溢）。
struct FrameBuffer
{
  // 拷贝/赋值没有安全语义：要共享就传 FrameBuffer*（槽位、池块都是这么传的）
  FrameBuffer() = default;
  FrameBuffer(const FrameBuffer &) = delete;
  FrameBuffer &operator=(const FrameBuffer &) = delete;
  // RAII：Allocate() 的内存随对象一起还；BindMemory() 的视图两个字段都为空 → no-op
  ~FrameBuffer() { Release(); }

  // 支持多平面（YUV420P 3个，NV12 2个，BGR 1个）
  uint8_t *data[3] = {nullptr, nullptr, nullptr};
  uint8_t *owned_mem = nullptr;  // Allocate() 拿到的基址（MPP 模式下是 DMA 缓冲的映射地址）
  void *native = nullptr;        // 平台句柄（MPP 模式下是 MppBuffer），与 owned_mem 同生共死
  size_t data_size = 0;
  size_t capacity = 0;

  int width = 0;
  int height = 0;
  PixelFormat fmt = PixelFormat::FMT_UNKNOWN;
  int stride[3] = {0, 0, 0};
  uint64_t timestamp_ms = 0;

  // ==========================================
  // 方法 1：由分配边界提供内存（rk3588 上是 MPP DRM 缓冲）
  // ==========================================
  bool Allocate(int w, int h, PixelFormat format, int align = 1)
  {
    if (!CalcMemoryLayout(w, h, format, align))
    {
      return false;
    }

    // 不再 new[]：交给 pool::FrameMemAlloc()。它返回 CPU 基址，并把平台句柄写进 native
    // （MPP：MppBuffer；堆模式：nullptr）。失败时两者都为空，调用方照旧走失败分支。
    owned_mem = pool::FrameMemAlloc(capacity, &native);
    if (!owned_mem)
    {
      native = nullptr;
      return false;
    }

    // 绑定指针偏移
    BindDataPointers(owned_mem);

    data_size = 0; // 初始状态无有效数据
    return true;
  }
  void Release()
  {
    // 两个字段任一非空就说明内存归本对象管，具体怎么还交给分配边界：
    // MPP 按 native 句柄 put，堆模式按 owned_mem delete[]
    if (owned_mem || native)
    {
      pool::FrameMemFree(owned_mem, native);
      owned_mem = nullptr;
      native = nullptr;
    }
    data[0] = data[1] = data[2] = nullptr;
  }

  // ==========================================
  // 方法 2：绑定外部传入的物理内存
  // ==========================================
  bool BindMemory(uint8_t *base_ptr, int w, int h, PixelFormat format, int align = 1)
  {
    if (!base_ptr)
      return false;
    if (!CalcMemoryLayout(w, h, format, align))
    {
      return false;
    }

    // 绑定指针偏移
    BindDataPointers(base_ptr);
    return true;
  }

private:
  // 提取的公共逻辑：计算尺寸、容量、步长
  bool CalcMemoryLayout(int w, int h, PixelFormat format, int align)
  {
    width = w;
    height = h;
    fmt = format;
    capacity = w * h * 3;

    int stride_w = (w + align - 1) / align * align;
    size_t y_size = 0, u_size = 0, v_size = 0;

    switch (fmt)
    {
    case PixelFormat::FMT_BGR888:
      stride[0] = stride_w * 3;
      y_size = stride_w * h * 3;
      break;

    case PixelFormat::FMT_NV12:
      stride[0] = stride_w;
      stride[1] = stride_w;
      y_size = stride_w * h;
      u_size = stride_w * h / 2;
      break;

    case PixelFormat::FMT_YUV420P:
      stride[0] = stride_w;
      stride[1] = stride_w / 2;
      stride[2] = stride_w / 2;
      y_size = stride_w * h;
      u_size = (stride_w / 2) * (h / 2);
      v_size = (stride_w / 2) * (h / 2);
      break;

    case PixelFormat::FMT_YUV422:
      stride[0] = stride_w;
      stride[1] = stride_w / 2;
      stride[2] = stride_w / 2;
      y_size = stride_w * h;
      u_size = (stride_w / 2) * h;
      v_size = (stride_w / 2) * h;
      break;

    default:
      std::cerr << "[FrameBuffer] Unsupported format! fmt:" << (int)format << std::endl;
      return false;
    }
    return true;
  }

  // 提取的公共逻辑：将 data[3] 指向正确的内存偏移
  void BindDataPointers(uint8_t *base_ptr)
  {
    if (fmt == PixelFormat::FMT_BGR888)
    {
      data[0] = base_ptr;
    }
    else if (fmt == PixelFormat::FMT_NV12)
    {
      data[0] = base_ptr;
      data[1] = base_ptr + stride[0] * height;
    }
    else
    {
      data[0] = base_ptr;
      data[1] = base_ptr + stride[0] * height;
      data[2] = base_ptr + stride[0] * height + stride[1] * (height / 2);
    }
  }
};

#endif