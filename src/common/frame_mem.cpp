#include "frame_mem.h"

#include <cstdio>
#include <mutex>
#include <new>

#if defined(EVS_FRAME_MEM_MPP)
#include "rockchip/mpp_buffer.h"
#include "rockchip/rk_mpi.h"
#endif

namespace pool
{
namespace
{
// mpp_buffer_get / mpp_buffer_group_get 内部虽然有自己的锁，但「懒建组」这一段
// 必须自己串起来：MemInit 里是单线程，但 VpssNode 会在运行期给槽位新建输出帧
// （moudle_vpss.cpp ensure_dst_frame → Allocate），跟 FreeMem 的收尾可能撞上。
std::mutex g_mem_mutex;

#if defined(EVS_FRAME_MEM_MPP)
MppBufferGroup g_group = nullptr;

// 懒建组：整个池子共用一个内部 DRM 组，统一记账、统一回收。
// 用 DRM 而不是 NORMAL：NORMAL 是给单测用的 malloc 缓冲，VPU 拿不到物理地址；
// DRM 组里的缓冲既有 CPU VA（mmap）又能直接交给编码器当输入。
MppBufferGroup ensureGroupLocked()
{
  if (g_group)
    return g_group;

  MPP_RET ret = mpp_buffer_group_get_internal(&g_group, MPP_BUFFER_TYPE_DRM);
  if (ret != MPP_OK || !g_group)
  {
    std::fprintf(stderr, "[FrameMem] mpp_buffer_group_get_internal(DRM) failed ret=%d\n", ret);
    g_group = nullptr;
    return nullptr;
  }

  // 0/0 = 单块大小不限额、数量不限额（mpp_buffer.h:341-345）。
  // 显式写出来是为了别让人照搬 BUFFER_GROUP_SIZE_DEFAULT(80MB) 当总量上限——
  // 池子 50 块 × 1080p NV12 ≈ 311MB，按 80MB 限的话第 13 块 mpp_buffer_get 就失败。
  mpp_buffer_group_limit_config(g_group, 0, 0);

  std::printf("[FrameMem] backend: MPP DRM buffer group (mpp_buffer_get + mmap)\n");
  return g_group;
}
#else
// 非 MPP 平台：沿用改造前的堆分配，行为与 P0-9/P0-10 修复后一致
bool g_backend_logged = false;

void logHeapBackendOnce()
{
  if (g_backend_logged)
    return;
  g_backend_logged = true;
  std::printf("[FrameMem] backend: heap new[]/delete[] (EVS_FRAME_MEM_MPP off)\n");
}
#endif
} // namespace

bool FrameMemIsMpp()
{
#if defined(EVS_FRAME_MEM_MPP)
  return true;
#else
  return false;
#endif
}

uint8_t *FrameMemAlloc(size_t bytes, void **native)
{
  if (native)
    *native = nullptr;
  if (bytes == 0)
    return nullptr;

#if defined(EVS_FRAME_MEM_MPP)
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  MppBufferGroup group = ensureGroupLocked();
  if (!group)
    return nullptr;

  MppBuffer buf = nullptr;
  MPP_RET ret = mpp_buffer_get(group, &buf, bytes);
  if (ret != MPP_OK || !buf)
  {
    std::fprintf(stderr, "[FrameMem] mpp_buffer_get(%zu bytes) failed ret=%d\n", bytes, ret);
    return nullptr;
  }

  // 第一次取 ptr 时库内部做 mmap（mpp_buffer_impl.c mpp_buffer_mmap），之后是纯指针运算。
  // DRM 缓冲默认不是 cacheable 映射，CPU 写完 VPU 直接可见，不需要额外的 cache 维护。
  void *ptr = mpp_buffer_get_ptr(buf);
  if (!ptr)
  {
    std::fprintf(stderr, "[FrameMem] mpp_buffer_get_ptr failed\n");
    mpp_buffer_put(buf);
    return nullptr;
  }

  if (native)
    *native = buf;
  return static_cast<uint8_t *>(ptr);
#else
  logHeapBackendOnce();
  return new (std::nothrow) uint8_t[bytes];
#endif
}

void FrameMemFree(uint8_t *cpu_ptr, void *native)
{
#if defined(EVS_FRAME_MEM_MPP)
  (void)cpu_ptr; // 映射由 MPP 自己维护，只管把句柄还回去
  if (!native)
    return;
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  mpp_buffer_put(static_cast<MppBuffer>(native));
#else
  (void)native;
  delete[] cpu_ptr;
#endif
}

size_t FrameMemUsage()
{
#if defined(EVS_FRAME_MEM_MPP)
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  return g_group ? mpp_buffer_group_usage(g_group) : 0;
#else
  return 0;
#endif
}

void FrameMemDeinit()
{
#if defined(EVS_FRAME_MEM_MPP)
  std::lock_guard<std::mutex> lock(g_mem_mutex);
  if (!g_group)
    return;
  // put 只是把缓冲挂回组的 unused 链表，DMA 内存仍然占着；
  // 先 clear（把组里所有缓冲真正释放）再 group_put（放掉组自身的引用），内存才回到系统。
  mpp_buffer_group_clear(g_group);
  mpp_buffer_group_put(g_group);
  g_group = nullptr;
#endif
}
} // namespace pool
