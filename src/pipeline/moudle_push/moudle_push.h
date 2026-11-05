#ifndef MOUDLE_PUSH_H
#define MOUDLE_PUSH_H

#include "pipeline_node.h"
#include "rk_mpp_encoder.h"
#include "zlm_push_server.h"
#include <atomic>
#include <condition_variable>
#include <queue>
#include <string>
#include <thread>

class PushNode : public PipelineNode {
public:
  // 构造函数：传入通道号、节点名、以及视频编码类型 (H264/H265)
  PushNode();

  ~PushNode() override;

  // 重写基类的核心生命周期接口
  void start() override;
  void stop() override;
  void create(int channel_id, const std::string &name) override;
  void set_data(const char *config) override;

  static void sendFrame(const uint8_t *pkt_data, size_t pkt_len,
                        CodingType type, uint64_t pts_us, void *user_arg);

private:
  // 内部工作线程函数
  void run();

  // 初始化 ZLM 推流句柄
  void init_zlm_push();

  // 解析 codec 字段（h264/avc、h265/hevc），返回 false 表示取值非法
  bool apply_codec(const std::string &codec);

  // 解析 protocol/transport，返回 false 表示当前后端不支持该协议
  bool apply_protocol(const std::string &protocol, const std::string &transport);

private:
  std::atomic<bool> running_;
  std::thread thread_;

  std::mutex q_mtx_;
  std::condition_variable cv_;

  json cfg_;
  std::mutex cfg_mtx_;

  // ZLM 相关
  ZlmServerStreamHandle m_handle = nullptr;
  ZlmFrameCodec codec_type_ = ZLM_FRAME_VIDEO_H265; // 推流侧 codec
  CodingType codec_coding_ = VIDEO_CodingH265;      // 编码器侧 codec
  MppEncoder enc;

  // 推流配置（由 set_data 从 json 解析更新，见 config/push.json）
  bool config_loaded_ = false;
  bool enable_ = true;
  std::string protocol_ = "rtsp";  // rtsp | rtmp | rtp（rtmp/rtp 后端未实现）
  std::string transport_ = "tcp";  // tcp | udp，仅记录，RTSP 由播放端决定
  std::string app_ = "live";
  std::string stream_ = "ch0";
  int rtsp_port_ = 8550;
  int width_ = 1920;
  int height_ = 1080;
  int fps_ = 30;
  int bitrate_kbps_ = 4096;
  int gop_ = 30; // 预留：EncoderInterface 尚未暴露 GOP 参数

  // 推流时间戳：按配置帧率推进，避免码流时间戳与实际帧率不符
  uint64_t frame_index_ = 0;
  uint64_t frame_interval_us_ = 33333;

  // 分辨率不一致只告警一次，避免逐帧刷日志
  bool warned_size_mismatch_ = false;
};

#endif