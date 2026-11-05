#ifndef MOUDLE_VI_H
#define MOUDLE_VI_H
#include "capture_factory.h"
#include "frame_buffer.h"
#include "pipeline_node.h"
#include "rk_mpp_encoder.h" // 引入RK MPP编码器头文件
#include <iostream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio> // fopen fwrite fclose
#include <queue>
extern "C"
{
#include <libavformat/avformat.h>
}

class ViNode : public PipelineNode
{
public:
  // 构造函数：传入通道号、节点名、以及视频编码类型 (H264/H265)
  ViNode();

  ~ViNode() override;

  // 重写基类的核心生命周期接口
  void start() override;
  void stop() override;
  void create(int channel_id, const std::string &name);
  void set_data(const char *config) override;

private:
  // 内部工作线程函数
  void run();

private:
  std::atomic<bool> running_;
  std::thread thread_;

  std::mutex q_mtx_;
  std::condition_variable cv_;

  json cfg_;
  std::mutex cfg_mtx_;

  // 推流输出分辨率与帧率（由 set_data 从 config/push.json 解析，
  // 采集到的原始帧会缩放到该尺寸，从而保证与 PushNode 声明的分辨率一致）
  int out_width_ = 1920;
  int out_height_ = 1080;
  int out_fps_ = 30;

  std::unique_ptr<CaptureInterface> m_capture;
};

#endif