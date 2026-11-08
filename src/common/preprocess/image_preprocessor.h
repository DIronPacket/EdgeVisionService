#ifndef EVS_PREPROCESS_IMAGE_PREPROCESSOR_H
#define EVS_PREPROCESS_IMAGE_PREPROCESSOR_H

#include <string>
#include <vector>

#include "frame_converter.h"
#include "frame_drawer.h"
#include "osd_renderer.h"
#include "preprocess_define.h"

namespace pre
{

// 预处理配置：一份配置描述"这一路要做什么"
struct PreprocessConfig
{
  bool enable = true;                             // 总开关
  PixelFormat dst_fmt = PixelFormat::FMT_UNKNOWN; // 输出格式；FMT_UNKNOWN = 不转码
  int align = 1;                                  // 输出 stride 对齐（按下游要求填，如 16/128）
  std::string font_path;                          // 字体文件路径；空 = 不加载字体
  int font_size = 24;                             // 矢量字号（点阵字体忽略此字段）
  bool use_freetype = false;                      // true=ttf/otf，需 BUILD_FREETYPE_OSD=ON
  std::vector<BoxStyle> boxes;                    // 静态画框列表
  std::vector<TextItem> texts;                    // 静态文本列表
};

// ==========================================
// 预处理门面：src 帧进 -> 转码 + 叠加 -> dst 帧出
//
// 这是 common 里对外暴露的唯一入口，内部组合三个组件：
//   FrameConverter（格式互转） + FrameDrawer（画框/贴图/截图） + OsdRenderer（字符叠加）
//
// 使用约定：
//   1. dst 的内存由调用方准备（一般是内存池里的另一块，或节点自己的缓冲）；
//      本类不分配帧内存，也不释放任何 FrameBuffer（HANDOFF §2.4-5）；
//   2. 同一实例只在一个线程里用（非线程安全）；
//   3. 逐帧调用 process()，内部不打印逐帧日志。
//
// 典型用法（放在一个 PipelineNode 的 run() 里）：
//   pre::ImagePreprocessor proc;
//   proc.open(cfg);
//   ...
//   if (proc.process(*src_fb, *dst_fb) != pre::PreStatus::OK) { m_queue.release(block); continue; }
// ==========================================
class ImagePreprocessor
{
public:
  ImagePreprocessor();
  ~ImagePreprocessor();

  ImagePreprocessor(const ImagePreprocessor &) = delete;
  ImagePreprocessor &operator=(const ImagePreprocessor &) = delete;

  // 应用配置：加载字体、登记静态叠加项。重复调用会先 close() 再重建。
  // 返回 ERR_DEPENDENCY：字体加载失败，或"配了文本但没配字体"；
  //      ERR_INVALID_ARG：配置自相矛盾（例如 align<=0）。
  PreStatus open(const PreprocessConfig &cfg);

  // 只关状态，不清理叠加项（便于运行期改配置后重新 open）
  void close();

  bool is_opened() const { return opened_; }

  // 核心：src -> dst。先按配置转码，再叠加 boxes / texts。
  // dst 的格式与尺寸要求：
  //   - cfg.dst_fmt != FMT_UNKNOWN：dst.fmt 必须是 cfg.dst_fmt，宽高与 src 相同；
  //   - cfg.dst_fmt == FMT_UNKNOWN：dst.fmt 必须等于 src.fmt；
  //     若 dst 与 src 指向同一块内存则原地叠加，否则深拷贝后再叠加。
  PreStatus process(const FrameBuffer &src, FrameBuffer &dst);

  // 只转码不叠加：需要一帧干净数据（送编码器 / 送算法）时用
  PreStatus convert_only(const FrameBuffer &src, FrameBuffer &dst);

  // 只叠加不转码：帧已经是目标格式时用（可直接就地改上游帧）
  PreStatus overlay_only(FrameBuffer &dst);

  // 运行期增删叠加项（不需要重新 open）
  PreStatus add_box(const BoxStyle &box);
  PreStatus add_text(const TextItem &item);
  void clear_overlay();

  // 直接访问内部组件（需要精细控制时用；常规流程走 process 即可）
  OsdRenderer &osd() { return osd_; }
  FrameDrawer &drawer() { return drawer_; }
  FrameConverter &converter() { return converter_; }

  // 估算 dst 需要的布局：给出源帧与目标格式，算出 stride / 各平面大小，
  // 供调用方在配置阶段一次性申请好内存（避免每帧 new）
  static bool calc_dst_layout(const FrameBuffer &src, PixelFormat dst_fmt, int align,
                              ImageLayout &out);

  // 从 JSON 配置字符串解析本模块配置（section 为空时默认取 "preprocess" 段）。
  // 期望格式：
  // {
  //   "preprocess": {
  //     "enable": true,
  //     "dst_fmt": "nv12",                  // nv12|yuv420p|yuv422|bgr888|""(不转码)
  //     "align": 16,
  //     "font": {"path": "font.evsfont", "size": 24, "freetype": false},
  //     "boxes": [{"x":10,"y":10,"w":100,"h":60,"color":[235,128,128],
  //                "thickness":2,"filled":false}],
  //     "texts": [{"text":"CAM0","x":20,"y":20,"color":[235,128,128],"scale":1}]
  //   }
  // }
  // 解析失败返回 false（不改动 out）。
  static bool parse_config(const char *config_str, const char *section,
                           PreprocessConfig &out);

private:
  PreprocessConfig cfg_;

  FrameConverter converter_;
  FrameDrawer drawer_;
  OsdRenderer osd_;

  bool opened_ = false;
};

} // namespace pre

#endif
