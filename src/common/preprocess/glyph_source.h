#ifndef EVS_PREPROCESS_GLYPH_SOURCE_H
#define EVS_PREPROCESS_GLYPH_SOURCE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pre
{

// ==========================================
// 字形位图：8bit 覆盖度（0=全透明 255=不透明）
// pixels 指向的缓冲由字形来源持有，只在"下一次取字形之前"有效，
// 需要长期保存必须先拷走（OsdRenderer 内部是即时用完即弃）。
// ==========================================
struct GlyphBitmap
{
  const uint8_t *pixels = nullptr; // 只读
  int width = 0;
  int height = 0;
  int pitch = 0;                   // 每行字节数
  int bearing_x = 0;               // 相对当前笔位置的水平偏移（像素，右正）
  int bearing_y = 0;               // 相对基线向上为负（BMP 惯例），本框架统一"向下为正"
};

// ==========================================
// 字形来源抽象：让 OsdRenderer 不关心字形从哪来
// （内置点阵 / FreeType 矢量字，二者可互换）
// ==========================================
class IGlyphSource
{
public:
  virtual ~IGlyphSource() {}

  // 是否已成功加载字体（false 时 OsdRenderer::draw_text 返回 ERR_DEPENDENCY）
  virtual bool is_ready() const = 0;

  // 行高（像素）：等宽点阵即字符高；矢量字取 ascent - descent + linegap
  virtual int font_height() const = 0;

  // 该 codepoint 的步进宽度（像素）：没有字形时返回空白宽度，不返回负数
  virtual int advance(int codepoint) const = 0;

  // 取字形位图；字体里没有该字符返回 false（调用方按空白前进）
  virtual bool get_glyph(int codepoint, GlyphBitmap &out) = 0;

  // 来源名，只用于日志（"bitmap" / "freetype"）
  virtual const char *name() const = 0;
};

// ==========================================
// 内置点阵字体（零第三方依赖，板端兜底方案）
//
// 字体文件格式（小端，"EVSF" = EdgeVision Simple Font）：
//   偏移  长度  含义
//   0     4     magic "EVSF"
//   4     2     version，当前为 1
//   6     2     first_char，字库首个字符的 ASCII 码
//   8     2     char_count，字符个数
//   10    2     glyph_w，每字符宽（像素）
//   12    2     glyph_h，每字符高（像素）
//   14    ...   char_count * (glyph_w * glyph_h) 字节，按字符顺序排列，
//               每字符是 glyph_w*glyph_h 个 8bit 覆盖度值（行优先，无补齐）
//
// 说明：选 8bit 覆盖度而不是 1bpp，是为了直接对接 FrameDrawer::blit(MASK8)，
// 免掉逐帧解包；代价是文件略大（8x16 的 95 个 ASCII = 12160 字节）。
// 需要中文时先确认 char_count/first_char 这套 ASCII 假设足够，否则应改用 FreeType。
// ==========================================
class BitmapGlyphSource : public IGlyphSource
{
public:
  BitmapGlyphSource();
  ~BitmapGlyphSource() override;

  // 从点阵字体文件加载；失败返回 false，并保持 is_ready() == false
  bool load(const std::string &font_path);

  // 直接用内存里的完整字体数据构造（内嵌字库、单元测试用；内部会拷一份）
  bool load_from_memory(const uint8_t *data, size_t size);

  // 释放字库数据（之后 is_ready() 变 false）
  void unload();

  bool is_ready() const override;
  int font_height() const override;
  int advance(int codepoint) const override;
  bool get_glyph(int codepoint, GlyphBitmap &out) override;
  const char *name() const override { return "bitmap"; }

private:
  // 解析并校验文件头，成功则把整个字库拷进 data_
  bool parse(const uint8_t *data, size_t size);

  // 取某字符的数据起始地址（越界返回 nullptr）
  const uint8_t *glyph_ptr(int codepoint) const;

private:
  std::vector<uint8_t> data_; // 完整字体文件内容（含文件头）
  int first_char_ = 0;
  int char_count_ = 0;
  int glyph_w_ = 0;
  int glyph_h_ = 0;
};

} // namespace pre

#endif
