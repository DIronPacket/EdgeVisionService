#ifndef EVS_PREPROCESS_FREETYPE_GLYPH_SOURCE_H
#define EVS_PREPROCESS_FREETYPE_GLYPH_SOURCE_H

#include <string>
#include <vector>

#include "glyph_source.h"

namespace pre
{

// ==========================================
// FreeType 矢量字体字形来源（可选编译）
//
// 编译开关：顶层 CMakeLists.txt 的 BUILD_FREETYPE_OSD（默认 OFF）。
//   - OFF（默认）：本类照常参与编译，但 load() 一律返回 false，
//     保证默认构建不依赖 freetype（install/ 里目前没有 freetype）。
//   - ON：需要先准备 freetype 头文件与库，`cmake -DBUILD_FREETYPE_OSD=ON` 后
//     由 find_package(Freetype) 找依赖，并给 common_api 打上 EVS_ENABLE_FREETYPE 宏。
//
// 头文件里不出现任何 freetype 类型（用 Impl 隐藏实现），
// 这样调用方与 OsdRenderer 都不需要 include FT_*.h（HANDOFF §2.7）。
// ==========================================
class FreeTypeGlyphSource : public IGlyphSource
{
public:
  FreeTypeGlyphSource();
  ~FreeTypeGlyphSource() override;

  // 编译期是否带上了 freetype；false 表示当前二进制不具备矢量字体能力
  static bool is_build_enabled();

  // 加载 ttf/otf 字体并设置字号（像素）。失败返回 false（含未编译 freetype 的情况）
  bool load(const std::string &font_path, int pixel_size);

  // 释放字体（之后 is_ready() 变 false）
  void unload();

  bool is_ready() const override;
  int font_height() const override;
  int advance(int codepoint) const override;
  bool get_glyph(int codepoint, GlyphBitmap &out) override;
  const char *name() const override { return "freetype"; }

private:
  // 隐藏 FT_Library / FT_Face：定义在 .cpp 内，未启用编译时是空结构
  struct Impl;

  Impl *impl_;
  std::vector<uint8_t> glyph_buf_; // 最近一次字形的 8bit 覆盖度位图
  int pixel_size_ = 0;
};

} // namespace pre

#endif
