#ifndef EVS_UPGRADE_MANIFEST_H
#define EVS_UPGRADE_MANIFEST_H

#include <string>
#include <vector>

#include "upgrade_types.h"

namespace evs {
namespace upgrade {

// manifest.json 的逐文件条目（§5.2 的 files[]，可选字段但推荐填）
struct ManifestFile {
  std::string path;    // 相对 payload/ 的路径，如 "bin/evs_media"（绝对路径 / ".." 一律拒）
  std::string sha256;  // 64 位十六进制
  std::string mode;    // "0755" / "0644"，格式化时透传
};

// manifest.json 的解析结果。字段语义见 docs/design/DESIGN_UPGRADE.md §5.2。
// 不认识的字段直接忽略（向前兼容，§9.2 同一原则）。
struct Manifest {
  int schema = 0;                 // 首期只认 1
  std::string version;            // MAJOR.MINOR.PATCH
  std::string git_commit;
  std::string built_at;           // ISO-8601 UTC，只做非空校验（不做时区解析）
  std::string hw;                 // target.hw，例如 "rk3588"
  std::string abi;                // target.abi，例如 "glibc-2.35"
  std::string min_from;           // target.min_from
  std::string payload_sha256;     // payload/ 打包流的 SHA-256
  long long size_bytes = 0;       // 下载前先查磁盘（§6.2 DOWNLOADING）
  std::vector<ManifestFile> files;
  int keep_versions = 2;          // rollback.keep_versions
  int boot_count_max = 3;         // rollback.boot_count_max
};

// 判定 target.* 与版本关系所需的本机信息
struct ManifestContext {
  std::string current_version;      // 当前 release 版本；空 = 没有可用版本（首装）
  std::string hw;                   // 期望值，例如 "rk3588"；空 = 不校验
  std::string abi;                  // 期望值，例如 "glibc-2.35"；空 = 不校验
  bool allow_downgrade = false;     // verify.allow_downgrade（只调试用）
};

// 解析清单文本。失败返回 false，*code 固定为 kSchema，err 指明**哪个字段**出错
// （例如 "target.abi: 缺失或不是字符串"）。
// 只做「字段完整性 + 取值范围」校验；验签是 verifier 的职责，不在本函数里。
bool ParseManifest(const std::string &text, Manifest *out, ErrorCode *code,
                   std::string *err);

// 语义化版本比较：a > b 返回 1，a < b 返回 -1，相等返回 0。
// 任一侧不是 MAJOR.MINOR.PATCH（各段纯数字）返回 2 —— 调用方按 kSchema 处理。
int CompareVersion(const std::string &a, const std::string &b);

// 兼容性 / 防降级判定（§5.5 的表）。返回 false 时 *code 指出具体原因，
// 调用方据此决定是「拒」还是「直接回 done（幂等）」。
bool CheckManifest(const Manifest &m, const ManifestContext &ctx, ErrorCode *code,
                   std::string *err);

// 序列化（写进 release 内留档 / 单元测试用）。排版为 2 空格缩进 + 结尾换行。
std::string ManifestToJson(const Manifest &m);

// 读文件全文（二进制安全：签名/包体里可能有 0x00）。失败返回 false 并写 err。
bool ReadFileBytes(const std::string &path, std::string *out, std::string *err);

}  // namespace upgrade
}  // namespace evs

#endif  // EVS_UPGRADE_MANIFEST_H
