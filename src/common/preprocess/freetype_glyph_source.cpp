#include "freetype_glyph_source.h"

#include <iostream>

// ==========================================
// FreeType 实现位
//
// 未定义 EVS_ENABLE_FREETYPE（默认）时不会 include 任何 freetype 头文件，
// 全部接口退化为"不可用"，保证 install/ 里没有 freetype 的机器也能正常构建。
// ==========================================

#ifdef EVS_ENABLE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace pre
{

// Impl 的定义放在 .cpp：头文件里只留前置声明，调用方看不到 FT_* 类型。
// 启用 freetype 时在这里持有 FT_Library / FT_Face；未启用时是个空结构。
struct FreeTypeGlyphSource::Impl
{
#ifdef EVS_ENABLE_FREETYPE
  FT_Library library;
  FT_Face face;

  Impl() : library(nullptr), face(nullptr) {}
#else
  Impl() {}
#endif
};

FreeTypeGlyphSource::FreeTypeGlyphSource() : impl_(new Impl()) {}

FreeTypeGlyphSource::~FreeTypeGlyphSource()
{
  unload();
  delete impl_;
  impl_ = nullptr;
}

bool FreeTypeGlyphSource::is_build_enabled()
{
#ifdef EVS_ENABLE_FREETYPE
  return true;
#else
  return false;
#endif
}

// ==========================================
// 加载 ttf/otf 并设置像素字号
// ==========================================
bool FreeTypeGlyphSource::load(const std::string &font_path, int pixel_size)
{
  unload();

#ifndef EVS_ENABLE_FREETYPE
  (void)font_path;
  (void)pixel_size;
  // 明确告警而不是静默失败：调用方一眼能看出是"没编 freetype"还是"字体有问题"
  std::cerr << "[FreeTypeGlyphSource] 当前构建未启用 FreeType（BUILD_FREETYPE_OSD=OFF），"
               "无法加载矢量字体"
            << std::endl;
  return false;
#else
  (void)pixel_size;
  // TODO（实现顺序，FT 2.x）：
  //   1) if (!font_path 非空 && pixel_size > 0) 参数校验；
  //   2) FT_Init_FreeType(&impl_->library) 失败返回 false；
  //   3) FT_New_Face(impl_->library, font_path.c_str(), 0, &impl_->face) 失败要
  //      FT_Done_FreeType 回滚；
  //   4) FT_Set_Pixel_Sizes(impl_->face, 0, pixel_size) 设置字号；
  //   5) pixel_size_ = pixel_size；失败路径统一走 unload() 释放已建资源；
  //   6) 逐帧不要打印日志，只有加载成功/失败各一条。
  (void)font_path;
  return false;
#endif
}

void FreeTypeGlyphSource::unload()
{
#ifdef EVS_ENABLE_FREETYPE
  if (impl_)
  {
    // 顺序不能反：先放 face 再放 library
    if (impl_->face)
    {
      // FT_Done_Face(impl_->face);
      impl_->face = nullptr;
    }
    if (impl_->library)
    {
      // FT_Done_FreeType(impl_->library);
      impl_->library = nullptr;
    }
  }
#endif
  glyph_buf_.clear();
  pixel_size_ = 0;
}

bool FreeTypeGlyphSource::is_ready() const
{
#ifdef EVS_ENABLE_FREETYPE
  (void)impl_;
  return false; // TODO: return impl_ && impl_->face != nullptr;
#else
  return false;
#endif
}

int FreeTypeGlyphSource::font_height() const
{
  // TODO: 返回 (face->size->metrics.ascender - face->size->metrics.descender) >> 6，
  //       或直接用 face->height >> 6（含行距），二选一口径要和 advance 一致。
  return 0;
}

int FreeTypeGlyphSource::advance(int codepoint) const
{
  (void)codepoint;
  // TODO:
  //   1) FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT) 失败则退回空白宽度；
  //   2) 返回 face->glyph->advance.x >> 6（26.6 定点转像素）。
  return 0;
}

bool FreeTypeGlyphSource::get_glyph(int codepoint, GlyphBitmap &out)
{
  (void)codepoint;
  (void)out;
  // TODO:
  //   1) FT_Load_Char(face, codepoint, FT_LOAD_RENDER) 渲染成 8bit 灰度位图；
  //   2) FT_GlyphSlot slot = face->glyph; 取 slot->bitmap（buffer/width/rows/pitch）；
  //   3) 把位图拷进 glyph_buf_（width*rows 字节），out.pixels 指向 glyph_buf_.data()，
  //      注意 bitmap.pitch 可能为负（自顶向下），拷贝时要按行序处理；
  //   4) out.width = bitmap.width, out.height = bitmap.rows, out.pitch = bitmap.width；
  //      bearing 由 slot->bitmap_left / bitmap_top 换算（本框架约定"向下为正"）。
  return false;
}

} // namespace pre
