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
  void create(int channel_id, const std::string &name);
  void set_data(const char *config) override;

  static void sendFrame(const uint8_t *pkt_data, size_t pkt_len,
                        CodingType type, uint64_t pts_us, void *user_arg);

private:
  // 内部工作线程函数
  void run();

  // 初始化 ZLM 推流句柄
  void init_zlm_push();

private:
  std::atomic<bool> running_;
  std::thread thread_;

  std::mutex q_mtx_;
  std::condition_variable cv_;

  json cfg_;
  std::mutex cfg_mtx_;

  // ZLM 相关
  ZlmServerStreamHandle m_handle = nullptr;
  ZlmFrameCodec codec_type_;
  MppEncoder enc;


  // 推流配置（可以从 json config 里解析更新）
  std::string app_ = "live";
  std::string stream_ = "test_stream";
  int rtsp_port_ = 8554;
  int width_ = 1920;
  int height_ = 1080;
  int fps_ = 30;
};

#endif