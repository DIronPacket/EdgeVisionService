// ============================================================
// 进程 3：升级代理（可执行名 evs_upgrade）
//
// 职责（骨架，见 docs/design/DESIGN_UPGRADE.md）：
//   1. 装崩溃 handler（最早，早于任何线程）
//   2. argv → Options（--check / --apply / --rollback / --on-media-started /
//      --verify= / --status；不带 = 常驻守护）
//   3. 交给 evs::upgrade_service::Run()：读配置 → 状态机 → 有序退出
//
// 这个文件**只做入口**：不读配置、不认识 Phase、不碰磁盘。这样一次性动作
// （OnFailure 拉起的 --rollback、ExecStartPost 的 --on-media-started）和常驻
// 模式共享同一份实现，不会出现"两条路径逻辑不一样"。
//
// 用法：
//   ./out/bin/evs_upgrade                       # 常驻，依次找 config/upgrade.json、../config/upgrade.json
//   ./out/bin/evs_upgrade /data/edgevision/config/upgrade.json --status
//   make run-upgrade ARGS="--check"
//
// 退出码：0 成功 / 1 参数或配置错 / 2 动作失败（原因码见上报的 code 字段）
// ============================================================

#include <cstdio>
#include <string>

#include "crash_handler.h"
#include "upgrade_service.h"

int main(int argc, char **argv) {
  // 崩溃采集要最早安装（早于任何线程创建）。升级进程崩了等于「设备停在半路」，
  // 现场必须能拿到崩溃阶段 —— service 会在每次阶段推进时 SetCrashTag("upgrade_phase")。
  evs::common::CrashConfig crash;
  crash.app_name = "evs_upgrade";
  crash.report_dir = "";  // 空 = EVS_CRASH_DIR 环境变量，再空 = ./crash
  crash.dump_core = true;
  crash.trap_sigquit = true;
  crash.install_terminate = true;
  if (!evs::common::InstallCrashHandler(crash))
    std::fprintf(stderr, "[upgrade] 崩溃 handler 未完全安装（继续运行）\n");

  evs::upgrade_service::Options opt;
  std::string err;
  if (!evs::upgrade_service::ParseArgs(argc, argv, &opt, &err)) {
    std::fprintf(stderr, "[upgrade] 参数错误: %s\n\n", err.c_str());
    std::printf("%s", evs::upgrade_service::Usage());
    return 1;
  }
  return evs::upgrade_service::Run(opt);
}
