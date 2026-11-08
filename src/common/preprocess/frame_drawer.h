#ifndef EVS_PREPROCESS_FRAME_DRAWER_H
#define EVS_PREPROCESS_FRAME_DRAWER_H

#include <string>

#include "preprocess_define.h"

namespace pre
{

// ==========================================
// 帧上绘图：画框 / 贴图 / 截图
//
// 全部接口都是"往 dst 的已有内存里写"，不分配、不释放帧内存；
// 目标帧格式支持 NV12 / YUV422P / YUV420P / BGR888，写之前必须先判 dst.fmt。
//
// 非线程安全：同一个实例不要被多个线程同时调用。
// 本类没有成员状态，需要多线程时每线程各建一个即可。
// ==========================================
class FrameDrawer
{
public:
  FrameDrawer() {}
  ~FrameDrawer() {}

  FrameDrawer(const FrameDrawer &) = delete;
  FrameDrawer &operator=(const FrameDrawer &) = delete;

  // 画框：filled=false 画边框（按 thickness 内缩），filled=true 实心填充。
  // 矩形越出帧边界时自动裁剪；完全在帧外返回 OK 且不写任何像素。
  // 要点：YUV 格式上画框需要同时写 Y 与对应色度，否则会出现"只有亮度变化"的灰框。
  PreStatus draw_box(FrameBuffer &dst, const BoxStyle &box);

  // 贴图：把 MASK8 / RGBA8888 / BGRA8888 小图叠加到 dst。
  //   - dst_rect.w/h <= 0 时按源图尺寸贴到 (x,y)；
  //   - 需要缩放时按最近邻放大（本函数不做高质量缩放，缩放请走 RGA）；
  //   - blend=true 时按 alpha 混合，blend=false 时直接覆盖（速度快，适合不透明图标）。
  // 越界部分裁剪，返回值看下面的错误码。
  PreStatus blit(FrameBuffer &dst, const BlitParam &param);

  // 截图（帧深拷贝）：把 src 拷进 dst，dst 可以是同格式，也可以是 BGR888。
  // 前置条件：dst.fmt == out_fmt，且 dst 的内存/宽高已经由调用方准备好。
  // 用途：把当前帧留一份出来交给算法/落盘，避免下一帧复用池内存后被覆盖。
  PreStatus snapshot(const FrameBuffer &src, FrameBuffer &dst, PixelFormat out_fmt);

  // 截图并落盘：写 24bit 未压缩 BMP（无第三方依赖，板端取证最快）。
  // 支持 src.fmt 为 NV12 / YUV420P / YUV422P / BGR888；内部复用 snapshot。
  PreStatus save_snapshot(const FrameBuffer &src, const std::string &bmp_path);

  // RGB -> YUV(BT.601 有限范围) 换算，供"按 RGB 描述颜色"的调用方使用。
  // 允许 r/g/b 为空指针，缺省按 0 处理；不会返回失败。
  static YuvColor rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b);

private:
  // 在 dst 上水平填充 len 个像素（含色度对写入），越界自动裁剪
  PreStatus fill_span(FrameBuffer &dst, int x, int y, int len, const YuvColor &color);

  // 单点 alpha 混合（alpha=255 相当于覆盖），越界直接忽略
  PreStatus blend_pixel(FrameBuffer &dst, int x, int y, const YuvColor &color,
                        uint8_t alpha);

  // 取/写某格式下 (x,y) 处的 Y 与色度（NV12/YUV422P/YUV420P 共用）
  //   y_out：亮度指针；u_out/v_out：该点对应的色度指针（可能被同块多像素共享）
  static bool locate_pixel(FrameBuffer &dst, int x, int y, uint8_t **y_out,
                           uint8_t **u_out, uint8_t **v_out);
};

} // namespace pre

#endif
