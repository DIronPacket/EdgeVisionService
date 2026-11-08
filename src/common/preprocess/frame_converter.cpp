#include "frame_converter.h"

#include <cstring>

namespace pre
{

// ==========================================
// 能力查询
// ==========================================
bool FrameConverter::is_supported(PixelFormat src, PixelFormat dst)
{
  if (src == PixelFormat::FMT_UNKNOWN || dst == PixelFormat::FMT_UNKNOWN)
    return false;
  if (src == dst)
    return true; // 同格式：按行深拷贝

  const bool s_nv12 = (src == PixelFormat::FMT_NV12);
  const bool s_420p = (src == PixelFormat::FMT_YUV420P);
  const bool s_422p = (src == PixelFormat::FMT_YUV422);
  const bool s_bgr = (src == PixelFormat::FMT_BGR888);

  const bool d_nv12 = (dst == PixelFormat::FMT_NV12);
  const bool d_420p = (dst == PixelFormat::FMT_YUV420P);
  const bool d_422p = (dst == PixelFormat::FMT_YUV422);
  const bool d_bgr = (dst == PixelFormat::FMT_BGR888);

  if (s_nv12 && (d_422p || d_420p || d_bgr))
    return true;
  if (s_420p && (d_nv12 || d_422p || d_bgr))
    return true;
  if (s_422p && (d_nv12 || d_420p || d_bgr))
    return true;
  if (s_bgr && d_nv12)
    return true;

  return false;
}

// ==========================================
// 布局计算：与 FrameBuffer::CalcMemoryLayout 用同一套 stride 口径（align 向上取整）
//
// 注意一处已知不一致（属既有问题，不在本任务修）：
// FrameBuffer::BindDataPointers 里 FMT_YUV422 的第三平面偏移用的是
// stride[1]*(height/2)（那是 YUV420P 的算法），比 YUV422P 的 U 平面真实大小
// stride[1]*height 少一半，data[2] 会指到 U 平面中间。
// 因此本类一律按"YUV422P 正确布局"计算；接真实数据前必须先确认上游是按哪种布局填的。
// ==========================================
bool FrameConverter::calc_layout(int width, int height, PixelFormat fmt, int align,
                                 ImageLayout &out)
{
  if (width <= 0 || height <= 0)
    return false;
  if (align <= 0)
    align = 1;

  out = ImageLayout();
  out.width = width;
  out.height = height;
  out.fmt = fmt;

  const int stride_w = (width + align - 1) / align * align;

  switch (fmt)
  {
  case PixelFormat::FMT_BGR888:
    out.stride[0] = stride_w * 3;
    out.plane_size[0] = static_cast<size_t>(out.stride[0]) * height;
    out.planes = 1;
    break;

  case PixelFormat::FMT_NV12:
    out.stride[0] = stride_w;
    out.stride[1] = stride_w;
    out.plane_size[0] = static_cast<size_t>(stride_w) * height;
    out.plane_size[1] = static_cast<size_t>(stride_w) * (height / 2);
    out.planes = 2;
    break;

  case PixelFormat::FMT_YUV420P:
    out.stride[0] = stride_w;
    out.stride[1] = stride_w / 2;
    out.stride[2] = stride_w / 2;
    out.plane_size[0] = static_cast<size_t>(stride_w) * height;
    out.plane_size[1] = static_cast<size_t>(stride_w / 2) * (height / 2);
    out.plane_size[2] = out.plane_size[1];
    out.planes = 3;
    break;

  case PixelFormat::FMT_YUV422:
    out.stride[0] = stride_w;
    out.stride[1] = stride_w / 2;
    out.stride[2] = stride_w / 2;
    out.plane_size[0] = static_cast<size_t>(stride_w) * height;
    out.plane_size[1] = static_cast<size_t>(stride_w / 2) * height;
    out.plane_size[2] = out.plane_size[1];
    out.planes = 3;
    break;

  default:
    return false;
  }

  out.total_size = 0;
  for (int i = 0; i < out.planes; ++i)
    out.total_size += out.plane_size[i];

  return true;
}

// ==========================================
// 前置校验
// ==========================================
PreStatus FrameConverter::check_pair(const FrameBuffer &src, const FrameBuffer &dst)
{
  if (!src.data[0] || !dst.data[0])
    return PreStatus::ERR_INVALID_ARG;
  if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0)
    return PreStatus::ERR_INVALID_ARG;
  if (src.width != dst.width || src.height != dst.height)
    return PreStatus::ERR_INVALID_ARG; // 本类不改分辨率
  if (src.stride[0] < src.width || dst.stride[0] < dst.width)
    return PreStatus::ERR_INVALID_ARG;
  return PreStatus::OK;
}

// ==========================================
// 主入口：校验 + 分派
// ==========================================
PreStatus FrameConverter::convert(const FrameBuffer &src, FrameBuffer &dst)
{
  PreStatus st = check_pair(src, dst);
  if (st != PreStatus::OK)
    return st;

  if (!is_supported(src.fmt, dst.fmt))
    return PreStatus::ERR_UNSUPPORTED_FMT;

  // 同格式：按平面逐行深拷贝（各自 stride 可能不同，不能用一次 memcpy 走完）
  if (src.fmt == dst.fmt)
  {
    ImageLayout layout;
    if (!calc_layout(src.width, src.height, src.fmt, 1, layout))
      return PreStatus::ERR_UNSUPPORTED_FMT;

    for (int p = 0; p < layout.planes; ++p)
    {
      if (!src.data[p] || !dst.data[p])
        return PreStatus::ERR_INVALID_ARG;

      const size_t stride = static_cast<size_t>(layout.stride[p]);
      const size_t rows = layout.plane_size[p] / stride;
      for (size_t r = 0; r < rows; ++r)
      {
        std::memcpy(dst.data[p] + r * static_cast<size_t>(dst.stride[p]),
                    src.data[p] + r * static_cast<size_t>(src.stride[p]), stride);
      }
    }
    dst.data_size = layout.total_size;
    dst.timestamp_ms = src.timestamp_ms;
    return PreStatus::OK;
  }

  // 分派到具体转换对
  switch (src.fmt)
  {
  case PixelFormat::FMT_NV12:
    if (dst.fmt == PixelFormat::FMT_YUV422)
      return nv12_to_yuv422p(src, dst);
    if (dst.fmt == PixelFormat::FMT_YUV420P)
      return nv12_to_yuv420p(src, dst);
    if (dst.fmt == PixelFormat::FMT_BGR888)
      return nv12_to_bgr888(src, dst);
    break;

  case PixelFormat::FMT_YUV420P:
    if (dst.fmt == PixelFormat::FMT_NV12)
      return yuv420p_to_nv12(src, dst);
    if (dst.fmt == PixelFormat::FMT_YUV422)
      return yuv420p_to_yuv422p(src, dst);
    if (dst.fmt == PixelFormat::FMT_BGR888)
      return yuv420p_to_bgr888(src, dst);
    break;

  case PixelFormat::FMT_YUV422:
    if (dst.fmt == PixelFormat::FMT_NV12)
      return yuv422p_to_nv12(src, dst);
    if (dst.fmt == PixelFormat::FMT_YUV420P)
      return yuv422p_to_yuv420p(src, dst);
    if (dst.fmt == PixelFormat::FMT_BGR888)
      return yuv422p_to_bgr888(src, dst);
    break;

  case PixelFormat::FMT_BGR888:
    if (dst.fmt == PixelFormat::FMT_NV12)
      return bgr888_to_nv12(src, dst);
    break;

  default:
    break;
  }

  return PreStatus::ERR_UNSUPPORTED_FMT;
}

// ==========================================
// 以下为各转换对的实现位（框架占位，统一返回 ERR_NOT_IMPL，像素循环由使用者填充）
//
// 通用实现要点：
//   - 一律"按平面 + 逐行"写，不要假设 stride == width；
//   - 成功后把 dst.data_size 填成 layout.total_size、dst.timestamp_ms 继承 src；
//   - 宽或高为奇数时，最后一行/最后一列的色度对要单独兜底，避免越界写；
//   - 不要在循环里 printf（逐帧日志会拖垮性能，见 P1-14）。
// ==========================================

PreStatus FrameConverter::nv12_to_yuv422p(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: Y 平面逐行拷贝；UV 交错行拆成 U、V 两个平面（色度行数 h/2 -> h）
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::yuv422p_to_nv12(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: Y 平面逐行拷贝；U、V 两行合并成一行交错 UV（色度垂直下采样 2:1）
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::nv12_to_yuv420p(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: 只做色度重排：交错 UV 行 -> U 平面 + V 平面；Y 平面逐行拷贝
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::yuv420p_to_nv12(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: nv12_to_yuv420p 的逆过程：U、V 平面交织成 UV 行
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::yuv422p_to_yuv420p(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: 色度垂直下采样 2:1（两行取平均更平滑，只取偶数行更快），Y 平面直接拷贝
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::yuv420p_to_yuv422p(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: 色度垂直上采样 1:2（每行复制成两行），Y 平面直接拷贝
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::nv12_to_bgr888(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: BT.601 有限范围 YUV->RGB，色度按 2x2 块共享；输出 B,G,R 三字节
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::yuv420p_to_bgr888(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: 同 nv12_to_bgr888，U、V 从两个独立平面取
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::yuv422p_to_bgr888(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: 同 nv12_to_bgr888，色度按 2x1 块共享（每行都有自己的 UV）
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameConverter::bgr888_to_nv12(const FrameBuffer &src, FrameBuffer &dst)
{
  (void)src;
  (void)dst;
  // TODO: BT.601 有限范围 RGB->YUV，色度 2x2 块取平均（或取左上角像素近似）
  return PreStatus::ERR_NOT_IMPL;
}

} // namespace pre
