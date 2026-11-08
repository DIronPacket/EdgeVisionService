#ifndef EVS_PREPROCESS_OSD_RENDERER_H
#define EVS_PREPROCESS_OSD_RENDERER_H

#include <string>

#include "frame_drawer.h"
#include "freetype_glyph_source.h"
#include "glyph_source.h"
#include "preprocess_define.h"

namespace pre
{

// ==========================================
// OSD 字符叠加（在帧上写字）
//
// 用法：
//   pre::OsdRenderer osd;
//   osd.set_bitmap_font("/path/font16.evsfont");        // 零依赖点阵
//   或 osd.set_truetype_font("/path/xx.ttf", 24);        // 需 BUILD_FREETYPE_OSD=ON
//   osd.draw_text(frame, "CAM0 2026-11-08", 20, 20, style);
//
// 两种字形来源可互换：画字统一走 FrameDrawer::blit(MASK8)，
// 所以只要求字形来源能给出 8bit 覆盖度位图，与目标帧格式（NV12/YUV422P/YUV420P/BGR888）无关。
//
// 非线程安全：一个实例只在一个节点线程里用。
// ==========================================
class OsdRenderer
{
public:
  OsdRenderer();
  ~OsdRenderer();

  OsdRenderer(const OsdRenderer &) = delete;
  OsdRenderer &operator=(const OsdRenderer &) = delete;

  // 使用内置点阵字体（.evsfont，文件格式见 glyph_source.h 注释）。
  // 成功返回 true；失败保持原字形来源不变。
  bool set_bitmap_font(const std::string &font_path);

  // 使用 FreeType 矢量字体（ttf/otf）。
  // 未启用 BUILD_FREETYPE_OSD 时返回 false 并打印一条告警，字形来源保持不变。
  bool set_truetype_font(const std::string &font_path, int pixel_size);

  // 换用外部字形来源（例如测试用的假字库）。只借用、不接管所有权，
  // 调用方必须保证 source 在本对象使用期间有效；传 nullptr 表示暂无字体。
  void set_glyph_source(IGlyphSource *source);

  // 字体是否可用（不可用时 draw_text 返回 ERR_NOT_OPEN）
  bool is_ready() const;

  // 当前字体行高（像素）；未加载字体返回 0
  int font_height() const;

  // 文本宽度（像素）：取最长一行的宽度，遇到 '\n' 换行重新计
  int text_width(const std::string &text) const;

  // 在 dst 上绘制一段 UTF-8 文本。
  //   (x,y) 是文本块的左上角（不是基线），与 TextItem 的坐标语义一致；
  //   支持 '\n' 换行；越出帧边界自动裁剪（FrameDrawer 负责）；
  //   style.draw_bg = true 时逐字铺背景块（相邻字形之间不会留缝）。
  // 返回：字体不可用 ERR_NOT_OPEN；dst 非法 ERR_INVALID_ARG；底层绘图错误原样透传。
  PreStatus draw_text(FrameBuffer &dst, const std::string &text, int x, int y,
                      const TextStyle &style);

  // 便捷重载：直接用 TextItem 里的 text / x / y / style
  PreStatus draw_text(FrameBuffer &dst, const TextItem &item);

  // 画一条字幕底条 + 文本（先铺实心背景块再写字），常用于底部时间戳/通道名。
  // style.bg_padding 决定背景块外扩量，style.bg_color 决定底条颜色。
  PreStatus draw_banner(FrameBuffer &dst, const std::string &text, int x, int y,
                        const TextStyle &style);

private:
  // 画单个字形：把字形位图当作 8bit 掩码贴到 (x,y)，缩放用 nearest（style.scale）
  PreStatus draw_glyph(FrameBuffer &dst, int codepoint, int x, int y,
                       const TextStyle &style);

  // UTF-8 解码：从 s[pos] 解一个 codepoint 并让 pos 前移相应字节数；
  // 非法字节返回 -1 且 pos 只前移 1 字节（调用方跳过即可，不中断整段文本）
  static int utf8_next(const std::string &s, size_t &pos);

private:
  BitmapGlyphSource bitmap_src_;   // 内置点阵（成员持有，不 new）
  FreeTypeGlyphSource ft_src_;     // 矢量字（未启用编译时 load 一律失败）
  IGlyphSource *glyph_ = nullptr;  // 当前生效的字形来源（指向上面两个之一或外部）
  FrameDrawer drawer_;             // 复用贴图/画框能力
};

} // namespace pre

#endif
