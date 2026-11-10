#include "moudle_vpss.h"

#include "MemPool.h"
#include "block_profiler.h"

#include <cctype>
#include <cstdio>
#include <iostream>
#include <new>

namespace {

// 配置字符串统一转小写，容忍 YUV420P / yuv420p / I420 等写法
std::string lower(const std::string &s) {
  std::string r(s);
  for (size_t i = 0; i < r.size(); ++i) {
    r[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(r[i])));
  }
  return r;
}

// 像素格式转名字，只用于日志
const char *fmt_name(PixelFormat fmt) {
  switch (fmt) {
  case PixelFormat::FMT_NV12:
    return "nv12";
  case PixelFormat::FMT_YUV420P:
    return "yuv420p";
  case PixelFormat::FMT_YUV422:
    return "yuv422p";
  case PixelFormat::FMT_BGR888:
    return "bgr888";
  default:
    return "keep";
  }
}

// 解析 dst_fmt 字段。ok=false 表示取值不在已知集合里（调用方据此保留原值）
PixelFormat parse_fmt(const std::string &s, bool &ok) {
  ok = true;
  const std::string v = lower(s);
  // 空 / none / keep 都表示"不转码，只做叠加"
  if (v.empty() || v == "none" || v == "keep")
    return PixelFormat::FMT_UNKNOWN;
  if (v == "nv12")
    return PixelFormat::FMT_NV12;
  if (v == "yuv420p" || v == "i420")
    return PixelFormat::FMT_YUV420P;
  // 注意：FMT_YUV422 的实际布局是三平面 YUV422P(I422)，不是打包的 YUYV/UYVY
  if (v == "yuv422" || v == "yuv422p" || v == "i422")
    return PixelFormat::FMT_YUV422;
  if (v == "bgr888" || v == "bgr")
    return PixelFormat::FMT_BGR888;
  ok = false;
  return PixelFormat::FMT_UNKNOWN;
}

} // namespace

// running_ 是 std::atomic<bool>，默认构造不写值（C++14），必须显式初始化，
// 否则内存里的脏值会让 start() 里 running_.exchange(true) 直接返回 true，线程起不来
VpssNode::VpssNode() : running_(false) {}

VpssNode::~VpssNode() { stop(); }

void VpssNode::start()
{
  if (running_.exchange(true))
    return;

  // 1. 按配置准备预处理门面（失败只降级为透传，不中断链路）
  if (enable_)
    open_preprocessor();
  else
    std::cout << "[" << get_full_name()
              << "] enable=false，本节点纯透传（不改帧、不建输出帧）" << std::endl;

  // 2. 启动线程
  thread_ = std::thread(&VpssNode::run, this);
  std::cout << "[" << get_full_name() << "] Vpss Node Started." << std::endl;
}

void VpssNode::stop()
{
  if (!running_.exchange(false))
    return;

  // 队列阻塞由 getBlock(100ms) 超时兜底（与 PrintNode 同款），无需额外唤醒
  if (thread_.joinable())
  {
    thread_.join();
  }

  // 只关状态，不动叠加项：允许 stop 后再 start（会重新 open）
  pre_.close();
  pre_opened_ = false;

  std::cout << "[" << get_full_name() << "] Vpss Node Stopped." << std::endl;
}

void VpssNode::create(int channel_id, const std::string &name)
{
  channel_id_ = channel_id;
  m_name = name + std::to_string(channel_id);
  // 中间节点：消费上游传下来的块，与 PushNode / PrintNode 一致用 USER 队列
  m_queue.InitQueue(m_name, 20, pool::QueueType::USER);
}
void VpssNode::set_data(const char *config_str)
{
  std::lock_guard<std::mutex> lock(cfg_mtx_);
  // 1. 判空保护
  if (!config_str)
  {
    std::cerr << "Config string is null!" << std::endl;
    return;
  }

  try
  {
    // 2. 解析 JSON：只认 {"vpss": {...}} 这一层，
    //    避免和同一份配置里的 push / print 段混在一起
    json j = json::parse(config_str);
    if (!j.contains("vpss") || !j["vpss"].is_object())
    {
      std::cout << "[VpssNode] 配置中没有 vpss 段，保持默认（enable=false，纯透传）"
                << std::endl;
      return;
    }

    json p = j["vpss"];
    cfg_ = p;

    // 3. 开关与槽位名
    enable_ = p.value("enable", enable_);
    src_slot_ = p.value("src_slot", src_slot_);
    out_slot_ = p.value("out_slot", out_slot_);
    if (src_slot_.empty() || out_slot_.empty())
    {
      std::cerr << "[VpssNode] src_slot/out_slot 不能为空（\"" << src_slot_
                << "\" / \"" << out_slot_ << "\"），回退默认 nv12 / vpss_out"
                << std::endl;
      src_slot_ = "nv12";
      out_slot_ = "vpss_out";
    }

    // 4. 输出格式与 stride 对齐
    bool fmt_ok = true;
    const std::string dst_fmt_str = p.value("dst_fmt", std::string(""));
    dst_fmt_ = parse_fmt(dst_fmt_str, fmt_ok);
    if (!fmt_ok)
    {
      std::cerr << "[VpssNode] dst_fmt 取值非法: \"" << dst_fmt_str
                << "\"，可选 nv12 / yuv420p / yuv422 / bgr888 / \"\"，回退不转码"
                << std::endl;
      dst_fmt_ = PixelFormat::FMT_UNKNOWN;
    }

    align_ = p.value("align", align_);
    if (align_ <= 0)
    {
      std::cerr << "[VpssNode] align 取值非法: " << align_ << "，回退 16"
                << std::endl;
      align_ = 16;
    }

    // 4.1 src_slot == out_slot 表示"原地叠加"（源和目标共用一块内存），
    //     只有不转码时才成立：要转码必须另开一块，否则一边读一边写会把数据搅乱
    if (src_slot_ == out_slot_ && dst_fmt_ != PixelFormat::FMT_UNKNOWN)
    {
      std::cerr << "[VpssNode] src_slot 与 out_slot 同名时不能转码（dst_fmt="
                << fmt_name(dst_fmt_) << "），输出槽位回退 "
                << (src_slot_ == "vpss_out" ? "vpss_out_b" : "vpss_out")
                << std::endl;
      out_slot_ = (src_slot_ == "vpss_out") ? "vpss_out_b" : "vpss_out";
    }

    // 5. 叠加相关（字体 / 画框 / 文本）整段交给 pre::PreprocessConfig：
    //    这里只缓存原始 JSON，start() 里交给 ImagePreprocessor::parse_config 解析，
    //    本节点不重复实现一遍字段映射（改格式只改 preprocess 一处）
    pre_json_.clear();
    if (p.contains("preprocess"))
      pre_json_ = p["preprocess"].dump();

    std::cout << "[VpssNode] 配置已加载: enable=" << (enable_ ? "true" : "false")
              << " src_slot=" << src_slot_ << " out_slot=" << out_slot_
              << " dst_fmt=" << fmt_name(dst_fmt_) << " align=" << align_
              << (pre_json_.empty() ? "" : " (含 preprocess 叠加配置)")
              << std::endl;
  }
  catch (const json::parse_error &e)
  {
    // 6. 捕获解析异常，防止崩溃
    std::cerr << "JSON Parse Error: " << e.what() << std::endl;
  }
  catch (const json::type_error &e)
  {
    std::cerr << "JSON Type Error: " << e.what() << std::endl;
  }
}

void VpssNode::open_preprocessor()
{
  // 1. "preprocess" 段 -> pre::PreprocessConfig
  //    TODO(pre)：ImagePreprocessor::parse_config 目前是框架占位（固定返回 false
  //    且不改动 out），所以这段现在不会生效，节点只按 dst_fmt_/align_ 走转码。
  //    它实现之后这里不用改：把字体/画框/文本写在 "vpss": {"preprocess": {...}} 里即可。
  if (!pre_json_.empty())
  {
    pre::PreprocessConfig parsed;
    if (pre::ImagePreprocessor::parse_config(pre_json_.c_str(), "preprocess", parsed))
      pre_cfg_ = parsed;
    else
      std::cerr << "[VpssNode] preprocess 段解析失败（parse_config 尚未实现或配置有误），"
                   "按默认叠加配置继续"
                << std::endl;
  }

  // 2. "vpss" 段里的字段优先级最高：这一路最终输出什么格式由它决定
  pre_cfg_.dst_fmt = dst_fmt_;
  pre_cfg_.align = align_;

  // 3. 既没配转码也没配叠加项时，没必要每帧多拷一份：直接透传
  need_process_ = (dst_fmt_ != PixelFormat::FMT_UNKNOWN) ||
                  !pre_cfg_.boxes.empty() || !pre_cfg_.texts.empty();
  if (!need_process_)
  {
    std::cout << "[" << get_full_name()
              << "] 未配置 dst_fmt 也未配置叠加项，本节点纯透传" << std::endl;
    return;
  }

  // 4. 打开预处理门面（失败只降级，不把整条链路拖死）
  const pre::PreStatus st = pre_.open(pre_cfg_);
  if (st != pre::PreStatus::OK)
  {
    pre_opened_ = false;
    std::cerr << "[" << get_full_name() << "] 预处理初始化失败: "
              << pre::pre_status_str(st) << "（本节点退化为纯透传，不影响上下游）"
              << std::endl;
    return;
  }

  pre_opened_ = true;
  std::cout << "[" << get_full_name() << "] 预处理已就绪: dst_fmt=" << fmt_name(dst_fmt_)
            << " align=" << align_ << " boxes=" << pre_cfg_.boxes.size()
            << " texts=" << pre_cfg_.texts.size() << std::endl;
}

void VpssNode::run()
{
  while (running_.load())
  {
    pool::MemBlock *block = m_queue.getBlock(100);
    if (!block)
      continue;

    // 纯透传：未启用 / 预处理初始化失败 / 没配任何处理项。
    // 这条路径零拷贝、零额外内存，所以本节点接进链路后行为与不接完全一致。
    if (!enable_ || !pre_opened_ || !need_process_)
    {
      push_input_pipe(block);
      m_queue.release(block);
      continue;
    }

    // 1. 取上游源帧（ViNode 写的就是 "nv12" 槽位）
    FrameBuffer *src = get_src_frame(block);
    if (!src)
    {
      if (!warned_no_src_)
      {
        warned_no_src_ = true;
        std::cerr << "[" << get_full_name() << "] 块上没有 \"" << src_slot_
                  << "\" 槽位的源帧（上游不是 ViNode，或槽位名配错），本帧跳过"
                  << std::endl;
      }
      m_queue.release(block);
      continue;
    }

    // 2. 本节点耗时统计：开始时间戳（拿到帧之后、处理之前）
    profile::NodeProfiler *profiler = profile::get_profiler(block);
    if (profiler)
      profiler->begin(m_name);

    // 3. 取/建本块的输出帧，然后交给具体实现
    FrameBuffer *dst = ensure_dst_frame(block, *src);
    if (!dst && !warned_no_dst_)
    {
      warned_no_dst_ = true;
      std::cerr << "[" << get_full_name() << "] 输出帧不可用（分配失败），本帧跳过"
                << std::endl;
    }
    const bool ok = (dst != nullptr) && process_frame(block, src, dst);

    // 结束时间戳必须写在 push / release 之前，否则会把下游等待时间算进本节点
    if (profiler)
      profiler->end(m_name);

    if (!ok)
    {
      if (!warned_proc_fail_)
      {
        warned_proc_fail_ = true;
        std::cerr << "[" << get_full_name()
                  << "] 本帧处理失败，丢弃该帧（后续失败不再重复打印）" << std::endl;
      }
      // 失败分支同样必须归还（P0-3）：否则池子被逐帧抽干，整条链路静默停摆
      m_queue.release(block);
      continue;
    }

    ++frame_no_;
    push_input_pipe(block);
    m_queue.release(block);
    fflush(stdout);
  }
}

FrameBuffer *VpssNode::get_src_frame(pool::MemBlock *block)
{
  if (!block || !block->_memMeta)
    return nullptr;
  return block->_memMeta->data<FrameBuffer *>(src_slot_);
}

// ==========================================
// 输出帧：每块一份，懒创建
//
// 为什么不是"节点自持一份"：块是流水的，同一时刻可能有多个块在飞，
// 一份共享输出帧会被后面的帧覆盖掉还没被下游用完的数据。
// 所以按块挂载：第一次遇到该块时分配，之后每帧复用（零额外分配）。
//
// 内存归属：这块内存和 MemManager::MemInit() 里给每个块分配的 "nv12" 帧一样，
// 挂在块自己的 MemMeta 槽位上，随块常驻、随块释放 ——
// MemManager::FreeMem() 统一调 MemMeta::clear()，把槽位里的对象一起还回去，
// 包括这里由节点自己挂上去的 out_slot_ 帧，本节点不用另写释放逻辑。
// ==========================================
FrameBuffer *VpssNode::ensure_dst_frame(pool::MemBlock *block, const FrameBuffer &src)
{
  if (!block || !block->_memMeta)
    return nullptr;

  FrameBuffer *dst = block->_memMeta->data<FrameBuffer *>(out_slot_);
  if (dst)
    return dst;

  // 没配 dst_fmt 时输出格式跟随源帧（只做叠加）
  const PixelFormat fmt =
      (dst_fmt_ == PixelFormat::FMT_UNKNOWN) ? src.fmt : dst_fmt_;

  FrameBuffer *frame = new (std::nothrow) FrameBuffer;
  if (!frame)
    return nullptr;

  if (!frame->Allocate(src.width, src.height, fmt, align_))
  {
    std::cerr << "[" << get_full_name() << "] 输出帧分配失败: " << src.width << "x"
              << src.height << " " << fmt_name(fmt) << std::endl;
    delete frame;
    return nullptr;
  }

  // 时间戳沿用上游：下游要按帧时间戳做同步时不用再找源帧
  frame->timestamp_ms = src.timestamp_ms;

  // 槽位同名时的一套规矩：已在 map 里的槽位会被上面的 data<> 直接取到（复用，
  // 走不到这里）；只有该槽位此前为空才会登记。MemMeta::set_ptr 覆盖同名旧槽位时
  // 会先释放旧对象，所以"接管"只对空槽位成立，不会把上游正在用的帧删掉。
  block->_memMeta->set_ptr(out_slot_, frame);

  if (frame_no_ == 0)
  {
    // 首帧打印一次布局，联调时先确认 stride / 平面数是否符合预期
    std::cout << "[" << get_full_name() << "] 首帧: " << src.width << "x" << src.height
              << " " << fmt_name(src.fmt) << "(stride=" << src.stride[0] << ")"
              << " -> " << frame->width << "x" << frame->height << " "
              << fmt_name(frame->fmt) << "(stride=" << frame->stride[0]
              << " stride1=" << frame->stride[1] << ") 槽位=" << out_slot_
              << std::endl;
  }

  return frame;
}

// ==========================================
// ★ 关键实现：本节点真正干活的地方 —— 留给你自己写 ★
//
// 现在这里是空实现（直接 return true），等价于"透传"：
// 节点接进链路后画面与不接一致，方便你先跑通上下游、看清 stride/槽位，再往里填。
//
// 按下面顺序填（每一步都只改这一个函数，run() 不用动）：
//
//   1) 主路径：调预处理门面把 src 转码 + 叠加到 dst
//        const pre::PreStatus st = pre_.process(*src, *dst);
//        if (st != pre::PreStatus::OK) {
//          // 首帧失败记得把返回码打出来，pre_status_str(st) 直接给中文名
//          return false;
//        }
//      ⚠ 注意：pre 模块里的像素级循环目前还是框架占位（统一返回 ERR_NOT_IMPL，
//        见 src/common/preprocess/README.md §6），所以现在就打开这一步会让每一帧都
//        走失败分支被丢弃。先把你要用的那几条转换/绘图实现出来，再打开这里。
//
//   2) 只要其中一项时，用更省的两个入口，避免多余拷贝：
//        - 只转码不叠加： pre_.convert_only(*src, *dst)
//        - 只叠加不转码： 先 memcpy/convert 好再 pre_.overlay_only(*dst)
//
//   3) 硬件的路（可选）：RK3588 上若帧是 DMA-BUF，缩放/格式转换交给 RGA 更省 CPU，
//      只把叠加留给 CPU：RGA 转完后调 pre_.overlay_only(*dst)。
//      RGA 封装在 src/platform（目前是空壳），要走这条路先补 platform 侧接口。
//
//   4) 联调辅助（用完删掉）：
//        - 每 N 帧打印一次本节点耗时、dst 的 stride/平面数；
//        - 需要抓图对比时用 pre_.drawer().save_snapshot(*dst, "/tmp/vpss.bmp")。
//
// 返回值语义：true = 处理完成，块继续流给下游；false = 本帧作废，
// run() 会打印一次告警并把块还回池子（不会卡住整条链路）。
// ==========================================
bool VpssNode::process_frame(pool::MemBlock *block, FrameBuffer *src, FrameBuffer *dst)
{
  // 未使用的参数在这里显式标记，避免 -Wunused-parameter 告警
  (void)block;
  (void)src;
  (void)dst;

  // TODO: 换成真正的处理，例如
  //   return pre_.process(*src, *dst) == pre::PreStatus::OK;
  return true;
}


