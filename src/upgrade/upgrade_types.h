#ifndef EVS_UPGRADE_UPGRADE_TYPES_H
#define EVS_UPGRADE_UPGRADE_TYPES_H

#include <string>

namespace evs {
namespace upgrade {

// 升级状态机的状态，与 docs/design/DESIGN_UPGRADE.md §6.1 的图一一对应。
//
// ⚠ 枚举值会**持久化**（/data/edgevision/state/upgrade.json 里存的是数字），
//   所以只能在末尾追加，不能插入/重排 —— 否则老状态文件会被解释成别的阶段。
enum class Phase {
  kIdle = 0,            // 空闲，等命令
  kChecking = 1,        // 拉清单 + 验签 + 比对版本/硬件/ABI
  kDownloading = 2,     // 下包到 *.part（可断点续传）
  kVerifying = 3,       // 校验 size_bytes / payload_sha256 / files[]
  kStaging = 4,         // 解包到 .staging 并逐文件校验
  kActivating = 5,      // 停业务 → 原子切 current/previous → 起业务
  kHealthChecking = 6,  // 进程存活 + RTSP 首帧 + 帧率 + 稳定时长
  kCommitted = 7,       // 终态成功：boot_count 清零、清理旧版本
  kRollingBack = 8,     // 正在回滚（幂等，可重入）
  kFailed = 9,          // 终态失败（原因码见 code）
  kRecovery = 10,       // 恢复模式：回滚也失败，需人工介入
};

// 失败原因码：与 §9.3 的 16 个一一对应，外加一个**骨架期专用** kNotImpl。
//
// kNotImpl 只用于「本骨架还没实现的动作」（解包 / 切换 / systemctl / ffprobe）。
// 补齐 P2/P3 时必须把它删掉：对外只允许发 §9.3 表里的码，否则云侧无法归类。
enum class ErrorCode {
  kNone = 0,
  kNet,         // E_NET
  kHttp,        // E_HTTP
  kHash,        // E_HASH
  kSig,         // E_SIG
  kSchema,      // E_SCHEMA
  kHw,          // E_HW
  kAbi,         // E_ABI
  kDowngrade,   // E_DOWNGRADE
  kMinFrom,     // E_MIN_FROM
  kSpace,       // E_SPACE
  kApply,       // E_APPLY
  kHealth,      // E_HEALTH
  kRollback,    // E_ROLLBACK
  kBusy,        // E_BUSY
  kRecovery,    // E_RECOVERY
  kConfig,      // E_CONFIG
  kNotImpl,     // E_NOTIMPL（骨架期临时码，P2 删除）
};

const char *ToString(Phase phase);        // "CHECKING"（日志用，大写）
const char *PhaseToPayload(Phase phase);  // "checking"（上报 JSON 用，§9.2 小写）
bool ParsePhase(const std::string &text, Phase *out);  // 接受大小写两种写法
const char *ToString(ErrorCode code);     // "E_NET"（直接进上报的 code 字段）
bool IsTerminal(Phase phase);             // kCommitted / kFailed / kRecovery

// 状态机持久化内容（§6.4 恢复时读它）。
struct UpgradeState {
  Phase phase = Phase::kIdle;
  std::string from;             // 升级前版本，例如 "1.4.1"
  std::string to;               // 目标版本
  int boot_count = 0;           // 新版本连续启动次数（§7.2）
  int percent = 0;              // 最近一次进度（只用于展示/续传提示）
  std::string code;             // 终态原因码文本（失败时非空）
  std::string message;          // 人可读描述
  long long updated_at_ms = 0;  // 最后一次写入的墙钟时间

  UpgradeState() {}
};

long long NowMs();                       // Unix 毫秒
std::string FormatTimeMs(long long ms);  // "2026-11-15 10:00:00.123"（本地时区）

}  // namespace upgrade
}  // namespace evs

#endif  // EVS_UPGRADE_UPGRADE_TYPES_H
