#ifndef EVS_PREPROCESS_DEFINE_H
#define EVS_PREPROCESS_DEFINE_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "frame_buffer.h"

// ==========================================
// 预处理模块公共定义（格式转换 / 绘图 / OSD 共用）
//
// 目录：src/common/preprocess/    命名空间：pre
// 三条约定（改实现时不要破坏）：
//   1. 不用异常，统一用 PreStatus 返回码，与工程现有"返回码 + 空指针"约定一致；
//   2. 只往已有内存里写，不拥有也不释放 FrameBuffer 里的内存（HANDOFF §2.4-5）；
//   3. 目标帧的内存与布局由调用方准备（FrameBuffer::Allocate 或 BindMemory），
//      本模块不替调用方分配帧内存，避免出现第二套内存管理。
// ==========================================
namespace pre
{

// 预处理返回码
enum class PreStatus
{
  OK = 0,
  ERR_INVALID_ARG,     // 空指针 / 尺寸非法 / 矩形越界 / src与dst宽高不一致
  ERR_UNSUPPORTED_FMT, // 格式不在支持矩阵内（见 FrameConverter::is_supported）
  ERR_NOT_OPEN,        // open() 未调用或已经 close()
  ERR_NO_MEMORY,       // 内部缓冲分配失败
  ERR_DEPENDENCY,      // 依赖缺失：未编译 freetype、字体加载失败
  ERR_IO,              // 文件读写失败（截图落盘、字体文件）
  ERR_NOT_IMPL         // 框架占位：该分支的实现尚未填充（grep ERR_NOT_IMPL 查 TODO）
};

// 返回码转字符串，日志里直接用（实现见 image_preprocessor.cpp）
const char *pre_status_str(PreStatus st);

// ==========================================
// 颜色
// ==========================================

// YUV 直通颜色：NV12 / YUV422P 链路上直接用 YUV 描述颜色，
// 避免"RGB -> YUV"来回转换带来的偏差（叠加物本身就在 YUV 帧里）
struct YuvColor
{
  uint8_t y; // 亮度 0~255；视频范围建议 16(黑) ~ 235(白)
  uint8_t u; // 色度 U(Cb) 0~255；128 为无色
  uint8_t v; // 色度 V(Cr) 0~255；128 为无色

  YuvColor() : y(235), u(128), v(128) {}                        // 默认白
  YuvColor(uint8_t y_, uint8_t u_, uint8_t v_) : y(y_), u(u_), v(v_) {}
};

// RGB 颜色：贴图/图标按 RGB 描述时用，落地前由 FrameDrawer::rgb_to_yuv 转成 YuvColor
struct RgbaColor
{
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t a; // 0=全透明 255=不透明

  RgbaColor() : r(255), g(255), b(255), a(255) {}
  RgbaColor(uint8_t r_, uint8_t g_, uint8_t b_, uint8_t a_ = 255)
      : r(r_), g(g_), b(b_), a(a_) {}
};

// ==========================================
// 几何
// ==========================================

// 矩形（像素坐标，左闭右开：实际覆盖 x ~ x+w-1、y ~ y+h-1）
struct Rect
{
  int x;
  int y;
  int w;
  int h;

  Rect() : x(0), y(0), w(0), h(0) {}
  Rect(int x_, int y_, int w_, int h_) : x(x_), y(y_), w(w_), h(h_) {}

  // 宽或高非正即视为空矩形
  bool empty() const { return w <= 0 || h <= 0; }

  // 是否完全落在 fw x fh 的帧内
  bool inside(int fw, int fh) const
  {
    return x >= 0 && y >= 0 && w > 0 && h > 0 && (x + w) <= fw && (y + h) <= fh;
  }
};

// 一种像素格式的内存布局。
// 用途：配置阶段先算好 dst 需要多大的 buffer，逐帧处理时不再临时分配。
// 字段含义与 FrameBuffer::CalcMemoryLayout 保持一致（stride 按行宽向 align 取整）
struct ImageLayout
{
  int width = 0;
  int height = 0;
  PixelFormat fmt = PixelFormat::FMT_UNKNOWN;
  int stride[3] = {0, 0, 0};
  size_t plane_size[3] = {0, 0, 0};
  size_t total_size = 0; // 三个平面之和，即 dst 至少需要的字节数
  int planes = 0;        // 有效平面数：BGR888=1, NV12=2, YUV420P/YUV422P=3
};

// ==========================================
// 叠加样式
// ==========================================

// 画框参数（边框或实心填充）
struct BoxStyle
{
  Rect rect;                        // 目标矩形（相对帧左上角）
  YuvColor color;                   // 颜色（YUV 直通）
  int thickness = 2;                // 边框线宽（像素）；filled=true 时忽略
  bool filled = false;              // true=实心填充（可做隐私遮挡），false=只画边框
};

// 文字样式
struct TextStyle
{
  YuvColor color;                   // 字形颜色
  YuvColor bg_color = YuvColor(16, 128, 128); // 背景色，仅 draw_bg=true 时生效（默认黑）
  bool draw_bg = false;             // 是否绘制文字背景块（提高暗背景下的可读性）
  uint8_t alpha = 255;              // 整体不透明度：0=全透明 255=不透明
  int bg_padding = 4;               // 背景块相对文字外扩的像素数
  int scale = 1;                    // 整数字形放大倍数（点阵字按最近邻放大，1=原始大小）
};

// 一条静态叠加文本（左上角对齐到 x,y；坐标语义由 OsdRenderer::draw_text 定义）
struct TextItem
{
  std::string text;                 // UTF-8 文本
  int x = 0;
  int y = 0;
  TextStyle style;
};

// 贴图源像素布局
enum class BlitFormat
{
  MASK8 = 0, // 8bit 覆盖度掩码（字形位图 / 单色 logo），前景色取 BlitParam::color
  RGBA8888,  // 每像素 4 字节 R,G,B,A
  BGRA8888   // 每像素 4 字节 B,G,R,A
};

// 贴图参数（把一块小图叠加到帧上）
struct BlitParam
{
  const uint8_t *src = nullptr;   // 源像素（只读，生命周期由调用方保证）
  int src_width = 0;
  int src_height = 0;
  int src_stride = 0;             // 源每行字节数；0 表示按宽度自然紧凑排列
  BlitFormat src_fmt = BlitFormat::MASK8;
  Rect dst_rect;                  // 目标区域；w/h<=0 时按源尺寸贴到 (x,y)
  YuvColor color;                 // MASK8 时的前景色（RGBA 源忽略此字段）
  uint8_t alpha = 255;            // 额外整体透明度（与源自身 alpha 相乘）
  bool blend = true;              // true=alpha 混合；false=直接覆盖（不做混合，最快）
};

} // namespace pre

#endif
