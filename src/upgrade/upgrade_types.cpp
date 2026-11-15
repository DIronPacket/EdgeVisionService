#include "upgrade_types.h"

#include <strings.h>   // strcasecmp

#include <ctime>
#include <cstdio>

namespace evs {
namespace upgrade {

namespace {

struct PhaseName {
  Phase phase;
  const char *upper;  // 日志
  const char *lower;  // 上报 JSON
};

const PhaseName kPhaseNames[] = {
    {Phase::kIdle, "IDLE", "idle"},
    {Phase::kChecking, "CHECKING", "checking"},
    {Phase::kDownloading, "DOWNLOADING", "downloading"},
    {Phase::kVerifying, "VERIFYING", "verifying"},
    {Phase::kStaging, "STAGING", "staging"},
    {Phase::kActivating, "ACTIVATING", "activating"},
    {Phase::kHealthChecking, "HEALTH_CHECKING", "health_checking"},
    {Phase::kCommitted, "COMMITTED", "committed"},
    {Phase::kRollingBack, "ROLLING_BACK", "rolling_back"},
    {Phase::kFailed, "FAILED", "failed"},
    {Phase::kRecovery, "RECOVERY", "recovery"},
};

const char *const kErrorCodeNames[] = {
    "",            // kNone：没有原因码，上报时写空串
    "E_NET",       "E_HTTP",   "E_HASH",     "E_SIG",      "E_SCHEMA",
    "E_HW",        "E_ABI",    "E_DOWNGRADE", "E_MIN_FROM", "E_SPACE",
    "E_APPLY",     "E_HEALTH", "E_ROLLBACK", "E_BUSY",     "E_RECOVERY",
    "E_CONFIG",    "E_NOTIMPL",
};

const size_t kPhaseNameCount = sizeof(kPhaseNames) / sizeof(kPhaseNames[0]);
const size_t kErrorCodeCount = sizeof(kErrorCodeNames) / sizeof(kErrorCodeNames[0]);

}  // namespace

const char *ToString(Phase phase) {
  for (size_t i = 0; i < kPhaseNameCount; ++i) {
    if (kPhaseNames[i].phase == phase) return kPhaseNames[i].upper;
  }
  return "UNKNOWN";
}

const char *PhaseToPayload(Phase phase) {
  for (size_t i = 0; i < kPhaseNameCount; ++i) {
    if (kPhaseNames[i].phase == phase) return kPhaseNames[i].lower;
  }
  return "unknown";
}

bool ParsePhase(const std::string &text, Phase *out) {
  if (out == nullptr) return false;
  for (size_t i = 0; i < kPhaseNameCount; ++i) {
    if (strcasecmp(text.c_str(), kPhaseNames[i].upper) == 0 ||
        strcasecmp(text.c_str(), kPhaseNames[i].lower) == 0) {
      *out = kPhaseNames[i].phase;
      return true;
    }
  }
  return false;
}

const char *ToString(ErrorCode code) {
  const size_t idx = static_cast<size_t>(code);
  if (idx >= kErrorCodeCount) return "E_UNKNOWN";
  return kErrorCodeNames[idx];
}

bool IsTerminal(Phase phase) {
  return phase == Phase::kCommitted || phase == Phase::kFailed ||
         phase == Phase::kRecovery;
}

long long NowMs() {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
  return static_cast<long long>(ts.tv_sec) * 1000LL +
         static_cast<long long>(ts.tv_nsec) / 1000000LL;
}

std::string FormatTimeMs(long long ms) {
  const time_t sec = static_cast<time_t>(ms / 1000);
  const int msec = static_cast<int>(ms % 1000);
  struct tm tm_local;
  if (localtime_r(&sec, &tm_local) == nullptr) return "?";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec, msec);
  return std::string(buf);
}

}  // namespace upgrade
}  // namespace evs
