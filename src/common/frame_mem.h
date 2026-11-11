#ifndef FRAME_MEM_H
#define FRAME_MEM_H

#include <cstddef>
#include <cstdint>

// ==========================================
// 帧内存的**唯一**分配/释放边界。
//
// FrameBuffer::Allocate() 不再 new[]，Release() 不再 delete[]，两边都只调这里：
//
//   rk3588（CMake 打开 EVS_FRAME_MEM_MPP，见 src/common/CMakeLists.txt）：
//     FrameMemAlloc → mpp_buffer_group_get_internal(MPP_BUFFER_TYPE_DRM) 懒建组
//                   → mpp_buffer_get(group, &buf, bytes)  拿一块 DMA 缓冲
//                   → mpp_buffer_get_ptr(buf)             拿 CPU 可写 VA（库内部 mmap）
//     FrameMemFree  → mpp_buffer_put(buf)   引用计数 -1；缓冲回到组的 unused 链表，
//                                             **不立刻还系统**（MPP 的池化语义）
//     FrameMemDeinit→ mpp_buffer_group_clear + group_put   真正 munmap/释放（FreeMem 末尾调）
//
//   其他平台（x86 等）：退化成 new[] / delete[]，语义与改造前完全一致，
//   这样非板端构建、以及只用到 header 的一次性探针都还能编、还能跑。
//
// 为什么放 cpp 而不是 header：MPP 头文件只允许出现在 rk3588 的编译单元里，
// frame_buffer.h 是所有层（platform / pipeline / common / 测试）都 include 的头，
// 一旦让它 include "rockchip/mpp_buffer.h"，x86 构建和离线探针立刻编不过。
// ==========================================

namespace pool
{
// 分配 bytes 字节，成功返回 CPU 可写的基址。
// native 交出底层句柄（MPP 模式是 MppBuffer；堆模式置 nullptr）。
// 失败返回 nullptr，且 *native 会被置 nullptr。
uint8_t *FrameMemAlloc(size_t bytes, void **native);

// 释放。cpu_ptr 是 Alloc 的返回值，native 是 Alloc 交出的句柄——两个都要传：
// 堆模式按 cpu_ptr delete[]，MPP 模式按 native mpp_buffer_put（cpu_ptr 仅为映射地址）。
void FrameMemFree(uint8_t *cpu_ptr, void *native);

// 当前后端是否 MPP（日志 / 自检 / 测试判定用，不影响分配语义）
bool FrameMemIsMpp();

// MPP 组内当前记账的字节数（堆模式返回 0）。
// 注意：mpp_buffer_put 之后这个值**不会下降**，缓冲只是回落到组的 unused 链表；
// 只有 FrameMemDeinit() 的 group_clear 才会让它归零——这正是要观察的点。
size_t FrameMemUsage();

// 释放缓冲组（MemManager::FreeMem() 在所有块还完之后调用；再 MemInit 会重新建组）。
// 调用前必须确保没有任何线程还在用这些缓冲。
void FrameMemDeinit();
} // namespace pool

#endif // FRAME_MEM_H
