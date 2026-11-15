#ifndef EVS_UPGRADE_SLOT_MANAGER_H
#define EVS_UPGRADE_SLOT_MANAGER_H

#include <string>
#include <vector>

#include "manifest.h"  // ManifestFile
#include "upgrade_types.h"

namespace evs {
namespace upgrade {

// 槽位配置（config/upgrade.json 的顶层与 release 段，§10.2）
struct SlotConfig {
  std::string install_root = "/opt/edgevision";  // releases/ current previous .staging
  std::string data_root = "/data/edgevision";    // config/ ota/ state/ logs/
  int keep_versions = 2;
  int free_space_min_mb = 256;
};

// ---- 磁盘原语（公开出来是为了单元测试能直接验证"原子性"这条不变量）----

// mkdir -p（已存在不算错）
bool MakeDirs(const std::string &path, std::string *err);
// 原子写：open(tmp) → write → fsync → close → rename → fsync(dir)（§6.3）
bool WriteFileAtomic(const std::string &path, const std::string &data,
                     std::string *err);
// 原子换符号链接：symlink(tmp) → rename(tmp, link) → fsync(dir)。
// **不要**先 unlink 再 symlink —— 中间窗口设备不可用（§6.3）
bool AtomicSymlink(const std::string &target, const std::string &link_path,
                   std::string *err);
std::string DirName(const std::string &path);
long long FreeSpaceMbOf(const std::string &path);

// A/B 目录（releases/ + current/previous 符号链接）的管理者。
//
// 职责边界：只回答"盘上现在是什么、能不能安全改成什么"；
// **不判断新版本健康与否**（那是 health_check + 状态机的事）。
class SlotManager {
 public:
  SlotManager();
  explicit SlotManager(const SlotConfig &cfg);

  void set_config(const SlotConfig &cfg) { cfg_ = cfg; }
  const SlotConfig &config() const { return cfg_; }

  // ---- 路径（纯字符串拼接，不碰磁盘）----
  std::string releases_dir() const;                       // install_root/releases
  std::string staging_dir() const;                        // install_root/.staging
  std::string current_link() const;                       // install_root/current
  std::string previous_link() const;                      // install_root/previous
  std::string release_dir(const std::string &version) const;
  std::string download_dir() const;                       // data_root/ota/download
  std::string state_file() const;                         // data_root/state/upgrade.json
  std::string config_dir() const;                         // data_root/config
  std::string logs_dir() const;                           // data_root/logs

  // ---- 已实现 ----
  bool EnsureLayout(std::string *err);
  // 读 current/previous 指向的版本名；指针不存在时 version 置空并返回 true
  bool ReadCurrentVersion(std::string *version, std::string *err) const;
  bool ReadPreviousVersion(std::string *version, std::string *err) const;
  // 原子切到 version：先落 previous（若 keep_previous），再 rename current
  bool SwitchCurrent(const std::string &version, bool keep_previous,
                     std::string *err);
  bool ReadState(UpgradeState *state, std::string *err) const;
  bool WriteState(const UpgradeState &state, std::string *err);
  long long FreeSpaceMb(const std::string &path) const;

  // ---- 骨架桩：P2/P3 补齐（§6.2 / §7），当前一律返回 kNotImpl ----
  bool ExtractPackage(const std::string &pkg_path, const std::string &version,
                      ErrorCode *code, std::string *err);
  bool VerifyStagedFiles(const std::string &version,
                         const std::vector<ManifestFile> &files, ErrorCode *code,
                         std::string *err);
  bool PruneOldReleases(std::string *err);
  bool ControlService(const std::string &unit, const std::string &action,
                      int timeout_sec, std::string *err);

 private:
  bool WriteVersionPointer(const std::string &link_path,
                           const std::string &version, std::string *err);
  SlotConfig cfg_;
};

}  // namespace upgrade
}  // namespace evs

#endif  // EVS_UPGRADE_SLOT_MANAGER_H
