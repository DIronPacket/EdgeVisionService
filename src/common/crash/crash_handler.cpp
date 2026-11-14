// ============================================================
// 崩溃现场采集（crash_handler）
//
// 设计约束：崩溃处理跑在信号上下文里，只能用 async-signal-safe 的调用。
// 因此这里刻意不用 snprintf / std::string / std::cerr / malloc：
//   - 数字转字符串、时间格式化全部手写（纯算术）
//   - 路径、tag 在 InstallCrashHandler() 阶段预格式化进静态缓冲
//   - 写文件只用 open/write，backtrace_symbols_fd 内部也是 write
// 唯一不严格保证的是 backtrace()（glibc 首次调用会初始化 unwind 缓存），
// 所以在 Install 阶段做了一次 warm-up。
//
// 另一条关键设计：报告写完后「把同一个信号再抛一次」。因为装了
// SA_RESETHAND，进入 handler 时该信号已被恢复成 SIG_DFL，二次抛出就由内核
// 按 /proc/sys/kernel/core_pattern 产生 core —— 这样既有可读报告，又有能用
// gdb 查看局部变量的 core。
// ============================================================

#include "crash_handler.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <typeinfo>

#include <cxxabi.h>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

namespace evs {
namespace common {
namespace {

const int kMaxFrames = 64;
const int kMaxTags = 8;
const size_t kTagLen = 160;
const int kMaxTrappedSignals = 8;

char g_app[64] = {0};
char g_exe[256] = {0};
char g_dir[224] = {0};
char g_tags[kMaxTags][kTagLen] = {{0}};
int g_tag_count = 0;
bool g_dump_core = true;

// 递归保护：handler 自己再崩（栈已毁 / 地址无效）时不能再进 handler
volatile sig_atomic_t g_in_handler = 0;

// 保护 tags 与安装过程（信号处理本身不加锁）
std::mutex g_state_mutex;

bool g_installed = false;
int g_trapped_sig[kMaxTrappedSignals] = {0};
struct sigaction g_old_action[kMaxTrappedSignals];
int g_trapped_count = 0;

// 备用信号栈：栈溢出导致的 SIGSEGV 在坏栈上跑 handler 会二次崩溃。
// 注意 sigaltstack 是按线程的，这里只覆盖调用 Install 的线程（主线程）。
char g_alt_stack[64 * 1024];

// ---------- async-signal-safe 的字符串/数字拼接 ----------

char *AppendStr(char *p, char *end, const char *s) {
  if (!s) return p;
  while (*s != '\0' && (p + 1) < end) *p++ = *s++;
  *p = '\0';
  return p;
}

char *AppendUInt(char *p, char *end, unsigned long v) {
  char tmp[24];
  int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v > 0 && n < static_cast<int>(sizeof(tmp))) {
    tmp[n++] = static_cast<char>('0' + (v % 10));
    v /= 10;
  }
  while (n > 0 && (p + 1) < end) *p++ = tmp[--n];
  *p = '\0';
  return p;
}

char *AppendHex(char *p, char *end, unsigned long v) {
  static const char kDigits[] = "0123456789abcdef";
  char tmp[20];
  int n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v > 0 && n < static_cast<int>(sizeof(tmp))) {
    tmp[n++] = kDigits[v & 0xful];
    v >>= 4;
  }
  while (n > 0 && (p + 1) < end) *p++ = tmp[--n];
  *p = '\0';
  return p;
}

void WriteAll(int fd, const char *s, size_t n) {
  while (n > 0) {
    ssize_t w = write(fd, s, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if (w == 0) return;
    s += w;
    n -= static_cast<size_t>(w);
  }
}

void WriteStr(int fd, const char *s) { WriteAll(fd, s, strlen(s)); }

// strsignal() 不在 async-signal-safe 列表里，自己映射常用致命信号
const char *SignalName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV (invalid memory access / 段错误)";
    case SIGBUS:  return "SIGBUS (bus error / 对齐或映射错误)";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGFPE:  return "SIGFPE (arithmetic exception / 除零)";
    case SIGABRT: return "SIGABRT (abort / 断言失败 / 未捕获异常)";
    case SIGQUIT: return "SIGQUIT (requested dump / kill -QUIT)";
    default:      return "unknown fatal signal";
  }
}

// epoch 秒 → "YYYY-MM-DD HH:MM:SS UTC"。
// 不能用 localtime_r：它会读时区文件（非 async-signal-safe）。这里用
// Howard Hinnant 的 civil_from_days 纯算术实现（无分支表、无时区）。
char *AppendUtc(char *p, char *end, long long secs) {
  long long days = secs / 86400;
  long long rem = secs % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  const long long z = days + 719468;
  const long long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned long long doe =
      static_cast<unsigned long long>(z - era * 146097);
  const unsigned long long yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const long long y = static_cast<long long>(yoe) + era * 400;
  const unsigned long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned long long mp = (5 * doy + 2) / 153;
  const unsigned long long d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned long long m = mp + (mp < 10 ? 3 : -9);
  const long long year = y + (m <= 2);

  char ybuf[8];
  long long yv = year;
  for (int i = 3; i >= 0; --i) {
    ybuf[i] = static_cast<char>('0' + (yv % 10));
    yv /= 10;
  }
  ybuf[4] = '\0';
  p = AppendStr(p, end, ybuf);
  p = AppendStr(p, end, "-");
  if (m < 10) p = AppendStr(p, end, "0");
  p = AppendUInt(p, end, static_cast<unsigned long>(m));
  p = AppendStr(p, end, "-");
  if (d < 10) p = AppendStr(p, end, "0");
  p = AppendUInt(p, end, static_cast<unsigned long>(d));
  p = AppendStr(p, end, " ");

  const long long hh = rem / 3600;
  const long long mm = (rem % 3600) / 60;
  const long long ss = rem % 60;
  if (hh < 10) p = AppendStr(p, end, "0");
  p = AppendUInt(p, end, static_cast<unsigned long>(hh));
  p = AppendStr(p, end, ":");
  if (mm < 10) p = AppendStr(p, end, "0");
  p = AppendUInt(p, end, static_cast<unsigned long>(mm));
  p = AppendStr(p, end, ":");
  if (ss < 10) p = AppendStr(p, end, "0");
  p = AppendUInt(p, end, static_cast<unsigned long>(ss));
  return AppendStr(p, end, " UTC");
}

// 报告文件名：<dir>/crash_<app>_<pid>_<epoch>_sig<sig>.log
// 文件名自带程序名 + pid + 时间，符合「一眼看出是哪个程序」的要求；
// 用 epoch 而非格式化时间是为了 handler 里少一次日期换算。
void BuildReportPath(char *out, size_t cap, int sig) {
  char *p = out;
  char *end = out + cap;
  p = AppendStr(p, end, g_dir[0] ? g_dir : ".");
  p = AppendStr(p, end, "/crash_");
  p = AppendStr(p, end, g_app);
  p = AppendStr(p, end, "_");
  p = AppendUInt(p, end, static_cast<unsigned long>(getpid()));
  p = AppendStr(p, end, "_");
  p = AppendUInt(p, end, static_cast<unsigned long>(time(NULL)));
  p = AppendStr(p, end, "_sig");
  p = AppendUInt(p, end, static_cast<unsigned long>(sig));
  p = AppendStr(p, end, ".log");
}

// ---------- 崩溃 handler 本体 ----------
//
// 进入条件：致命信号（SEGV/BUS/ILL/FPE/ABRT/QUIT）。此时进程状态已不可信，
// 只做四件事：写报告 → 写调用栈 → 写 gdb 使用提示 → 重新抛信号让内核落 core。
void OnFatalSignal(int sig, siginfo_t *info, void *raw_ctx) {
  if (g_in_handler) {
    // 已在校验现场又崩一次（很可能栈/堆已毁），不再尝试任何格式化
    syscall(SYS_tgkill, static_cast<pid_t>(getpid()),
            static_cast<pid_t>(syscall(SYS_gettid)), sig);
    _exit(128 + sig);
  }
  g_in_handler = 1;

  char path[512];
  BuildReportPath(path, sizeof(path), sig);

  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) fd = STDERR_FILENO;  // 目录不可写时至少进终端 / journal

  char buf[2048];
  char *end = buf + sizeof(buf);
  char *p = buf;

  p = AppendStr(p, end, "\n==== EdgeVisionService CRASH REPORT ====\n");
  p = AppendStr(p, end, "app        = ");
  p = AppendStr(p, end, g_app);
  p = AppendStr(p, end, "\nexe        = ");
  p = AppendStr(p, end, g_exe[0] ? g_exe : "(readlink /proc/self/exe failed)");
  p = AppendStr(p, end, "\npid        = ");
  p = AppendUInt(p, end, static_cast<unsigned long>(getpid()));
  p = AppendStr(p, end, "   tid = ");
  p = AppendUInt(p, end, static_cast<unsigned long>(syscall(SYS_gettid)));
  p = AppendStr(p, end, "\ntime       = ");
  p = AppendUtc(p, end, static_cast<long long>(time(NULL)));
  p = AppendStr(p, end, "\nsignal     = ");
  p = AppendStr(p, end, SignalName(sig));
  p = AppendStr(p, end, " [");
  p = AppendUInt(p, end, static_cast<unsigned long>(sig));
  p = AppendStr(p, end, "]");
  if (info) {
    p = AppendStr(p, end, "\nsi_code    = ");
    p = AppendUInt(p, end, static_cast<unsigned long>(info->si_code));
    p = AppendStr(p, end, "   fault_addr = 0x");
    p = AppendHex(p, end,
                  static_cast<unsigned long>(reinterpret_cast<uintptr_t>(info->si_addr)));
  }

  // 寄存器：PC 指向「哪一行指令」，FP/LR 是 gdb 之外最快的粗定位手段
  {
    const ucontext_t *uc = static_cast<const ucontext_t *>(raw_ctx);
    if (uc) {
#if defined(__aarch64__)
      p = AppendStr(p, end, "\nregisters  = pc=0x");
      p = AppendHex(p, end, static_cast<unsigned long>(uc->uc_mcontext.pc));
      p = AppendStr(p, end, " sp=0x");
      p = AppendHex(p, end, static_cast<unsigned long>(uc->uc_mcontext.sp));
      p = AppendStr(p, end, " lr=0x");
      p = AppendHex(p, end, static_cast<unsigned long>(uc->uc_mcontext.regs[30]));
      p = AppendStr(p, end, " fp=0x");
      p = AppendHex(p, end, static_cast<unsigned long>(uc->uc_mcontext.regs[29]));
#elif defined(__x86_64__)
      p = AppendStr(p, end, "\nregisters  = rip=0x");
      p = AppendHex(p, end, static_cast<unsigned long>(uc->uc_mcontext.gregs[REG_RIP]));
      p = AppendStr(p, end, " rsp=0x");
      p = AppendHex(p, end, static_cast<unsigned long>(uc->uc_mcontext.gregs[REG_RSP]));
      p = AppendStr(p, end, " rbp=0x");
      p = AppendHex(p, end, static_cast<unsigned long>(uc->uc_mcontext.gregs[REG_RBP]));
#else
      (void)uc;
#endif
    }
  }

  // 业务上下文（建链时的链路 id / 配置路径等），由 SetCrashTag 预先写入
  p = AppendStr(p, end, "\n-- crash tags --\n");
  for (int i = 0; i < g_tag_count; ++i) {
    p = AppendStr(p, end, "  ");
    p = AppendStr(p, end, g_tags[i]);
    p = AppendStr(p, end, "\n");
  }

  WriteAll(fd, buf, static_cast<size_t>(p - buf));
  if (fd != STDERR_FILENO) WriteAll(STDERR_FILENO, buf, static_cast<size_t>(p - buf));

  // 调用栈：backtrace_symbols_fd 直接 write 到 fd，不需要 malloc
  WriteStr(fd, "-- call stack (模块(符号+偏移) [地址]) --\n");
  WriteStr(fd, "   注: 前 2 帧是 crash handler 自身与 __kernel_rt_sigreturn，\n"
               "       真正的崩溃现场从其后开始\n");
  void *frames[kMaxFrames];
  int n = backtrace(frames, kMaxFrames);
  backtrace_symbols_fd(frames, n, fd);
  if (fd != STDERR_FILENO) backtrace_symbols_fd(frames, n, STDERR_FILENO);

  WriteStr(fd,
           "-- 下一步 --\n"
           "  1) 地址 -> 函数/文件:行号: scripts/coredump/symbolize_report.sh <本报告>\n"
           "  2) 看变量（需要 core）: gdb <exe> <core>\n"
           "       (gdb) thread apply all bt full\n"
           "       (gdb) frame <崩溃帧号>; info locals; info args; p *this\n"
           "     core 位置由 /proc/sys/kernel/core_pattern 决定；\n"
           "     没找到就执行 scripts/coredump/enable_coredump.sh 后再复现\n");

  if (g_dump_core) {
    // 【关键】handler 执行期间，当前信号被内核自动屏蔽（没设 SA_NODEFER）。
    // 若直接 tgkill 再 _exit，信号只会挂成 pending，然后被 _exit 丢掉，
    // core 一个字节都不会有 —— 本机实测踩过。必须先解除屏蔽。
    sigset_t unblock_one;
    sigemptyset(&unblock_one);
    sigaddset(&unblock_one, sig);
    sigprocmask(SIG_UNBLOCK, &unblock_one, NULL);

    // SA_RESETHAND 已把该信号恢复为 SIG_DFL：向崩溃线程重抛 → 内核按
    // core_pattern 写 core（同一份现场同时有报告和 core）
    syscall(SYS_tgkill, static_cast<pid_t>(getpid()),
            static_cast<pid_t>(syscall(SYS_gettid)), sig);
    // 正常情况内核在 tgkill 返回用户态前就按 SIG_DFL 终止进程，走不到这里；
    // 保留 _exit 兜底，避免 tgkill 失败时进程带伤继续跑。
  }
  _exit(128 + sig);
}

// std::terminate：未捕获异常 / 线程函数抛异常 / noexcept 违规。
// 默认行为只是 abort()，不打印异常类型与栈，等于什么都没留下。
void OnTerminate() {
  WriteStr(STDERR_FILENO,
           "\n[crash] std::terminate called "
           "(uncaught exception / noexcept violation)\n");
  std::exception_ptr ep = std::current_exception();
  if (ep) {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception &e) {
      int status = 0;
      char *demangled =
          abi::__cxa_demangle(typeid(e).name(), NULL, NULL, &status);
      std::fprintf(stderr, "[crash] exception type: %s\n",
                   demangled ? demangled : typeid(e).name());
      std::fprintf(stderr, "[crash] what(): %s\n", e.what());
      std::free(demangled);
    } catch (...) {
      WriteStr(STDERR_FILENO, "[crash] exception type: (non std::exception)\n");
    }
  } else {
    WriteStr(STDERR_FILENO,
             "[crash] exception: (none; terminate called directly)\n");
  }
  PrintBacktrace("std::terminate");
  // abort() → SIGABRT → OnFatalSignal → 报告 + core。
  // 部分 libc 的 abort 会先把 SIGABRT 恢复默认动作，那样报告写不出来，
  // 但上面已经把异常类型与栈打到 stderr，关键信息不丢。
  std::abort();
}

}  // namespace

bool InstallCrashHandler(const CrashConfig &cfg) {
  std::lock_guard<std::mutex> lock(g_state_mutex);

  const std::string app = cfg.app_name.empty() ? std::string("app") : cfg.app_name;
  std::snprintf(g_app, sizeof(g_app), "%s", app.c_str());
  g_dump_core = cfg.dump_core;

  // exe 绝对路径在安装阶段解析：handler 里少一次系统调用，报告里也能直接给出
  // 「用哪个二进制去 gdb」。
  const ssize_t n = readlink("/proc/self/exe", g_exe, sizeof(g_exe) - 1);
  const size_t exe_len = (n > 0) ? static_cast<size_t>(n) : 0;
  g_exe[exe_len] = '\0';

  // 报告目录：cfg.report_dir → $EVS_CRASH_DIR → ./crash
  std::string dir = cfg.report_dir;
  if (dir.empty()) {
    const char *env = std::getenv("EVS_CRASH_DIR");
    if (env && env[0] != '\0') dir = env;
  }
  if (dir.empty()) dir = "crash";
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    std::fprintf(stderr, "[crash] 无法创建报告目录 %s (%s)，改用 /tmp\n",
                 dir.c_str(), std::strerror(errno));
    dir = "/tmp";
  }
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir.c_str());

  // 尽力把 RLIMIT_CORE 抬到 hard limit：默认 soft 常常是 0（本机就是），
  // 那样即使装了 handler，内核也不会写 core，「看变量」这一步直接断掉。
  // 只能抬到 hard 上限，hard=0 时需要用户执行 ulimit -c unlimited（见脚本）。
  struct rlimit rl;
  bool core_enabled = false;
  if (getrlimit(RLIMIT_CORE, &rl) == 0) {
    if (rl.rlim_cur != rl.rlim_max) {
      struct rlimit want = rl;
      want.rlim_cur = rl.rlim_max;
      if (setrlimit(RLIMIT_CORE, &want) == 0) rl = want;
    }
    core_enabled = (rl.rlim_cur > 0);
  }

  // 被 setuid / 容器 / 父进程改过 dumpable 时，内核仍不会写 core
  prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

  // 备用信号栈：栈溢出触发的 SIGSEGV 若还在坏栈上跑 handler 会二次崩溃。
  // 注意 sigaltstack 是按线程生效，这里只覆盖调用本函数的线程（主线程）。
  stack_t ss;
  std::memset(&ss, 0, sizeof(ss));
  ss.ss_sp = g_alt_stack;
  ss.ss_size = sizeof(g_alt_stack);
  ss.ss_flags = 0;
  if (sigaltstack(&ss, NULL) != 0)
    std::fprintf(stderr, "[crash] sigaltstack 失败: %s\n", std::strerror(errno));

  // backtrace warm-up：首次调用会初始化 unwind 缓存（可能分配内存），
  // 必须在 handler 之外做完，否则崩溃现场可能自己再死一次。
  void *warm[1];
  (void)backtrace(warm, 1);

  int sigs[kMaxTrappedSignals];
  int count = 0;
  sigs[count++] = SIGSEGV;
  sigs[count++] = SIGBUS;
  sigs[count++] = SIGILL;
  sigs[count++] = SIGFPE;
  sigs[count++] = SIGABRT;
  if (cfg.trap_sigquit) sigs[count++] = SIGQUIT;

  // SA_RESETHAND：进入 handler 时该信号恢复默认动作 → handler 里重抛就会
  // 由内核按 core_pattern 写 core，而不是再进一次 handler。
  // SA_ONSTACK：handler 跑在 g_alt_stack 上。
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = &OnFatalSignal;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);

  bool ok = true;
  g_trapped_count = 0;
  for (int i = 0; i < count; ++i) {
    if (sigaction(sigs[i], &sa, &g_old_action[g_trapped_count]) == 0) {
      g_trapped_sig[g_trapped_count] = sigs[i];
      ++g_trapped_count;
    } else {
      ok = false;
      std::fprintf(stderr, "[crash] 安装信号 %d 失败: %s\n", sigs[i],
                   std::strerror(errno));
    }
  }
  g_installed = true;

  if (cfg.install_terminate) std::set_terminate(&OnTerminate);

  std::fprintf(stderr,
               "[crash] 崩溃处理已安装: app=%s dump_dir=%s core=%s "
               "(rlimit_core=%s) sigquit=%s\n",
               g_app, g_dir, g_dump_core ? "on" : "off",
               core_enabled ? "enabled"
                            : "0 (需 ulimit -c unlimited 或放宽 hard limit)",
               cfg.trap_sigquit ? "on" : "off");
  return ok;
}

void UninstallCrashHandler() {
  std::lock_guard<std::mutex> lock(g_state_mutex);
  if (!g_installed) return;
  for (int i = 0; i < g_trapped_count; ++i)
    sigaction(g_trapped_sig[i], &g_old_action[i], NULL);
  g_trapped_count = 0;
  g_installed = false;
}

void SetCrashTag(const char *key, const char *value) {
  if (!key || key[0] == '\0') return;
  std::lock_guard<std::mutex> lock(g_state_mutex);

  char entry[kTagLen];
  std::snprintf(entry, sizeof(entry), "%s=%s", key, value ? value : "");
  const size_t key_len = std::strlen(key);
  for (int i = 0; i < g_tag_count; ++i) {
    if (std::strncmp(g_tags[i], key, key_len) == 0 && g_tags[i][key_len] == '=') {
      std::snprintf(g_tags[i], kTagLen, "%s", entry);  // 同 key 覆盖
      return;
    }
  }
  if (g_tag_count < kMaxTags)
    std::snprintf(g_tags[g_tag_count++], kTagLen, "%s", entry);
}

void PrintBacktrace(const char *reason) {
  void *frames[kMaxFrames];
  const int n = backtrace(frames, kMaxFrames);
  std::fprintf(stderr, "[crash] backtrace (%s), %d frames:\n",
               reason ? reason : "", n);
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
}

}  // namespace common
}  // namespace evs
