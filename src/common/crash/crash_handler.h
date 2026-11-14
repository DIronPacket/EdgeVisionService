#ifndef EVS_COMMON_CRASH_HANDLER_H
#define EVS_COMMON_CRASH_HANDLER_H

#include <string>

namespace evs {
namespace common {

// 崩溃现场采集配置。
//
// 它解决的是「程序死了但什么都没留下」：进程收到致命信号（SIGSEGV/SIGBUS/
// SIGILL/SIGFPE/SIGABRT）或 std::terminate 时，先写一份人类可读报告
// （程序名 / exe 路径 / pid / tid / 时间 / 信号 / 故障地址 / 寄存器 / 调用栈 /
// 业务 tag），再把同一个信号按默认动作重新抛一次，让内核按 core_pattern 落
// core —— 报告回答「哪个程序、哪个函数」，core + gdb 回答「哪个变量」。
struct CrashConfig {
  // 报告文件名与正文里的程序名，必填（例如 "EdgeVisionService"）
  std::string app_name;
  // 报告目录。空 = 取环境变量 EVS_CRASH_DIR，再空 = ./crash（相对进程 cwd）。
  // 目录不存在会尝试创建；创建失败退到 /tmp，避免崩溃时连报告都写不出。
  std::string report_dir;
  // true = 报告写完后把信号按默认动作重新抛出，交给内核写 core（推荐）
  bool dump_core;
  // true = SIGQUIT 也走「报告 + core」。这样进程卡死时 kill -QUIT <pid>
  // 就能拿到带业务上下文的现场（SIGQUIT 默认动作本来就是 core）
  bool trap_sigquit;
  // true = 安装 std::set_terminate（未捕获异常默认只 abort，什么都不打）
  bool install_terminate;

  CrashConfig() : dump_core(true), trap_sigquit(true), install_terminate(true) {}
};

// 安装崩溃处理。必须尽早调用（main 开头，早于任何线程创建）。
// 重复调用会用新配置覆盖；内部串行化。
// 返回 false 表示至少有一个关键信号没装上（其余仍生效），原因打到 stderr。
bool InstallCrashHandler(const CrashConfig &cfg);

// 恢复安装前的信号动作（正常退出路径不必调用，进程退出自然失效）
void UninstallCrashHandler();

// 记录「崩溃时也要带上的业务上下文」，例如当前链路 id、帧序号、配置路径。
// 建议只在启动/建链阶段调用：信号里读取时不加锁，运行期高频改写可能读到半截值。
// 最多 kMaxTags 条，同 key 覆盖。
void SetCrashTag(const char *key, const char *value);

// 打印当前调用栈到 stderr（可在非信号上下文随时调用，用于断言/兜底日志）
void PrintBacktrace(const char *reason);

}  // namespace common
}  // namespace evs

#endif  // EVS_COMMON_CRASH_HANDLER_H
