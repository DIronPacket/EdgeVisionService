#include "comm_types.h"

#include <cctype>

namespace evs {
namespace comm {

const char *ToString(CommType type) {
  switch (type) {
  case CommType::kUdp:
    return "udp";
  case CommType::kMqtt:
    return "mqtt";
  }
  return "unknown";
}

const char *ToString(CommRole role) {
  switch (role) {
  case CommRole::kServer:
    return "server";
  case CommRole::kClient:
    return "client";
  }
  return "unknown";
}

const char *ToString(CommState state) {
  switch (state) {
  case CommState::kIdle:
    return "idle";
  case CommState::kRunning:
    return "running";
  case CommState::kStopped:
    return "stopped";
  case CommState::kError:
    return "error";
  }
  return "unknown";
}

namespace {

std::string ToLower(const std::string &text) {
  std::string out = text;
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
  }
  return out;
}

}  // namespace

bool ParseCommType(const std::string &text, CommType *out) {
  if (out == nullptr) {
    return false;
  }
  const std::string lower = ToLower(text);
  if (lower == "udp") {
    *out = CommType::kUdp;
    return true;
  }
  if (lower == "mqtt") {
    *out = CommType::kMqtt;
    return true;
  }
  return false;
}

bool ParseCommRole(const std::string &text, CommRole *out) {
  if (out == nullptr) {
    return false;
  }
  const std::string lower = ToLower(text);
  if (lower == "server") {
    *out = CommRole::kServer;
    return true;
  }
  if (lower == "client") {
    *out = CommRole::kClient;
    return true;
  }
  return false;
}

}  // namespace comm
}  // namespace evs
