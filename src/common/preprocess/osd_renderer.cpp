#include "osd_renderer.h"

#include <iostream>

namespace pre
{

OsdRenderer::OsdRenderer() {}

OsdRenderer::~OsdRenderer() {}

// ==========================================
// 字形来源切换
// ==========================================
bool OsdRenderer::set_bitmap_font(const std::string &font_path)
{
  if (!bitmap_src_.load(font_path))
    return false;

  glyph_ = &bitmap_src_;
  return true;
}

bool OsdRenderer::set_truetype_font(const std::string &font_path, int pixel_size)
{
  if (!ft_src_.load(font_path, pixel_size))
    return false; // 未启用 freetype 或字体文件有问题，保持原字形来源

  glyph_ = &ft_src_;
  return true;
}

void OsdRenderer::set_glyph_source(IGlyphSource *source) { glyph_ = source; }

bool OsdRenderer::is_ready() const { return glyph_ && glyph_->is_ready(); }

int OsdRenderer::font_height() const { return is_ready() ? glyph_->font_height() : 0; }

// ==========================================
// 文本测量：按 '\n' 分行，取最宽一行
// ==========================================
int OsdRenderer::text_width(const std::string &text) const
{
  if (!is_ready())
    return 0;

  int max_width = 0;
  int line_width = 0;
  size_t pos = 0;

  while (pos < text.size())
  {
    const int cp = utf8_next(text, pos);
    if (cp < 0)
      continue; // 非法字节跳过
    if (cp == '\n')
    {
      if (line_width > max_width)
        max_width = line_width;
      line_width = 0;
      continue;
    }
    if (cp == '\r')
      continue;

    line_width += glyph_->advance(cp);
  }

  return (line_width > max_width) ? line_width : max_width;
}

// ==========================================
// 绘制整段文本
// ==========================================
PreStatus OsdRenderer::draw_text(FrameBuffer &dst, const std::string &text, int x, int y,
                                 const TextStyle &style)
{
  if (!is_ready())
    return PreStatus::ERR_NOT_OPEN;
  if (!dst.data[0] || dst.width <= 0 || dst.height <= 0)
    return PreStatus::ERR_INVALID_ARG;

  const int scale = (style.scale > 0) ? style.scale : 1;
  const int glyph_h = glyph_->font_height() * scale;
  const int pad = (style.bg_padding > 0) ? style.bg_padding : 0;

  int pen_x = x;
  int pen_y = y;

  size_t pos = 0;
  while (pos < text.size())
  {
    const int cp = utf8_next(text, pos);
    if (cp < 0)
      continue; // 非法字节跳过，不中断整段文本

    if (cp == '\n')
    {
      pen_x = x;
      pen_y += glyph_h;
      continue;
    }
    if (cp == '\r')
      continue;

    const int adv = glyph_->advance(cp) * scale;

    // 背景块：逐字铺（相邻字形的块会互相覆盖，因此不会留缝）；
    // 想要"整行一个底条"请用 draw_banner()。
    if (style.draw_bg && adv > 0)
    {
      BoxStyle bg;
      bg.rect = Rect(pen_x - pad, pen_y - pad, adv + pad * 2, glyph_h + pad * 2);
      bg.color = style.bg_color;
      bg.thickness = 0;
      bg.filled = true;

      PreStatus st = drawer_.draw_box(dst, bg);
      if (st != PreStatus::OK)
        return st;
    }

    PreStatus st = draw_glyph(dst, cp, pen_x, pen_y, style);
    if (st != PreStatus::OK)
      return st;

    pen_x += adv;
  }

  return PreStatus::OK;
}

PreStatus OsdRenderer::draw_text(FrameBuffer &dst, const TextItem &item)
{
  return draw_text(dst, item.text, item.x, item.y, item.style);
}

// ==========================================
// 字幕底条 + 文本
// ==========================================
PreStatus OsdRenderer::draw_banner(FrameBuffer &dst, const std::string &text, int x, int y,
                                   const TextStyle &style)
{
  if (!is_ready())
    return PreStatus::ERR_NOT_OPEN;

  const int scale = (style.scale > 0) ? style.scale : 1;
  const int pad = (style.bg_padding > 0) ? style.bg_padding : 0;
  const int w = text_width(text) + pad * 2;
  const int h = glyph_->font_height() * scale + pad * 2;
  if (w <= 0 || h <= 0)
    return PreStatus::ERR_INVALID_ARG;

  BoxStyle bg;
  bg.rect = Rect(x - pad, y - pad, w, h);
  bg.color = style.bg_color;
  bg.thickness = 0;
  bg.filled = true;

  PreStatus st = drawer_.draw_box(dst, bg);
  if (st != PreStatus::OK)
    return st;

  // 底条已经铺好，写字时不要再铺一遍背景
  TextStyle text_style = style;
  text_style.draw_bg = false;
  return draw_text(dst, text, x, y, text_style);
}

// ==========================================
// 单字形绘制：把 8bit 覆盖度位图当掩码贴上去
// ==========================================
PreStatus OsdRenderer::draw_glyph(FrameBuffer &dst, int codepoint, int x, int y,
                                  const TextStyle &style)
{
  if (!is_ready())
    return PreStatus::ERR_NOT_OPEN;

  GlyphBitmap glyph;
  if (!glyph_->get_glyph(codepoint, glyph))
    return PreStatus::OK; // 字体里没有该字形：当空白处理，不算错误

  if (!glyph.pixels || glyph.width <= 0 || glyph.height <= 0)
    return PreStatus::ERR_INVALID_ARG;

  const int scale = (style.scale > 0) ? style.scale : 1;

  BlitParam param;
  param.src = glyph.pixels;
  param.src_width = glyph.width;
  param.src_height = glyph.height;
  param.src_stride = glyph.pitch;
  param.src_fmt = BlitFormat::MASK8;
  param.dst_rect =
      Rect(x + glyph.bearing_x, y + glyph.bearing_y, glyph.width * scale, glyph.height * scale);
  param.color = style.color;
  param.alpha = style.alpha;
  param.blend = true;

  return drawer_.blit(dst, param);
}

// ==========================================
// UTF-8 解码（1~4 字节标准序列；非法序列返回 -1，只前移 1 字节不中断文本）
// ==========================================
int OsdRenderer::utf8_next(const std::string &s, size_t &pos)
{
  if (pos >= s.size())
    return -1;

  const uint8_t b0 = static_cast<uint8_t>(s[pos]);

  if (b0 < 0x80) // 单字节 ASCII
  {
    ++pos;
    return static_cast<int>(b0);
  }

  int extra = 0;
  int cp = 0;
  if ((b0 & 0xE0) == 0xC0)
  {
    extra = 1;
    cp = b0 & 0x1F;
  }
  else if ((b0 & 0xF0) == 0xE0)
  {
    extra = 2;
    cp = b0 & 0x0F;
  }
  else if ((b0 & 0xF8) == 0xF0)
  {
    extra = 3;
    cp = b0 & 0x07;
  }
  else
  {
    ++pos; // 非法起始字节
    return -1;
  }

  if (pos + static_cast<size_t>(extra) >= s.size())
  {
    pos = s.size(); // 截断序列，整段丢弃
    return -1;
  }

  for (int i = 1; i <= extra; ++i)
  {
    const uint8_t bi = static_cast<uint8_t>(s[pos + static_cast<size_t>(i)]);
    if ((bi & 0xC0) != 0x80)
    {
      ++pos; // 后续字节非法：只跳过起始字节
      return -1;
    }
    cp = (cp << 6) | (bi & 0x3F);
  }

  pos += static_cast<size_t>(extra) + 1;
  return cp;
}

} // namespace pre

