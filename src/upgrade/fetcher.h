#ifndef EVS_UPGRADE_FETCHER_H
#define EVS_UPGRADE_FETCHER_H

#include <string>

#include "upgrade_types.h"

namespace evs {
namespace upgrade {

// 取清单/取包的配置（对应 config/upgrade.json 的 source 段，§10.2）
struct FetchConfig {
  std::string type = "file";   // "file"（本地/U 盘导入）| "http"（含 https，走 curl）
  std::string base_url;        // 目录型源站：file 是本机目录，http 是 URL 前缀
  int timeout_sec = 30;        // 单次传输超时（curl -m）
  int retry = 3;               // 总尝试次数（>=1；指数退避 1s/2s/4s）
  std::string curl_bin = "curl";   // 方案 A（§5.6）：系统 curl，走 fork/execv
  std::string tmp_dir = "/tmp";    // FetchToString 的临时落盘目录（service 会改成 data_root 下）
};

// 取数据。**只负责搬运，不校验任何内容**（校验是 verifier / manifest 的职责）。
//
// 三条实现约束（都是踩过的坑）：
//   1. 绝不调用 system() / popen()：会多一层 shell 解析，URL 里的 & ? ; 会被 shell 吃掉，
//      也没法干净地把超时/退出码分开处理。一律 fork + execvp + waitpid。
//   2. 一律先写 <dst>.part，成功后 rename —— 断电只留半成品，不会出现"看起来下完了"。
//   3. https 走 curl 时用 -f：404/500 必须当失败，而不是把错误页当包体存下来。
class Fetcher {
 public:
  Fetcher();
  explicit Fetcher(const FetchConfig &cfg);

  void set_config(const FetchConfig &cfg) { cfg_ = cfg; }
  const FetchConfig &config() const { return cfg_; }

  // 逻辑名（"manifest.json" / 包文件名）→ 具体位置。
  // name 本身是绝对位置（/path、file://、http(s)://）时原样返回，便于 cmd 里带显式 url。
  bool Resolve(const std::string &name, std::string *location,
               std::string *err) const;

  // 取到内存（清单 / 签名，几 KB）。内部先落临时文件再读，
  // 保证「验签看到的字节」与「落盘的字节」完全一致。
  bool FetchToString(const std::string &name, std::string *out, ErrorCode *code,
                     std::string *err) const;

  // 取到文件（包体，几十 MB）。成功时 dst 已就位；失败时 dst 不存在，可能残留 .part。
  bool FetchToFile(const std::string &name, const std::string &dst,
                   ErrorCode *code, std::string *err) const;

 private:
  bool FetchLocationToString(const std::string &location, std::string *out,
                             ErrorCode *code, std::string *err) const;
  bool FetchLocationToFile(const std::string &location, const std::string &dst,
                           ErrorCode *code, std::string *err) const;

  FetchConfig cfg_;
};

}  // namespace upgrade
}  // namespace evs

#endif  // EVS_UPGRADE_FETCHER_H
