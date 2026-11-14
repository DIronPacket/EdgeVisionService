#ifndef EVS_MAIN_MEDIA_MEDIA_SERVICE_H
#define EVS_MAIN_MEDIA_MEDIA_SERVICE_H

#include <string>

namespace evs {
namespace media {

// 媒体进程的启动参数。
//
// 两个入口共用这一套编排（逻辑只有一份）：
//   src/main/media/main_media.cpp → 可执行 evs_media（新名字，进程 1）
//   src/main/main.cpp             → 可执行 EdgeVisionService（历史兼容壳）
struct MediaServiceOptions {
    // 空 = 交给 NodeConfigManager::load_with_fallback 按候选路径找
    //       （./config/node.json → ../config/node.json → push.json …）
    std::string config_path;
    // 崩溃报告文件名里用的程序名：evs_media / EdgeVisionService
    std::string app_name = "evs_media";
    bool install_crash_handler = true;
};

// 跑完媒体进程的整条生命周期，返回进程退出码（0 = 正常退出，1 = 配置/装配失败）：
//   0) 装崩溃 handler（早于任何线程创建）
//   1) 读配置 + 打印摘要（不碰节点、不碰设备）
//   2) 进程级资源 MemManager::MemInit()
//   3) PipelineBuilder::build_all() 装配（单链失败只告警）
//   4) 装 SIGINT/SIGTERM → start_all() → 等信号
//   5) 有序退出：stop_all() → 释放节点 → FreeMem()
//
// 这里不出现任何节点名 / 设备路径 / 像素参数——加一路只改 config/node.json。
int Run(const MediaServiceOptions &opt);

}  // namespace media
}  // namespace evs

#endif  // EVS_MAIN_MEDIA_MEDIA_SERVICE_H
