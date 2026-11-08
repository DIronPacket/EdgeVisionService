#ifndef EVS_PREPROCESS_FRAME_CONVERTER_H
#define EVS_PREPROCESS_FRAME_CONVERTER_H

#include "preprocess_define.h"

namespace pre
{

// ==========================================
// 像素格式转换器（纯 CPU，无成员状态，可以每个线程一个实例）
//
// 支持矩阵（未实现的函数一律返回 PreStatus::ERR_NOT_IMPL）：
//     NV12     <->  YUV422P(FMT_YUV422)   4:2:0 <-> 4:2:2
//     NV12     <->  YUV420P               semi-planar <-> planar
//     YUV422P  <->  YUV420P
//     NV12     <->  BGR888                截图 / 调试用
//     YUV420P  ->   BGR888
//     YUV422P  ->   BGR888
//     BGR888   ->   NV12                  贴图回灌用
//
// 改实现前必须知道的两点：
//   1. PixelFormat::FMT_YUV422 在 FrameBuffer 里的布局是 Y/U/V 三平面、
//      u_size=(stride/2)*h，即 **YUV422P（I422）**，不是打包的 YUYV/UYVY。
//      要支持打包格式必须扩展 PixelFormat 枚举（公共头改动，属独立任务）。
//   2. 本类只做格式转换，不改分辨率：src/dst 的 width/height 必须一致；
//      需要缩放走 RGA（`platform` 侧尚未接入）。
//
// 逐帧调用，不要在里面做 printf（HANDOFF §2.3 日志规定）。
// ==========================================
class FrameConverter
{
public:
  FrameConverter() {}
  ~FrameConverter() {}

  FrameConverter(const FrameConverter &) = delete;
  FrameConverter &operator=(const FrameConverter &) = delete;

  // 是否支持 src -> dst（src==dst 也算支持，直接 memcpy 语义）
  static bool is_supported(PixelFormat src, PixelFormat dst);

  // 计算某格式在给定宽高/对齐下的内存布局，不分配内存。
  // align 语义与 FrameBuffer::CalcMemoryLayout 一致：行宽向上取整到 align 的倍数。
  // 返回 false：宽高非正，或格式不支持。
  static bool calc_layout(int width, int height, PixelFormat fmt, int align,
                          ImageLayout &out);

  // 主入口：把 src 转成 dst。
  // 前置条件（不满足返回 ERR_INVALID_ARG）：
  //   - src.data[0] / dst.data[0] 非空；src、dst 的 width/height 都大于 0 且彼此相等；
  //   - dst.fmt 就是目标格式；
  //   - src.stride[0] >= src.width，dst.stride[0] >= dst.width。
  // 说明：src.fmt == dst.fmt 时按行深拷贝（保留各自 stride）。
  PreStatus convert(const FrameBuffer &src, FrameBuffer &dst);

private:
  // 统一的前置校验：宽高一致 + 指针非空 + stride 合法
  static PreStatus check_pair(const FrameBuffer &src, const FrameBuffer &dst);

  // NV12(2平面) -> YUV422P(3平面)
  //   要点：Y 平面按行拷贝（stride 可能不同）；UV 由 NV12 的交错 UV 行拆成
  //   U、V 两个平面，行数不变（420->422 是垂直方向复制一行色度）。
  PreStatus nv12_to_yuv422p(const FrameBuffer &src, FrameBuffer &dst);

  // YUV422P(3平面) -> NV12(2平面)
  //   要点：U、V 两个平面按 2 行合并成 1 行交错 UV（垂直 4:2:2 -> 4:2:0 取平均或丢弃奇数行）。
  PreStatus yuv422p_to_nv12(const FrameBuffer &src, FrameBuffer &dst);

  // NV12 -> YUV420P
  //   要点：只做色度重排（UV 交错 -> U 平面 + V 平面），Y 平面按行拷贝。
  PreStatus nv12_to_yuv420p(const FrameBuffer &src, FrameBuffer &dst);

  // YUV420P -> NV12
  //   要点：色度重排（U 平面 + V 平面 -> UV 交错），是上面那条的逆过程。
  PreStatus yuv420p_to_nv12(const FrameBuffer &src, FrameBuffer &dst);

  // YUV422P -> YUV420P
  //   要点：色度垂直下采样 2:1（两行取平均，或按需只取偶数行）。
  PreStatus yuv422p_to_yuv420p(const FrameBuffer &src, FrameBuffer &dst);

  // YUV420P -> YUV422P
  //   要点：色度垂直上采样 1:2（每行复制成两行）；想更平滑可做 3:1 加权插值。
  PreStatus yuv420p_to_yuv422p(const FrameBuffer &src, FrameBuffer &dst);

  // NV12 -> BGR888
  //   要点：按 BT.601 有限范围做 YUV->RGB（R=1.164*(Y-16)+1.596*(V-128) 等），
  //   色度按 2x2 块取同一 UV；BGR888 每像素 3 字节。
  PreStatus nv12_to_bgr888(const FrameBuffer &src, FrameBuffer &dst);

  // YUV420P -> BGR888
  //   要点：同 NV12->BGR888，只是 U、V 从两个独立平面取。
  PreStatus yuv420p_to_bgr888(const FrameBuffer &src, FrameBuffer &dst);

  // YUV422P -> BGR888
  //   要点：同 NV12->BGR888，色度按 2x1 块取同一 UV（垂直方向不再共享）。
  PreStatus yuv422p_to_bgr888(const FrameBuffer &src, FrameBuffer &dst);

  // BGR888 -> NV12
  //   要点：按 BT.601 有限范围做 RGB->YUV，色度 2x2 块内取平均（或用左上角像素近似）。
  PreStatus bgr888_to_nv12(const FrameBuffer &src, FrameBuffer &dst);
};

} // namespace pre

#endif
