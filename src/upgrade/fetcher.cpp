#include "fetcher.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "manifest.h"  // ReadFileBytes

namespace evs {
namespace upgrade {
namespace {

bool StartsWith(const std::string &s, const char *prefix) {
  const size_t n = std::strlen(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// name 是不是"绝对位置"（不再拼 base_url）
bool IsAbsoluteLocation(const std::string &name) {
  return StartsWith(name, "http://") || StartsWith(name, "https://") ||
         StartsWith(name, "file://") || (!name.empty() && name[0] == '/');
}

std::string StripFileScheme(const std::string &loc) {
  return StartsWith(loc, "file://") ? loc.substr(7) : loc;
}

std::string JoinLocation(const std::string &base, const std::string &name) {
  if (base.empty()) return name;
  if (base[base.size() - 1] == '/') return base + name;
  return base + "/" + name;
}

bool IsHttpLocation(const std::string &loc) {
  return StartsWith(loc, "http://") || StartsWith(loc, "https://");
}

// 指数退避：第 1 次失败等 1s，第 2 次 2s，第 3 次 4s（上限 4s）
void BackoffSleep(int attempt_index) {
  int ms = 1000;
  for (int i = 1; i < attempt_index && i < 3; ++i) ms *= 2;
  struct timespec ts;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

// 本机文件复制：open/write/fsync/close。写成 .part 由调用方 rename。
bool CopyLocalFile(const std::string &src, const std::string &dst,
                   std::string *err) {
  const int in = ::open(src.c_str(), O_RDONLY);
  if (in < 0) {
    if (err != nullptr) *err = "源文件打不开: " + src;
    return false;
  }
  const int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) {
    ::close(in);
    if (err != nullptr) *err = "目标文件打不开: " + dst;
    return false;
  }

  bool ok = true;
  std::vector<char> buf(64 * 1024);
  for (;;) {
    const ssize_t got = ::read(in, &buf[0], buf.size());
    if (got < 0) {
      if (errno == EINTR) continue;
      if (err != nullptr) *err = "读源文件出错: " + std::string(std::strerror(errno));
      ok = false;
      break;
    }
    if (got == 0) break;
    ssize_t written = 0;
    while (written < got) {
      const ssize_t n = ::write(out, &buf[0] + written,
                                static_cast<size_t>(got - written));
      if (n < 0) {
        if (errno == EINTR) continue;
        if (err != nullptr)
          *err = "写目标文件出错: " + std::string(std::strerror(errno));
        ok = false;
        break;
      }
      written += n;
    }
    if (!ok) break;
  }
  // 落盘顺序：先 fsync 文件，再由调用方 rename + fsync 目录（§6.3）
  if (ok && ::fsync(out) != 0) {
    if (err != nullptr)
      *err = "fsync 目标文件出错: " + std::string(std::strerror(errno));
    ok = false;
  }
  ::close(in);
  ::close(out);
  if (!ok) ::unlink(dst.c_str());
  return ok;
}

}  // namespace

Fetcher::Fetcher() {}

Fetcher::Fetcher(const FetchConfig &cfg) : cfg_(cfg) {}

bool Fetcher::Resolve(const std::string &name, std::string *location,
                      std::string *err) const {
  if (location == nullptr) return false;
  if (name.empty()) {
    if (err != nullptr) *err = "逻辑名为空";
    return false;
  }
  if (IsAbsoluteLocation(name)) {
    *location = name;
    return true;
  }
  if (cfg_.base_url.empty()) {
    if (err != nullptr) *err = "source.base_url 为空，无法拼出位置";
    return false;
  }
  *location = JoinLocation(cfg_.base_url, name);
  return true;
}

namespace {

// 跑一次 curl（不经 shell）。返回 false 只表示"没跑起来"，退出码走 *exit_code。
bool RunCurl(const std::string &curl_bin, const std::vector<std::string> &args,
             int *exit_code, std::string *err) {
  std::vector<char *> argv;
  argv.reserve(args.size() + 2);
  argv.push_back(const_cast<char *>(curl_bin.c_str()));
  for (size_t i = 0; i < args.size(); ++i)
    argv.push_back(const_cast<char *>(args[i].c_str()));
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    if (err != nullptr) *err = std::string("fork 失败: ") + std::strerror(errno);
    return false;
  }
  if (pid == 0) {
    // 子进程：只 exec。失败用 _exit（不要触发父进程的 atexit / 崩溃 handler）
    ::execvp(argv[0], &argv[0]);
    ::_exit(127);
  }

  int status = 0;
  for (;;) {
    const pid_t r = ::waitpid(pid, &status, 0);
    if (r == pid) break;
    if (r < 0 && errno == EINTR) continue;
    if (err != nullptr) *err = std::string("waitpid 失败: ") + std::strerror(errno);
    return false;
  }
  if (!WIFEXITED(status)) {
    *exit_code = -1;
    return true;
  }
  *exit_code = WEXITSTATUS(status);
  return true;
}

ErrorCode MapCurlExit(int exit_code, const std::string &location,
                      const std::string &curl_bin, std::string *err) {
  std::string msg;
  ErrorCode code = ErrorCode::kNet;
  switch (exit_code) {
    case 0:
      return ErrorCode::kNone;
    case 22:
      code = ErrorCode::kHttp;
      msg = "HTTP 返回非 2xx（404/500/鉴权失败）: " + location;
      break;
    case 6:
      msg = "DNS 解析失败: " + location;
      break;
    case 7:
      msg = "连接不上: " + location;
      break;
    case 28:
      msg = "传输超时: " + location;
      break;
    case 35:
      msg = "TLS 握手失败: " + location;
      break;
    case 56:
      msg = "接收数据失败（连接被重置）: " + location;
      break;
    case 127:
      msg = "找不到可执行的 curl（source.curl_bin='" + curl_bin +
            "'）；方案 A 需要系统 curl，见 §5.6";
      break;
    default:
      msg = "curl 退出码 " + std::to_string(exit_code) + ": " + location;
      break;
  }
  if (err != nullptr) *err = msg;
  return code;
}

bool FetchHttpOnce(const FetchConfig &cfg, const std::string &location,
                   const std::string &part, ErrorCode *code, std::string *err) {
  std::vector<std::string> args;
  args.push_back("-fsSL");   // -f：HTTP 非 2xx 当失败；-sS：安静但保留错误；-L：跟重定向
  args.push_back("--connect-timeout");
  args.push_back(std::to_string(cfg.timeout_sec > 10 ? 10 : cfg.timeout_sec));
  args.push_back("-m");
  args.push_back(std::to_string(cfg.timeout_sec));
  args.push_back("-o");
  args.push_back(part);
  // 已存在 .part 就走断点续传（骨架期只覆盖 http(s) 这一路的 -C -）
  struct stat st;
  if (::stat(part.c_str(), &st) == 0 && st.st_size > 0) {
    args.push_back("-C");
    args.push_back("-");
  }
  args.push_back(location);

  int exit_code = -1;
  std::string run_err;
  if (!RunCurl(cfg.curl_bin, args, &exit_code, &run_err)) {
    if (code != nullptr) *code = ErrorCode::kNet;
    if (err != nullptr) *err = run_err;
    return false;
  }
  const ErrorCode mapped = MapCurlExit(exit_code, location, cfg.curl_bin, err);
  if (code != nullptr) *code = mapped;
  return mapped == ErrorCode::kNone;
}

}  // namespace

bool Fetcher::FetchLocationToFile(const std::string &location,
                                  const std::string &dst, ErrorCode *code,
                                  std::string *err) const {
  if (code != nullptr) *code = ErrorCode::kNone;
  const int attempts = cfg_.retry > 0 ? cfg_.retry : 1;
  const std::string part = dst + ".part";

  ErrorCode last_code = ErrorCode::kNone;
  std::string last_err;
  for (int i = 0; i < attempts; ++i) {
    if (i > 0) {
      std::printf("[upgrade] 取数失败，第 %d/%d 次重试（%s）\n", i + 1, attempts,
                  last_err.c_str());
      std::fflush(stdout);
      BackoffSleep(i);
    }

    ErrorCode one_code = ErrorCode::kNone;
    std::string one_err;
    bool ok = false;
    if (IsHttpLocation(location)) {
      ok = FetchHttpOnce(cfg_, location, part, &one_code, &one_err);
    } else {
      // 本地 / U 盘导入：直接拷（同样先写 .part，保证下游看到的是"要么没有、要么完整"）
      ok = CopyLocalFile(StripFileScheme(location), part, &one_err);
      one_code = ok ? ErrorCode::kNone : ErrorCode::kNet;
    }

    if (ok) {
      if (::rename(part.c_str(), dst.c_str()) != 0) {
        if (code != nullptr) *code = ErrorCode::kApply;
        if (err != nullptr)
          *err = "rename " + part + " → " + dst + " 失败: " +
                 std::strerror(errno);
        ::unlink(part.c_str());
        return false;
      }
      return true;
    }
    last_code = one_code;
    last_err = one_err;
  }

  if (code != nullptr) *code = last_code;
  if (err != nullptr)
    *err = last_err + "（已尝试 " + std::to_string(attempts) + " 次）";
  return false;
}

bool Fetcher::FetchLocationToString(const std::string &location, std::string *out,
                                    ErrorCode *code, std::string *err) const {
  if (out == nullptr) return false;
  char tmp[512];
  std::snprintf(tmp, sizeof(tmp), "%s/evs-fetch-%ld-%lld.tmp",
                cfg_.tmp_dir.c_str(), static_cast<long>(::getpid()), NowMs());
  const std::string tmp_path(tmp);

  if (!FetchLocationToFile(location, tmp_path, code, err)) {
    ::unlink(tmp_path.c_str());
    return false;
  }
  const bool ok = ReadFileBytes(tmp_path, out, err);
  ::unlink(tmp_path.c_str());
  if (!ok && code != nullptr) *code = ErrorCode::kApply;
  return ok;
}

bool Fetcher::FetchToString(const std::string &name, std::string *out,
                            ErrorCode *code, std::string *err) const {
  std::string location;
  if (!Resolve(name, &location, err)) {
    if (code != nullptr) *code = ErrorCode::kNet;
    return false;
  }
  return FetchLocationToString(location, out, code, err);
}

bool Fetcher::FetchToFile(const std::string &name, const std::string &dst,
                          ErrorCode *code, std::string *err) const {
  if (dst.empty()) {
    if (code != nullptr) *code = ErrorCode::kApply;
    if (err != nullptr) *err = "目标路径为空";
    return false;
  }
  std::string location;
  if (!Resolve(name, &location, err)) {
    if (code != nullptr) *code = ErrorCode::kNet;
    return false;
  }
  return FetchLocationToFile(location, dst, code, err);
}

}  // namespace upgrade
}  // namespace evs
