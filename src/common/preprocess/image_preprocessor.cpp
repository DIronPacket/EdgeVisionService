#include "image_preprocessor.h"

#include <iostream>

namespace pre
{

// ==========================================
// 返回码转字符串（日志/调试用）
// ==========================================
const char *pre_status_str(PreStatus st)
{
  switch (st)
  {
  case PreStatus::OK:
    return "OK";
  case PreStatus::ERR_INVALID_ARG:
    return "ERR_INVALID_ARG";
  case PreStatus::ERR_UNSUPPORTED_FMT:
    return "ERR_UNSUPPORTED_FMT";
  case PreStatus::ERR_NOT_OPEN:
    return "ERR_NOT_OPEN";
  case PreStatus::ERR_NO_MEMORY:
    return "ERR_NO_MEMORY";
  case PreStatus::ERR_DEPENDENCY:
    return "ERR_DEPENDENCY";
  case PreStatus::ERR_IO:
    return "ERR_IO";
  case PreStatus::ERR_NOT_IMPL:
    return "ERR_NOT_IMPL";
  }
  return "ERR_UNKNOWN";
}

ImagePreprocessor::ImagePreprocessor() {}

ImagePreprocessor::~ImagePreprocessor() { close(); }

// ==========================================
// 应用配置
// ==========================================
PreStatus ImagePreprocessor::open(const PreprocessConfig &cfg)
{
  close();

  if (cfg.align <= 0)
  {
    std::cerr << "[ImagePreprocessor] align 必须为正数，当前 " << cfg.align << std::endl;
    return PreStatus::ERR_INVALID_ARG;
  }

  cfg_ = cfg;

  // 1) 字体：配了路径就加载；只配了文本却没配字体属于配置错误，直接拒绝，
  //    免得运行时每帧静默画不出字
  if (!cfg_.font_path.empty())
  {
    const bool ok = cfg_.use_freetype
                        ? osd_.set_truetype_font(cfg_.font_path, cfg_.font_size)
                        : osd_.set_bitmap_font(cfg_.font_path);
    if (!ok)
    {
      std::cerr << "[ImagePreprocessor] 字体加载失败: " << cfg_.font_path
                << " (freetype=" << (cfg_.use_freetype ? "on" : "off") << ")" << std::endl;
      return PreStatus::ERR_DEPENDENCY;
    }
  }
  else if (!cfg_.texts.empty())
  {
    std::cerr << "[ImagePreprocessor] 配置了 " << cfg_.texts.size()
              << " 条文本但没有字体，无法叠加文字" << std::endl;
    return PreStatus::ERR_DEPENDENCY;
  }

  // 2) 目标格式自检：FMT_UNKNOWN 表示不转码，其它值必须是转换器支持的格式
  if (cfg_.dst_fmt != PixelFormat::FMT_UNKNOWN)
  {
    ImageLayout probe;
    if (!FrameConverter::calc_layout(16, 16, cfg_.dst_fmt, cfg_.align, probe))
    {
      std::cerr << "[ImagePreprocessor] 目标格式不支持: " << static_cast<int>(cfg_.dst_fmt)
                << std::endl;
      return PreStatus::ERR_UNSUPPORTED_FMT;
    }
  }

  opened_ = true;
  std::cout << "[ImagePreprocessor] 已就绪: dst_fmt=" << static_cast<int>(cfg_.dst_fmt)
            << " align=" << cfg_.align << " boxes=" << cfg_.boxes.size()
            << " texts=" << cfg_.texts.size() << std::endl;
  return PreStatus::OK;
}

void ImagePreprocessor::close() { opened_ = false; }

// ==========================================
// 核心：转码 + 叠加
// ==========================================
PreStatus ImagePreprocessor::process(const FrameBuffer &src, FrameBuffer &dst)
{
  if (!opened_)
    return PreStatus::ERR_NOT_OPEN;

  // 不需要转码：dst 与 src 同格式。同一块内存就地叠加，否则先深拷贝再叠加
  if (cfg_.dst_fmt == PixelFormat::FMT_UNKNOWN || cfg_.dst_fmt == src.fmt)
  {
    if (dst.fmt != src.fmt)
      return PreStatus::ERR_INVALID_ARG;

    if (dst.data[0] != src.data[0])
    {
      PreStatus st = drawer_.snapshot(src, dst, src.fmt);
      if (st != PreStatus::OK)
        return st;
    }
    return overlay_only(dst);
  }

  // 需要转码：先转码再叠加（叠加项坐标基于输出分辨率，两者分辨率一致）
  PreStatus st = converter_.convert(src, dst);
  if (st != PreStatus::OK)
    return st;

  return overlay_only(dst);
}

PreStatus ImagePreprocessor::convert_only(const FrameBuffer &src, FrameBuffer &dst)
{
  if (!opened_)
    return PreStatus::ERR_NOT_OPEN;

  if (cfg_.dst_fmt == PixelFormat::FMT_UNKNOWN || cfg_.dst_fmt == src.fmt)
  {
    if (dst.fmt != src.fmt)
      return PreStatus::ERR_INVALID_ARG;
    return drawer_.snapshot(src, dst, src.fmt);
  }

  return converter_.convert(src, dst);
}

// ==========================================
// 只叠加静态项（顺序：先画框，再写字，保证文字盖在框上）
// ==========================================
PreStatus ImagePreprocessor::overlay_only(FrameBuffer &dst)
{
  if (!opened_)
    return PreStatus::ERR_NOT_OPEN;
  if (!dst.data[0])
    return PreStatus::ERR_INVALID_ARG;

  for (size_t i = 0; i < cfg_.boxes.size(); ++i)
  {
    PreStatus st = drawer_.draw_box(dst, cfg_.boxes[i]);
    if (st != PreStatus::OK)
      return st;
  }

  for (size_t i = 0; i < cfg_.texts.size(); ++i)
  {
    PreStatus st = osd_.draw_text(dst, cfg_.texts[i]);
    if (st != PreStatus::OK)
      return st;
  }

  return PreStatus::OK;
}

// ==========================================
// 运行期增删叠加项
// ==========================================
PreStatus ImagePreprocessor::add_box(const BoxStyle &box)
{
  if (box.rect.empty())
    return PreStatus::ERR_INVALID_ARG;

  cfg_.boxes.push_back(box);
  return PreStatus::OK;
}

PreStatus ImagePreprocessor::add_text(const TextItem &item)
{
  if (item.text.empty())
    return PreStatus::ERR_INVALID_ARG;
  if (!osd_.is_ready())
    return PreStatus::ERR_DEPENDENCY; // 没字体就先别加，避免运行时静默丢字

  cfg_.texts.push_back(item);
  return PreStatus::OK;
}

void ImagePreprocessor::clear_overlay()
{
  cfg_.boxes.clear();
  cfg_.texts.clear();
}

// ==========================================
// 估算 dst 布局
// align 是输出 stride 的对齐值；dst_fmt 为 FMT_UNKNOWN 时按源帧格式算
// ==========================================
bool ImagePreprocessor::calc_dst_layout(const FrameBuffer &src, PixelFormat dst_fmt,
                                        int align, ImageLayout &out)
{
  const PixelFormat fmt = (dst_fmt == PixelFormat::FMT_UNKNOWN) ? src.fmt : dst_fmt;
  return FrameConverter::calc_layout(src.width, src.height, fmt, align, out);
}

// ==========================================
// JSON 配置解析
//
// TODO（实现要点）：
//   1) 用 common/json.hpp（nlohmann::json::parse）解析，捕获 parse_error/type_error
//      与 PrintNode::set_data() 的写法保持一致（见 moudle_print.cpp）；
//   2) 没有目标 section 时返回 false 而不是"用默认值继续"，避免配置写错却静默跑通；
//   3) "dst_fmt" 字符串 -> PixelFormat 的映射建议独立成
//      static bool parse_pixel_format(const std::string&, PixelFormat&)；
//   4) 颜色统一用 [y,u,v] 三元数组解析，缺省取 YuvColor() 默认白；
//   5) 解析成功后再整体赋值给 out（局部变量攒好，避免解析一半污染 out）。
// ==========================================
bool ImagePreprocessor::parse_config(const char *config_str, const char *section,
                                     PreprocessConfig &out)
{
  (void)config_str;
  (void)section;
  (void)out;
  return false;
}

} // namespace pre

