#include "frame_drawer.h"

#include <cstdio>

namespace pre
{

// ==========================================
// RGB -> YUV(BT.601 有限范围)
//   Y =  0.257R + 0.504G + 0.098B + 16
//   U = -0.148R - 0.291G + 0.439B + 128
//   V =  0.439R - 0.368G - 0.071B + 128
// 用于"调用方按 RGB 描述颜色"的场景；帧内已有 YUV 时直接填 YuvColor 更省事。
// ==========================================
YuvColor FrameDrawer::rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b)
{
  const float fr = static_cast<float>(r);
  const float fg = static_cast<float>(g);
  const float fb = static_cast<float>(b);

  float fy = 0.257f * fr + 0.504f * fg + 0.098f * fb + 16.0f;
  float fu = -0.148f * fr - 0.291f * fg + 0.439f * fb + 128.0f;
  float fv = 0.439f * fr - 0.368f * fg - 0.071f * fb + 128.0f;

  // 截断到 0~255，避免溢出回绕出现花点
  if (fy < 0.0f) fy = 0.0f;
  if (fy > 255.0f) fy = 255.0f;
  if (fu < 0.0f) fu = 0.0f;
  if (fu > 255.0f) fu = 255.0f;
  if (fv < 0.0f) fv = 0.0f;
  if (fv > 255.0f) fv = 255.0f;

  return YuvColor(static_cast<uint8_t>(fy + 0.5f), static_cast<uint8_t>(fu + 0.5f),
                  static_cast<uint8_t>(fv + 0.5f));
}

// ==========================================
// 定位 (x,y) 处的 Y 与色度指针
//
// 色度共享规则（写实现时必须和这里保持一致）：
//   NV12    ：2x2 像素共享一对 UV（交错存放，u=uv[0] v=uv[1]）
//   YUV420P ：2x2 像素共享，U、V 各自独立平面
//   YUV422P ：2x1 像素共享（水平 1/2，垂直不共享）
//   BGR888  ：没有独立色度平面，返回 false，调用方需按 3 字节/像素单独处理
//
// 已知坑：FrameBuffer::BindDataPointers 对 FMT_YUV422 的 data[2] 偏移用的是
// YUV420P 的算法，比真实 U 平面小一半；接真实 YUV422 数据前先确认上游布局。
// ==========================================
bool FrameDrawer::locate_pixel(FrameBuffer &dst, int x, int y, uint8_t **y_out,
                               uint8_t **u_out, uint8_t **v_out)
{
  if (!y_out || !dst.data[0])
    return false;
  *y_out = nullptr;
  if (u_out)
    *u_out = nullptr;
  if (v_out)
    *v_out = nullptr;

  if (x < 0 || y < 0 || x >= dst.width || y >= dst.height)
    return false;
  if (dst.stride[0] < dst.width)
    return false;

  switch (dst.fmt)
  {
  case PixelFormat::FMT_NV12:
  {
    if (!dst.data[1])
      return false;
    *y_out = dst.data[0] + static_cast<size_t>(y) * dst.stride[0] + x;
    const int cx = x & ~1; // 偶数对齐到色度对起点
    const int cy = y & ~1;
    uint8_t *uv = dst.data[1] + static_cast<size_t>(cy / 2) * dst.stride[1] + cx;
    if (u_out)
      *u_out = uv;
    if (v_out)
      *v_out = uv + 1;
    return true;
  }

  case PixelFormat::FMT_YUV420P:
  {
    if (!dst.data[1] || !dst.data[2])
      return false;
    *y_out = dst.data[0] + static_cast<size_t>(y) * dst.stride[0] + x;
    const int cx = x / 2;
    const int cy = y / 2;
    if (u_out)
      *u_out = dst.data[1] + static_cast<size_t>(cy) * dst.stride[1] + cx;
    if (v_out)
      *v_out = dst.data[2] + static_cast<size_t>(cy) * dst.stride[2] + cx;
    return true;
  }

  case PixelFormat::FMT_YUV422:
  {
    if (!dst.data[1] || !dst.data[2])
      return false;
    *y_out = dst.data[0] + static_cast<size_t>(y) * dst.stride[0] + x;
    const int cx = x / 2;
    if (u_out)
      *u_out = dst.data[1] + static_cast<size_t>(y) * dst.stride[1] + cx;
    if (v_out)
      *v_out = dst.data[2] + static_cast<size_t>(y) * dst.stride[2] + cx;
    return true;
  }

  default:
    return false; // BGR888 无独立色度平面
  }
}

// ==========================================
// 以下为实现位（框架占位，返回 ERR_NOT_IMPL）
//
// 统一要求：
//   - 越界一律裁剪而不是报错（字幕/画框贴边很常见）；
//   - 写 YUV 时必须同时写对应色度，否则画出来的框只有亮度变化、看着发灰；
//   - BGR888 目标要单独走"3 字节/像素 + RGB 直写"分支，不走 locate_pixel；
//   - 逐帧路径不要 printf。
// ==========================================

PreStatus FrameDrawer::draw_box(FrameBuffer &dst, const BoxStyle &box)
{
  (void)dst;
  (void)box;
  // TODO:
  //   1) 先按帧尺寸裁剪 box.rect，裁剪后为空直接返回 OK；
  //   2) filled=true：rect 内逐行 fill_span()；
  //   3) filled=false：画上/下两条 thickness 高的横线 + 左/右两条 thickness 宽的竖线；
  //      thickness 大于 rect 一半时按实心处理，避免线宽互相覆盖成空心；
  //   4) BGR888 分支按字节直写；
  //   5) 全部画完把 dst.data_size 补上（若上游没填）。
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameDrawer::blit(FrameBuffer &dst, const BlitParam &param)
{
  (void)dst;
  (void)param;
  // TODO:
  //   1) 校验 param.src 非空、src_width/height 为正；src_stride 为 0 时按
  //      src_width * 每像素字节数（MASK8=1，RGBA/BGRA=4）自动计算；
  //   2) 目标区域：param.dst_rect.w/h<=0 时取源尺寸；再与帧尺寸求交集；
  //   3) 缩放：仅支持整数最近邻（sx = x * src_w / dst_w），非整数比例也不做插值；
  //   4) MASK8：mask 值 × param.alpha / 255 作为混合系数，前景色用 param.color；
  //      RGBA/BGRA：取源 alpha × param.alpha / 255，RGB 经 rgb_to_yuv 换算成前景色；
  //   5) blend=false 时 alpha>=128 直接覆盖、否则跳过（速度优先的硬边图标）；
  //   6) 用 blend_pixel() 完成单点混合，避免每个格式分支重复写色度逻辑。
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameDrawer::snapshot(const FrameBuffer &src, FrameBuffer &dst, PixelFormat out_fmt)
{
  (void)src;
  (void)dst;
  (void)out_fmt;
  // TODO:
  //   1) 校验 dst.data[0] 非空、dst.fmt == out_fmt、dst 宽高 == src 宽高；
  //   2) out_fmt == src.fmt：逐平面逐行 memcpy（各自 stride 可能不同）；
  //   3) 否则复用 FrameConverter::convert() 做格式转换（注意它当前也是占位实现）；
  //   4) 成功后对齐 dst.timestamp_ms = src.timestamp_ms、dst.data_size；
  //   5) 本函数不分配内存：dst 的 buffer 由调用方（一般是内存池另一块）准备。
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameDrawer::save_snapshot(const FrameBuffer &src, const std::string &bmp_path)
{
  (void)src;
  (void)bmp_path;
  // TODO:
  //   1) 内部准备一个 BGR888 的临时 FrameBuffer（Allocate 或栈上 vector），
  //      调 snapshot(src, tmp, PixelFormat::FMT_BGR888) 得到 BGR 数据；
  //   2) 写 24bit BMP：14 字节文件头 + 40 字节信息头，像素数据按行倒序、
  //      每行按 4 字节对齐补齐（bfOffBits / biSizeImage 要按补齐后算）；
  //   3) 用 fopen/fwrite，失败返回 ERR_IO 并关闭句柄；
  //   4) 落盘命名建议带时间戳，由调用方拼路径（本函数不管命名）。
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameDrawer::fill_span(FrameBuffer &dst, int x, int y, int len,
                                 const YuvColor &color)
{
  (void)dst;
  (void)x;
  (void)y;
  (void)len;
  (void)color;
  // TODO: 在 y 行从 x 开始填 len 个像素（先裁剪到 [0, width)）；
  //   Y 平面逐点写 color.y；色度按目标格式的共享规则写（见 locate_pixel 注释），
  //   同一色度对被多次写时要保证写入值一致，否则会出现相邻像素色块跳变。
  return PreStatus::ERR_NOT_IMPL;
}

PreStatus FrameDrawer::blend_pixel(FrameBuffer &dst, int x, int y,
                                   const YuvColor &color, uint8_t alpha)
{
  (void)dst;
  (void)x;
  (void)y;
  (void)color;
  (void)alpha;
  // TODO: 公式 fg*a + bg*(255-a)，按 255 归一（用 (x*a + y*(255-a) + 127)/255 避免浮点）；
  //   alpha>=255 时直接覆盖（省掉一次读）；越界或 locate_pixel 返回 false 时静默跳过。
  return PreStatus::ERR_NOT_IMPL;
}

} // namespace pre

