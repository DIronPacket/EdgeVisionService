#ifndef EVS_MAIN_UPGRADE_SERVICE_H
#define EVS_MAIN_UPGRADE_SERVICE_H

#include <string>

namespace evs {
namespace upgrade_service {

// 一次性动作（执行完就退出）。不带子命令 = 常驻守护（kDaemon）。
//
// 为什么要这些子命令而不是纯常驻：回滚要和 systemd 的 OnFailure 联动（§7.4），
// 而 OnFailure 拉起的是一次性进程；boot_count 计数也由 ExecStartPost 一次性调用
// 完成（§7.2）。一次性动作必须能在**不建通道、不占端口**的前提下跑完。
enum class CliAction {
  kDaemon = 0,       // 常驻：订阅 cmd/#，等命令
  kCheck,            // 仅 CHECKING：拉清单 + 验签 + 判定，不下包
  kApply,            // 一条龙：check → download → verify（骨架期到 STAGING 前止步）
  kRollback,         // 手工/OnFailure 回滚
  kOnMediaStarted,   // evs-media 的 ExecStartPost：boot_count += 1（§7.2）
  kVerifyLocal,      // 只验本地清单签名（发布机自检，不联网）
  kStatus,           // 打印槽位 + 状态文件 + 免费空间
};

struct Options {
  std::string config_path;       // 空 = 依次找 config/upgrade.json、../config/upgrade.json
  CliAction action = CliAction::kDaemon;
  std::string manifest_path;     // kVerifyLocal 用
  std::string signature_path;    // 空 = <manifest>.sig
  std::string reason;            // 回滚原因，原样带进上报（start-limit / manual / ...）
  bool dry_run = false;          // 只打印计划，不落盘、不下载、不切链
  bool help = false;             // -h/--help：打印 Usage 后退出 0
};

// 解析 argv。失败返回 false 并写 err（main 负责打印 Usage）。
bool ParseArgs(int argc, char **argv, Options *out, std::string *err);
const char *Usage();

// 主流程入口。返回值 = 进程退出码：
//   0 成功 / 1 参数或配置错 / 2 动作失败（已如实上报原因码）
int Run(const Options &options);

}  // namespace upgrade_service
}  // namespace evs

#endif  // EVS_MAIN_UPGRADE_SERVICE_H
