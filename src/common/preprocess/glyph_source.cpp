#include "glyph_source.h"

#include <cstring>
#include <fstream>
#include <iostream>

namespace pre
{

namespace
{
// 文件头长度：magic(4) + version(2) + first_char(2) + char_count(2)
//             + glyph_w(2) + glyph_h(2)
const size_t kFontHeaderSize = 14;
const uint16_t kFontVersion = 1;

// 读小端 u16（p 已保证可读 2 字节）
uint16_t read_u16_le(const uint8_t *p)
{
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}
} // namespace

BitmapGlyphSource::BitmapGlyphSource() {}

BitmapGlyphSource::~BitmapGlyphSource() { unload(); }

// ==========================================
// 从文件加载点阵字库（格式见 glyph_source.h 顶部注释）
// ==========================================
bool BitmapGlyphSource::load(const std::string &font_path)
{
  unload();
  if (font_path.empty())
    return false;

  std::ifstream ifs(font_path.c_str(), std::ios::binary);
  if (!ifs.is_open())
  {
    std::cerr << "[BitmapGlyphSource] 字体文件打不开: " << font_path << std::endl;
    return false;
  }

  // 字库很小（8x16 的 95 字符约 12KB），一次性读进内存
  ifs.seekg(0, std::ios::end);
  const std::streamoff file_size = ifs.tellg();
  ifs.seekg(0, std::ios::beg);
  if (file_size <= 0)
  {
    std::cerr << "[BitmapGlyphSource] 字体文件为空: " << font_path << std::endl;
    return false;
  }

  std::vector<uint8_t> buf(static_cast<size_t>(file_size));
  ifs.read(reinterpret_cast<char *>(buf.data()), file_size);
  if (!ifs)
  {
    std::cerr << "[BitmapGlyphSource] 字体文件读取失败: " << font_path << std::endl;
    return false;
  }

  return parse(buf.data(), buf.size());
}

// ==========================================
// 直接从内存构造（内嵌字库 / 单元测试）
// ==========================================
bool BitmapGlyphSource::load_from_memory(const uint8_t *data, size_t size)
{
  unload();
  return parse(data, size);
}

void BitmapGlyphSource::unload()
{
  data_.clear();
  first_char_ = 0;
  char_count_ = 0;
  glyph_w_ = 0;
  glyph_h_ = 0;
}

// ==========================================
// 解析文件头并做边界校验，成功后把字库整段拷进 data_
// ==========================================
bool BitmapGlyphSource::parse(const uint8_t *data, size_t size)
{
  if (!data || size < kFontHeaderSize)
  {
    std::cerr << "[BitmapGlyphSource] 字体数据过短，不是合法 EVSF 文件" << std::endl;
    return false;
  }
  if (std::memcmp(data, "EVSF", 4) != 0)
  {
    std::cerr << "[BitmapGlyphSource] magic 不是 EVSF" << std::endl;
    return false;
  }

  const uint16_t version = read_u16_le(data + 4);
  if (version != kFontVersion)
  {
    std::cerr << "[BitmapGlyphSource] 不支持的字体版本: " << version << std::endl;
    return false;
  }

  const int first_char = read_u16_le(data + 6);
  const int char_count = read_u16_le(data + 8);
  const int glyph_w = read_u16_le(data + 10);
  const int glyph_h = read_u16_le(data + 12);
  if (char_count <= 0 || glyph_w <= 0 || glyph_h <= 0)
  {
    std::cerr << "[BitmapGlyphSource] 字体头字段非法: count=" << char_count
              << " w=" << glyph_w << " h=" << glyph_h << std::endl;
    return false;
  }

  const size_t glyph_bytes = static_cast<size_t>(glyph_w) * static_cast<size_t>(glyph_h);
  const size_t need = kFontHeaderSize + glyph_bytes * static_cast<size_t>(char_count);
  if (size < need)
  {
    std::cerr << "[BitmapGlyphSource] 字体数据不完整: 需要 " << need
              << " 字节，实际 " << size << std::endl;
    return false;
  }

  data_.assign(data, data + need);
  first_char_ = first_char;
  char_count_ = char_count;
  glyph_w_ = glyph_w;
  glyph_h_ = glyph_h;

  std::cout << "[BitmapGlyphSource] 点阵字库已加载: " << glyph_w_ << "x" << glyph_h_
            << "，字符范围 " << first_char_ << "~" << (first_char_ + char_count_ - 1)
            << std::endl;
  return true;
}

// ==========================================
// 取某字符的图像数据起始地址
// ==========================================
const uint8_t *BitmapGlyphSource::glyph_ptr(int codepoint) const
{
  if (data_.empty() || codepoint < first_char_)
    return nullptr;

  const int index = codepoint - first_char_;
  if (index >= char_count_)
    return nullptr;

  const size_t glyph_bytes = static_cast<size_t>(glyph_w_) * static_cast<size_t>(glyph_h_);
  const size_t offset = kFontHeaderSize + glyph_bytes * static_cast<size_t>(index);
  if (offset + glyph_bytes > data_.size())
    return nullptr;

  return data_.data() + offset;
}

bool BitmapGlyphSource::is_ready() const { return !data_.empty(); }

int BitmapGlyphSource::font_height() const { return glyph_h_; }

// 等宽点阵：步进 = 字宽（如需字距，在这里统一加常数即可）
int BitmapGlyphSource::advance(int codepoint) const
{
  if (glyph_w_ <= 0)
    return 0;
  (void)codepoint; // 等宽字库，与字符无关
  return glyph_w_;
}

bool BitmapGlyphSource::get_glyph(int codepoint, GlyphBitmap &out)
{
  const uint8_t *p = glyph_ptr(codepoint);
  if (!p)
    return false;

  out.pixels = p;
  out.width = glyph_w_;
  out.height = glyph_h_;
  out.pitch = glyph_w_; // 8bit 覆盖度，每像素 1 字节，无行尾补齐
  out.bearing_x = 0;
  out.bearing_y = 0;    // 点阵字：字形左上角对齐笔位置
  return true;
}

} // namespace pre
