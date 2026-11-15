#include "slot_manager.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <json.hpp>

namespace evs {
namespace upgrade {
namespace {

// 把一个文件/目录 fsync 到盘：**这步最容易被省**，只 fsync 文件不 fsync 目录，
// 掉电后目录项仍可能丢，rename 等于没做（§6.3）。
void FsyncDirOf(const std::string &path) {
  const int fd = ::open(DirName(path).c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

bool WriteAll(int fd, const char *data, size_t size, std::string *err) {
  size_t written = 0;
  while (written < size) {
    const ssize_t n = ::write(fd, data + written, size - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (err != nullptr)
        *err = std::string("write 失败: ") + std::strerror(errno);
      return false;
    }
    written += static_cast<size_t>(n);
  }
  return true;
}

// 读 current/previous 指向的版本名。指针不存在不算错误（首装就是这种状态）。
bool ReadLinkVersion(const std::string &link_path, std::string *version,
                     std::string *err) {
  if (version == nullptr) return false;
  version->clear();
  char buf[512];
  const ssize_t n = ::readlink(link_path.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) {
    if (errno == ENOENT) return true;
    if (err != nullptr)
      *err = "读符号链接 " + link_path + " 失败: " + std::strerror(errno);
    return false;
  }
  buf[n] = '\0';
  const std::string target(buf);
  const size_t pos = target.find_last_of('/');
  *version = (pos == std::string::npos) ? target : target.substr(pos + 1);
  return true;
}

}  // namespace

std::string DirName(const std::string &path) {
  if (path.empty()) return std::string(".");
  std::string p = path;
  while (p.size() > 1 && p[p.size() - 1] == '/') p.erase(p.size() - 1);
  const size_t pos = p.find_last_of('/');
  if (pos == std::string::npos) return std::string(".");
  if (pos == 0) return std::string("/");
  return p.substr(0, pos);
}

bool MakeDirs(const std::string &path, std::string *err) {
  if (path.empty()) {
    if (err != nullptr) *err = "目录路径为空";
    return false;
  }
  std::string cur = (path[0] == '/') ? std::string("/") : std::string();
  size_t pos = (path[0] == '/') ? 1 : 0;
  while (pos <= path.size()) {
    size_t next = path.find('/', pos);
    if (next == std::string::npos) next = path.size();
    const std::string comp = path.substr(pos, next - pos);
    if (!comp.empty()) {
      if (cur.empty()) {
        cur = comp;
      } else if (cur == "/") {
        cur += comp;
      } else {
        cur += "/" + comp;
      }
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
        if (err != nullptr)
          *err = "创建目录 " + cur + " 失败: " + std::strerror(errno);
        return false;
      }
    }
    if (next == path.size()) break;
    pos = next + 1;
  }
  return true;
}

bool WriteFileAtomic(const std::string &path, const std::string &data,
                     std::string *err) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    if (err != nullptr)
      *err = "打开临时文件 " + tmp + " 失败: " + std::strerror(errno);
    return false;
  }
  bool ok = WriteAll(fd, data.data(), data.size(), err);
  if (ok && ::fsync(fd) != 0) {
    ok = false;
    if (err != nullptr)
      *err = std::string("fsync 临时文件失败: ") + std::strerror(errno);
  }
  ::close(fd);
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    if (err != nullptr)
      *err = "rename " + tmp + " → " + path + " 失败: " + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  FsyncDirOf(path);
  return true;
}

bool AtomicSymlink(const std::string &target, const std::string &link_path,
                   std::string *err) {
  const std::string tmp = link_path + ".tmp";
  ::unlink(tmp.c_str());
  if (::symlink(target.c_str(), tmp.c_str()) != 0) {
    if (err != nullptr)
      *err = "symlink " + tmp + " → " + target + " 失败: " + std::strerror(errno);
    return false;
  }
  if (::rename(tmp.c_str(), link_path.c_str()) != 0) {
    if (err != nullptr)
      *err = "rename " + tmp + " → " + link_path + " 失败: " +
             std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  FsyncDirOf(link_path);
  return true;
}

long long FreeSpaceMbOf(const std::string &path) {
  struct statvfs vfs;
  if (::statvfs(path.c_str(), &vfs) != 0) return -1;
  return static_cast<long long>(vfs.f_bavail) *
         static_cast<long long>(vfs.f_frsize) / (1024LL * 1024LL);
}

SlotManager::SlotManager() {}

SlotManager::SlotManager(const SlotConfig &cfg) : cfg_(cfg) {}

std::string SlotManager::releases_dir() const {
  return cfg_.install_root + "/releases";
}

std::string SlotManager::staging_dir() const {
  return cfg_.install_root + "/.staging";
}

std::string SlotManager::current_link() const {
  return cfg_.install_root + "/current";
}

std::string SlotManager::previous_link() const {
  return cfg_.install_root + "/previous";
}

std::string SlotManager::release_dir(const std::string &version) const {
  return releases_dir() + "/" + version;
}

std::string SlotManager::download_dir() const {
  return cfg_.data_root + "/ota/download";
}

std::string SlotManager::state_file() const {
  return cfg_.data_root + "/state/upgrade.json";
}

std::string SlotManager::config_dir() const {
  return cfg_.data_root + "/config";
}

std::string SlotManager::logs_dir() const { return cfg_.data_root + "/logs"; }

bool SlotManager::EnsureLayout(std::string *err) {
  const std::string dirs[] = {cfg_.install_root, releases_dir(), staging_dir(),
                              download_dir(),       DirName(state_file()),
                              config_dir(),         logs_dir()};
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
    if (!MakeDirs(dirs[i], err)) return false;
  }
  return true;
}

bool SlotManager::ReadCurrentVersion(std::string *version,
                                     std::string *err) const {
  return ReadLinkVersion(current_link(), version, err);
}

bool SlotManager::ReadPreviousVersion(std::string *version,
                                      std::string *err) const {
  return ReadLinkVersion(previous_link(), version, err);
}

bool SlotManager::WriteVersionPointer(const std::string &link_path,
                                      const std::string &version,
                                      std::string *err) {
  // 符号链接目标用**相对路径**（releases/1.4.2）：整个 install_root 被整体搬走时仍然有效
  return AtomicSymlink("releases/" + version, link_path, err);
}

bool SlotManager::SwitchCurrent(const std::string &version,
                                bool keep_previous, std::string *err) {
  if (version.empty()) {
    if (err != nullptr) *err = "目标版本为空";
    return false;
  }
  if (::access(release_dir(version).c_str(), F_OK) != 0) {
    if (err != nullptr)
      *err = "release 目录不存在: " + release_dir(version) +
             "（解包还没做？）";
    return false;
  }

  std::string current;
  if (!ReadCurrentVersion(&current, err)) return false;

  // 掉电安全顺序（§6.3）：先把 previous 指向老版本，再动 current。
  // 反过来的话，两次 rename 之间掉电会出现「current 已换、previous 还没记」→ 无法回滚。
  if (keep_previous && !current.empty() && current != version) {
    if (!WriteVersionPointer(previous_link(), current, err)) return false;
  }
  return WriteVersionPointer(current_link(), version, err);
}

bool SlotManager::ReadState(UpgradeState *state, std::string *err) const {
  if (state == nullptr) return false;
  *state = UpgradeState();

  std::string text;
  std::string rerr;
  if (!ReadFileBytes(state_file(), &text, &rerr)) {
    // 文件不存在 = 从来没升级过（首装）：不是错误，按 IDLE 处理
    if (::access(state_file().c_str(), F_OK) != 0) return true;
    if (err != nullptr) *err = rerr;
    return false;
  }

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(text);
  } catch (const std::exception &e) {
    if (err != nullptr)
      *err = "状态文件 JSON 语法错误（" + state_file() + "）: " + e.what();
    return false;
  }
  if (!j.is_object()) {
    if (err != nullptr) *err = "状态文件根节点必须是对象";
    return false;
  }

  std::string phase_text;
  if (j.contains("phase") && j["phase"].is_string())
    phase_text = j["phase"].get<std::string>();
  if (!ParsePhase(phase_text, &state->phase)) {
    if (err != nullptr) *err = "状态文件 phase 字段不认识: '" + phase_text + "'";
    return false;
  }
  if (j.contains("from") && j["from"].is_string())
    state->from = j["from"].get<std::string>();
  if (j.contains("to") && j["to"].is_string())
    state->to = j["to"].get<std::string>();
  if (j.contains("code") && j["code"].is_string())
    state->code = j["code"].get<std::string>();
  if (j.contains("message") && j["message"].is_string())
    state->message = j["message"].get<std::string>();
  if (j.contains("boot_count") && j["boot_count"].is_number_integer())
    state->boot_count = j["boot_count"].get<int>();
  if (j.contains("percent") && j["percent"].is_number_integer())
    state->percent = j["percent"].get<int>();
  if (j.contains("updated_at_ms") && j["updated_at_ms"].is_number_integer())
    state->updated_at_ms = j["updated_at_ms"].get<long long>();
  return true;
}

bool SlotManager::WriteState(const UpgradeState &state, std::string *err) {
  nlohmann::json j;
  j["phase"] = PhaseToPayload(state.phase);            // 人可读
  j["phase_id"] = static_cast<int>(state.phase);       // 程序读（枚举顺序冻结）
  j["from"] = state.from;
  j["to"] = state.to;
  j["boot_count"] = state.boot_count;
  j["percent"] = state.percent;
  j["code"] = state.code;
  j["message"] = state.message;
  j["updated_at_ms"] = state.updated_at_ms > 0 ? state.updated_at_ms : NowMs();

  std::string mkerr;
  if (!MakeDirs(DirName(state_file()), &mkerr)) {
    if (err != nullptr) *err = mkerr;
    return false;
  }
  return WriteFileAtomic(state_file(), j.dump(2) + "\n", err);
}

long long SlotManager::FreeSpaceMb(const std::string &path) const {
  return FreeSpaceMbOf(path);
}

// ================= 骨架桩（P2/P3 补齐）=================
//
// 四个桩的写法统一：返回 false + kNotImpl + err 里写清楚"该去哪里看实现要求"。
// 这样状态机走到这一步会以 E_NOTIMPL 收尾并如实上报，**不会假装成功**。

bool SlotManager::ExtractPackage(const std::string &pkg_path,
                                 const std::string &version, ErrorCode *code,
                                 std::string *err) {
  (void)pkg_path;
  if (code != nullptr) *code = ErrorCode::kNotImpl;
  if (err != nullptr)
    *err = "解包尚未实现（骨架）：P2 用 tar -xzf 解到 " + staging_dir() + "/" +
           version + "，逐文件 fsync，再按 manifest.files[] 校验 sha256/mode";
  return false;
}

bool SlotManager::VerifyStagedFiles(const std::string &version,
                                    const std::vector<ManifestFile> &files,
                                    ErrorCode *code, std::string *err) {
  (void)files;
  if (code != nullptr) *code = ErrorCode::kNotImpl;
  if (err != nullptr)
    *err = "暂存区逐文件校验尚未实现（骨架）：P2 对 " + staging_dir() + "/" +
           version + " 下每个文件算 sha256 并与清单比对";
  return false;
}

bool SlotManager::PruneOldReleases(std::string *err) {
  if (err != nullptr)
    *err = "旧版本清理尚未实现（骨架）：P3 按 keep_versions=" +
           std::to_string(cfg_.keep_versions) +
           " 删除最老的 release（永不删 current / previous）";
  return false;
}

bool SlotManager::ControlService(const std::string &unit,
                                 const std::string &action, int timeout_sec,
                                 std::string *err) {
  (void)timeout_sec;
  if (err != nullptr)
    *err = "systemctl " + action + " " + unit +
           " 尚未实现（骨架）：P3 用 fork/execv 调 systemctl，带超时；"
           "不要用 system()，也不要假设 ProtectSystem=strict 下 D-Bus 一定通";
  return false;
}

}  // namespace upgrade
}  // namespace evs
