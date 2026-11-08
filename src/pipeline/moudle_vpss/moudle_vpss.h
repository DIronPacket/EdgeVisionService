#ifndef MOUDLE_VPSS_H
#define MOUDLE_VPSS_H

#include "frame_buffer.h"
#include "image_preprocessor.h" // pre::ImagePreprocessor / pre::PreprocessConfig
#include "pipeline_node.h"

#include <atomic>
#include <string>
#include <thread>

// ==========================================
// VpssNode：流水线中间的图像处理节点（格式转换 + 叠加）
//
//   vi(采集) --> vpss(本节点) --> push(编码推流) --> print(耗时统计)
//
// 生命周期三段式，与 ViNode / PushNode / PrintNode 完全一致：
//   create()      起名字 + 建队列（中间节点用 USER 队列）
//   set_data()    解析 JSON 的 "vpss" 段，必须在 start() 之前调用
//   start()/stop() 起停工作线程；run() 是工作线程主循环
//
// 数据流约定（改之前先读 HANDOFF §2.4）：
//   1. 不 new 块、不额外申请块：上游把源帧挂在块的 src_slot（默认 "nv12"）槽位上，
//      本节点在同一个块上挂一个输出帧（默认槽位 "vpss_out"）；
//   2. 下游取法：block->_memMeta->data<FrameBuffer *>("vpss_out")；
//      想直接喂现有 PushNode（它写死读 "nv12"），把 out_slot 配成 "nv12" 即可；
//   3. 取块必有归还：run() 任何分支的出口都必须 m_queue.release(block)；
//   4. 输出帧每块一份，避免多块同时在飞时互相踩内存（见 ensure_dst_frame）。
//
// 配置示例（config/push.json 里加一段，和其它节点共用同一份配置）：
//   "vpss": {
//     "enable": true,           // false = 纯透传（默认，接进链路不影响现有行为）
//     "src_slot": "nv12",       // 上游源帧所在槽位
//     "out_slot": "vpss_out",   // 本节点输出帧挂载的槽位
//     "dst_fmt": "yuv420p",     // nv12|yuv420p|yuv422|bgr888，空 = 不转码
//     "align": 16,              // 输出 stride 对齐
//     "preprocess": { ... }     // 直接转交 pre::PreprocessConfig（字体/画框/文本）
//   }
// ==========================================
class VpssNode : public PipelineNode
{
public:
  VpssNode();

  ~VpssNode() override;

  // 重写基类的核心生命周期接口
  void start() override;
  void stop() override;
  void create(int channel_id, const std::string &name) override;
  void set_data(const char *config) override;

  // 当前输出帧槽位名（供同进程内其它模块查询，例如按需接不同的下游）
  const std::string &out_slot() const { return out_slot_; }

private:
  // 内部工作线程函数
  void run();

  // 把 "preprocess" 段解析成 pre::PreprocessConfig 并 open 预处理门面。
  // 失败只降级为透传，不影响上下游（联调期最怕一个节点把整条链路拖死）
  void open_preprocessor();

  // 取上游写好的源帧；块里没有该槽位时返回 nullptr
  FrameBuffer *get_src_frame(pool::MemBlock *block);

  // 取本块的输出帧：首次遇到该块时按源帧尺寸 + 输出格式分配并挂到块上，之后复用
  FrameBuffer *ensure_dst_frame(pool::MemBlock *block, const FrameBuffer &src);

  // ===== 关键实现：这里留给你自己写（详见 .cpp 里的注释）=====
  bool process_frame(pool::MemBlock *block, FrameBuffer *src, FrameBuffer *dst);

private:
  std::atomic<bool> running_;
  std::thread thread_;

  json cfg_;
  std::mutex cfg_mtx_;

  // ---- 由 set_data 解析得到（运行期不再变更，改配置请重启节点）----
  bool enable_ = false;                  // false: 纯透传，不建输出帧、不做任何处理
  std::string src_slot_ = "nv12";        // 上游源帧槽位（ViNode 写的就是这个名字）
  std::string out_slot_ = "vpss_out";    // 输出帧槽位
  PixelFormat dst_fmt_ = PixelFormat::FMT_UNKNOWN; // UNKNOWN = 只叠加不转码
  int align_ = 16;                       // 输出 stride 对齐

  // "preprocess" 段的原始 JSON（等 parse_config 实现后由它解析成 pre_cfg_）
  std::string pre_json_;

  pre::PreprocessConfig pre_cfg_;
  pre::ImagePreprocessor pre_;

  // ---- 运行期状态 ----
  bool pre_opened_ = false;   // 预处理门面是否可用（false => 透传）
  bool need_process_ = false; // 是否配了转码或叠加项（都没配就没必要多拷一份）
  uint64_t frame_no_ = 0;     // 已处理帧数（仅用于首帧日志）

  // 告警只打一次，避免逐帧刷屏把串口/日志打爆
  bool warned_no_src_ = false;
  bool warned_no_dst_ = false;
  bool warned_proc_fail_ = false;
};

#endif
