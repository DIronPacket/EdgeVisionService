#include "manifest.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include <json.hpp>

namespace evs {
namespace upgrade {
namespace {

std::string Join(const std::string &where, const char *key) {
  if (where.empty()) return std::string(key);
  return where + "." + key;
}

// 必填字符串字段。缺失 / 类型不对 / 空串一律算错，err 里带完整字段路径。
bool RequireString(const nlohmann::json &o, const char *key,
                   const std::string &where, std::string *dst, std::string *err) {
  const std::string path = Join(where, key);
  if (!o.is_object() || !o.contains(key) || !o[key].is_string() ||
      o[key].get<std::string>().empty()) {
    if (err != nullptr) *err = path + ": 缺失、不是字符串或为空";
    return false;
  }
  *dst = o[key].get<std::string>();
  return true;
}

// 选填字符串字段：缺失就保持 dst 原值
void OptionalString(const nlohmann::json &o, const char *key, std::string *dst) {
  if (o.is_object() && o.contains(key) && o[key].is_string())
    *dst = o[key].get<std::string>();
}

// 选填整数字段：缺失 / 类型不对 / 越界就保持默认值
void OptionalInt(const nlohmann::json &o, const char *key, int min_value,
                 int max_value, int *dst) {
  if (!o.is_object() || !o.contains(key) || !o[key].is_number_integer()) return;
  const int v = o[key].get<int>();
  if (v < min_value || v > max_value) return;
  *dst = v;
}

bool IsHex64(const std::string &s) {
  if (s.size() != 64) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

std::string ToLowerCopy(const std::string &s) {
  std::string out = s;
  for (size_t i = 0; i < out.size(); ++i)
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
  return out;
}

bool SplitVersion(const std::string &v, std::vector<long long> *parts) {
  parts->clear();
  std::string cur;
  for (size_t i = 0; i <= v.size(); ++i) {
    if (i == v.size() || v[i] == '.') {
      if (cur.empty() || cur.size() > 9) return false;
      for (size_t k = 0; k < cur.size(); ++k) {
        if (!std::isdigit(static_cast<unsigned char>(cur[k]))) return false;
      }
      parts->push_back(std::atoll(cur.c_str()));
      cur.clear();
    } else {
      cur.push_back(v[i]);
    }
  }
  return parts->size() == 3;
}

// 路径必须是相对路径、不含 ".." 分量 —— 防止解包时被写到 payload/ 之外（§8.1）
bool IsSafeRelativePath(const std::string &path, std::string *why) {
  if (path.empty()) {
    if (why != nullptr) *why = "空路径";
    return false;
  }
  if (path[0] == '/') {
    if (why != nullptr) *why = "绝对路径";
    return false;
  }
  std::string cur;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (cur == "..") {
        if (why != nullptr) *why = "含 '..' 分量";
        return false;
      }
      cur.clear();
    } else {
      cur.push_back(path[i]);
    }
  }
  return true;
}

}  // namespace

bool ReadFileBytes(const std::string &path, std::string *out, std::string *err) {
  std::ifstream ifs(path.c_str(), std::ios::in | std::ios::binary);
  if (!ifs.is_open()) {
    if (err != nullptr) *err = "打不开文件: " + path;
    return false;
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  *out = oss.str();
  if (ifs.bad()) {
    if (err != nullptr) *err = "读文件出错: " + path;
    return false;
  }
  return true;
}

bool ParseManifest(const std::string &text, Manifest *out, ErrorCode *code,
                   std::string *err) {
  if (code != nullptr) *code = ErrorCode::kSchema;
  if (out == nullptr) {
    if (err != nullptr) *err = "Manifest 输出指针为空";
    return false;
  }

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(text);
  } catch (const std::exception &e) {
    if (err != nullptr) *err = std::string("JSON 语法错误: ") + e.what();
    return false;
  }
  if (!root.is_object()) {
    if (err != nullptr) *err = "清单根节点必须是对象";
    return false;
  }

  Manifest m;
  if (!root.contains("schema") || !root["schema"].is_number_integer()) {
    if (err != nullptr) *err = "schema: 缺失或不是整数";
    return false;
  }
  m.schema = root["schema"].get<int>();
  if (m.schema != 1) {
    if (err != nullptr)
      *err = "schema=" + std::to_string(m.schema) + " 不认识（本程序只支持 1）";
    return false;
  }

  if (!RequireString(root, "version", "", &m.version, err)) return false;
  if (CompareVersion(m.version, m.version) == 2) {
    if (err != nullptr) *err = "version='" + m.version + "' 不是 MAJOR.MINOR.PATCH";
    return false;
  }
  if (!RequireString(root, "git_commit", "", &m.git_commit, err)) return false;
  if (!RequireString(root, "built_at", "", &m.built_at, err)) return false;

  if (!root.contains("target") || !root["target"].is_object()) {
    if (err != nullptr) *err = "target: 缺失或不是对象";
    return false;
  }
  const nlohmann::json &target = root["target"];
  if (!RequireString(target, "hw", "target", &m.hw, err)) return false;
  if (!RequireString(target, "abi", "target", &m.abi, err)) return false;
  if (!RequireString(target, "min_from", "target", &m.min_from, err)) return false;
  if (CompareVersion(m.min_from, m.min_from) == 2) {
    if (err != nullptr)
      *err = "target.min_from='" + m.min_from + "' 不是 MAJOR.MINOR.PATCH";
    return false;
  }

  if (!RequireString(root, "payload_sha256", "", &m.payload_sha256, err))
    return false;
  m.payload_sha256 = ToLowerCopy(m.payload_sha256);
  if (!IsHex64(m.payload_sha256)) {
    if (err != nullptr) *err = "payload_sha256: 必须是 64 位十六进制";
    return false;
  }

  if (!root.contains("size_bytes") || !root["size_bytes"].is_number_integer() ||
      root["size_bytes"].get<long long>() <= 0) {
    if (err != nullptr) *err = "size_bytes: 缺失、不是整数或不大于 0";
    return false;
  }
  m.size_bytes = root["size_bytes"].get<long long>();

  if (root.contains("files")) {
    if (!root["files"].is_array()) {
      if (err != nullptr) *err = "files: 必须是数组";
      return false;
    }
    for (size_t i = 0; i < root["files"].size(); ++i) {
      const std::string where = "files[" + std::to_string(i) + "]";
      const nlohmann::json &item = root["files"][i];
      if (!item.is_object()) {
        if (err != nullptr) *err = where + ": 必须是对象";
        return false;
      }
      ManifestFile f;
      if (!RequireString(item, "path", where, &f.path, err)) return false;
      std::string why;
      if (!IsSafeRelativePath(f.path, &why)) {
        if (err != nullptr) *err = where + ".path='" + f.path + "': " + why;
        return false;
      }
      if (!RequireString(item, "sha256", where, &f.sha256, err)) return false;
      f.sha256 = ToLowerCopy(f.sha256);
      if (!IsHex64(f.sha256)) {
        if (err != nullptr) *err = where + ".sha256: 必须是 64 位十六进制";
        return false;
      }
      OptionalString(item, "mode", &f.mode);
      m.files.push_back(f);
    }
  }

  if (root.contains("rollback")) {
    if (!root["rollback"].is_object()) {
      if (err != nullptr) *err = "rollback: 必须是对象";
      return false;
    }
    OptionalInt(root["rollback"], "keep_versions", 1, 100, &m.keep_versions);
    OptionalInt(root["rollback"], "boot_count_max", 1, 100, &m.boot_count_max);
  }

  *out = m;
  if (code != nullptr) *code = ErrorCode::kNone;
  return true;
}

int CompareVersion(const std::string &a, const std::string &b) {
  std::vector<long long> va;
  std::vector<long long> vb;
  if (!SplitVersion(a, &va) || !SplitVersion(b, &vb)) return 2;
  for (size_t i = 0; i < 3; ++i) {
    if (va[i] > vb[i]) return 1;
    if (va[i] < vb[i]) return -1;
  }
  return 0;
}

bool CheckManifest(const Manifest &m, const ManifestContext &ctx, ErrorCode *code,
                   std::string *err) {
  if (code != nullptr) *code = ErrorCode::kNone;

  if (!ctx.hw.empty() && m.hw != ctx.hw) {
    if (code != nullptr) *code = ErrorCode::kHw;
    if (err != nullptr)
      *err = "target.hw='" + m.hw + "'，本机是 '" + ctx.hw + "'";
    return false;
  }
  if (!ctx.abi.empty() && m.abi != ctx.abi) {
    if (code != nullptr) *code = ErrorCode::kAbi;
    if (err != nullptr)
      *err = "target.abi='" + m.abi + "'，本机是 '" + ctx.abi + "'";
    return false;
  }

  // 首装（current 为空）不参与版本比较，但仍然要过上面的硬件 / ABI 判定
  if (ctx.current_version.empty()) return true;

  if (CompareVersion(ctx.current_version, m.min_from) == 2 ||
      CompareVersion(m.version, ctx.current_version) == 2) {
    if (code != nullptr) *code = ErrorCode::kSchema;
    if (err != nullptr)
      *err = "版本号无法比较（current='" + ctx.current_version + "' min_from='" +
             m.min_from + "'）";
    return false;
  }

  const int cmp = CompareVersion(m.version, ctx.current_version);
  if (cmp == 0) {
    // 同版本：调用方按「重复下发」处理（§5.5 最后一行）。这里不改成功/失败语义，
    // 只把原因说清楚，让上层能回 done / result=up-to-date。
    if (code != nullptr) *code = ErrorCode::kDowngrade;
    if (err != nullptr)
      *err = "清单版本 " + m.version + " 与当前版本相同（重复下发）";
    return false;
  }
  if (cmp < 0 && !ctx.allow_downgrade) {
    if (code != nullptr) *code = ErrorCode::kDowngrade;
    if (err != nullptr)
      *err = "清单版本 " + m.version + " 低于当前版本 " + ctx.current_version +
             "（allow_downgrade=false 时拒绝降级）";
    return false;
  }
  if (CompareVersion(ctx.current_version, m.min_from) < 0) {
    if (code != nullptr) *code = ErrorCode::kMinFrom;
    if (err != nullptr)
      *err = "当前版本 " + ctx.current_version + " 低于 target.min_from " +
             m.min_from + "（不允许跨大版本直接升）";
    return false;
  }
  return true;
}

std::string ManifestToJson(const Manifest &m) {
  nlohmann::json root;
  root["schema"] = m.schema;
  root["version"] = m.version;
  root["git_commit"] = m.git_commit;
  root["built_at"] = m.built_at;
  root["target"]["hw"] = m.hw;
  root["target"]["abi"] = m.abi;
  root["target"]["min_from"] = m.min_from;
  root["payload_sha256"] = m.payload_sha256;
  root["size_bytes"] = m.size_bytes;
  root["files"] = nlohmann::json::array();
  for (size_t i = 0; i < m.files.size(); ++i) {
    nlohmann::json f;
    f["path"] = m.files[i].path;
    f["sha256"] = m.files[i].sha256;
    if (!m.files[i].mode.empty()) f["mode"] = m.files[i].mode;
    root["files"].push_back(f);
  }
  root["rollback"]["keep_versions"] = m.keep_versions;
  root["rollback"]["boot_count_max"] = m.boot_count_max;
  return root.dump(2) + "\n";
}

}  // namespace upgrade
}  // namespace evs
