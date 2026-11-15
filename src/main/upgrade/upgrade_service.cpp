// ============================================================
// evs_upgrade 的编排层（状态机 + 事件上报 + 槽位操作）
//
// 分层（与 DESIGN_UPGRADE §4.3 一致）：
//   main_upgrade.cpp   → 只做 argv / 崩溃 handler / 调 Run()
//   upgrade_service.*  → 本文件：状态机推进、事件上报、持久化、命令分发
//   src/upgrade/*      → 无状态工具库（清单 / 验签 / 取数 / 槽位 / 健康检查）
//
// 状态机（§6.1）：
//   IDLE → CHECKING → DOWNLOADING → VERIFYING → STAGING → ACTIVATING
//        → HEALTH_CHECKING → COMMITTED → IDLE
//   任一步失败 → FAILED；切换后不健康 → ROLLING_BACK
//
// 骨架期的边界（**故意留桩，不假装成功**）：
//   已实现：检查（取清单/验签/兼容性判定）、下载（断点续传/重试/水位）、
//           校验 size_bytes + payload_sha256、槽位原子切换、状态原子持久化、
//           命令分发、事件上报、boot_count 计数、本地验签子命令
//   留桩  ：解包（ExtractPackage）、逐文件校验、systemctl 控制、ffprobe 帧率
//   → 走到这些桩会以 E_NOTIMPL 收尾并如实上报，不会"看起来升级成功"
// ============================================================

#include "upgrade_service.h"

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#include <json.hpp>

#include "comm_manager.h"
#include "comm_types.h"
#include "crash_handler.h"
#include "fetcher.h"
#include "health_check.h"
#include "manifest.h"
#include "slot_manager.h"
#include "upgrade_types.h"
#include "verifier.h"

namespace evs {
namespace upgrade_service {

namespace {

namespace up = evs::upgrade;

volatile sig_atomic_t g_run = 1;

void OnSignal(int sig) {
  (void)sig;
  g_run = 0;
}

void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char *fmt, ...) {
  std::printf("[upgrade] ");
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::printf("\n");
  std::fflush(stdout);
}

// 取路径最后一段（包文件名用；不含目录）
std::string BaseName(const std::string &path) {
  const size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

// 配置（config/upgrade.json，字段见 §10.2）
struct UpgradeConfig {
  up::SlotConfig slot;      // install_root / data_root / keep_versions / free_space_min_mb
  up::FetchConfig source;   // source.*
  up::HealthConfig health;  // health.*
  std::string manifest_name = "manifest.json";
  std::string signature_name = "manifest.sig";
  std::string public_key = "/etc/evs/ota_ed25519.pub";
  bool allow_downgrade = false;
  std::string hw = "rk3588";
  std::string abi;              // 空 = 不校验 ABI（开发机调试用）
  int boot_count_max = 3;
  std::string media_unit = "evs-media.service";
  // comm.*：protocol=mqtt 才建通道；none = 只打印不联网（离线调试/冒烟）
  std::string protocol = "mqtt";
  std::string channel_name = "upgrade";
  std::string host = "127.0.0.1";
  int port = 1883;
  std::string client_id = "evs-upgrade";
  std::string sub_topic = "evs/v1/upgrade/cmd/#";
  std::string pub_prefix = "evs/v1/upgrade/evt/";
  std::string config_path;
};

// ---------- 字段读取（类型不符就当没写，不抛异常；与 main_comm.cpp 同一写法）----------

void ReadString(const nlohmann::json &o, const char *key, std::string *dst) {
  if (o.is_object() && o.contains(key) && o[key].is_string())
    *dst = o[key].get<std::string>();
}

void ReadInt(const nlohmann::json &o, const char *key, int *dst) {
  if (o.is_object() && o.contains(key) && o[key].is_number_integer())
    *dst = o[key].get<int>();
}

void ReadBool(const nlohmann::json &o, const char *key, bool *dst) {
  if (o.is_object() && o.contains(key) && o[key].is_boolean())
    *dst = o[key].get<bool>();
}

std::vector<std::string> CandidatePaths(const std::string &explicit_path) {
  std::vector<std::string> paths;
  if (!explicit_path.empty()) {
    paths.push_back(explicit_path);
    return paths;
  }
  paths.push_back("config/upgrade.json");
  paths.push_back("../config/upgrade.json");
  return paths;
}

// 读配置。**不提供「用内置默认值静默启动」**（§10.2 末尾）：升级进程参数错了
// 比不启动更危险 —— 默认值可能把 releases 指到错误位置。
bool LoadConfig(const std::string &path, UpgradeConfig *cfg, std::string *err) {
  std::ifstream ifs(path.c_str());
  if (!ifs.is_open()) {
    *err = "配置文件打不开: " + path;
    return false;
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();

  nlohmann::json root;
  try {
    root = nlohmann::json::parse(oss.str());
  } catch (const std::exception &e) {
    *err = path + " JSON 语法错误: " + e.what();
    return false;
  }
  if (!root.is_object()) {
    *err = path + " 根节点必须是对象";
    return false;
  }

  cfg->config_path = path;

  ReadString(root, "install_root", &cfg->slot.install_root);
  ReadString(root, "data_root", &cfg->slot.data_root);

  if (root.contains("release") && root["release"].is_object()) {
    const nlohmann::json &r = root["release"];
    ReadInt(r, "keep_versions", &cfg->slot.keep_versions);
    ReadInt(r, "free_space_min_mb", &cfg->slot.free_space_min_mb);
  }

  if (root.contains("source") && root["source"].is_object()) {
    const nlohmann::json &s = root["source"];
    ReadString(s, "type", &cfg->source.type);
    ReadString(s, "base_url", &cfg->source.base_url);
    ReadString(s, "manifest_name", &cfg->manifest_name);
    ReadString(s, "signature_name", &cfg->signature_name);
    ReadString(s, "curl_bin", &cfg->source.curl_bin);
    ReadInt(s, "timeout_sec", &cfg->source.timeout_sec);
    ReadInt(s, "retry", &cfg->source.retry);
  }

  if (root.contains("verify") && root["verify"].is_object()) {
    ReadString(root["verify"], "public_key", &cfg->public_key);
    ReadBool(root["verify"], "allow_downgrade", &cfg->allow_downgrade);
  }

  if (root.contains("target") && root["target"].is_object()) {
    ReadString(root["target"], "hw", &cfg->hw);
    ReadString(root["target"], "abi", &cfg->abi);
  }

  if (root.contains("health") && root["health"].is_object()) {
    const nlohmann::json &h = root["health"];
    ReadString(h, "rtsp_url", &cfg->health.rtsp_url);
    ReadString(h, "ffprobe_path", &cfg->health.ffprobe_path);
    ReadInt(h, "probe_timeout_sec", &cfg->health.probe_timeout_sec);
    ReadInt(h, "min_fps", &cfg->health.min_fps);
    ReadInt(h, "stable_sec", &cfg->health.stable_sec);
  }

  if (root.contains("rollback") && root["rollback"].is_object())
    ReadInt(root["rollback"], "boot_count_max", &cfg->boot_count_max);

  if (root.contains("services") && root["services"].is_object())
    ReadString(root["services"], "media", &cfg->media_unit);

  if (root.contains("comm") && root["comm"].is_object()) {
    const nlohmann::json &c = root["comm"];
    ReadString(c, "protocol", &cfg->protocol);
    ReadString(c, "channel_name", &cfg->channel_name);
    ReadString(c, "host", &cfg->host);
    ReadInt(c, "port", &cfg->port);
    ReadString(c, "client_id", &cfg->client_id);
    ReadString(c, "sub_topic", &cfg->sub_topic);
    ReadString(c, "pub_topic", &cfg->pub_prefix);
  }

  // ---------- 校验：缺项直接拒绝启动 ----------
  if (cfg->slot.install_root.empty() || cfg->slot.data_root.empty()) {
    *err = "install_root / data_root 不能为空";
    return false;
  }
  if (cfg->slot.install_root == cfg->slot.data_root) {
    *err = "install_root 与 data_root 不能是同一个目录";
    return false;
  }
  if (cfg->source.type != "file" && cfg->source.type != "http") {
    *err = "source.type 只支持 file / http，实际 '" + cfg->source.type + "'";
    return false;
  }
  if (cfg->source.base_url.empty()) {
    *err = "source.base_url 为空（清单与包都在它下面）";
    return false;
  }
  if (cfg->source.timeout_sec <= 0) cfg->source.timeout_sec = 30;
  if (cfg->source.retry <= 0) cfg->source.retry = 1;
  if (cfg->public_key.empty()) {
    *err = "verify.public_key 为空：没有公钥就拒绝一切升级（不允许跳过验签）";
    return false;
  }
  if (cfg->protocol != "mqtt" && cfg->protocol != "none") {
    *err = "comm.protocol 只支持 mqtt / none（evt 子主题依赖 MQTT），实际 '" +
           cfg->protocol + "'";
    return false;
  }
  if (cfg->protocol == "mqtt" &&
      (cfg->sub_topic.empty() || cfg->pub_prefix.empty())) {
    *err = "comm.sub_topic / comm.pub_topic 不能为空";
    return false;
  }
  if (cfg->boot_count_max <= 0) cfg->boot_count_max = 3;
  if (cfg->slot.keep_versions <= 0) cfg->slot.keep_versions = 2;

  // 取数临时文件落在 data_root 下（而不是 /tmp）：PrivateTmp=yes 时 /tmp 与宿主
  // 隔离，把几十 MB 的 .part 放 /tmp 等于白占一份空间（§8.2）
  cfg->source.tmp_dir = cfg->slot.data_root + "/ota/download";
  return true;
}


// 事件上报（§9.1 的三个 evt 子主题）。
//
// 为什么一条主题一条通道：MqttComm 的发布主题是**通道级**配置（topic_pub），
// 不能按消息指定 topic；三个 evt 子主题就只能三条发布通道。
// protocol=none 时不建通道、只打印 —— 离线调试与冒烟测试用的就是这条路。
class EventBus {
 public:
  EventBus() : mgr_(nullptr), enabled_(false) {}

  // 必须在设置收包回调**之后**调用（SetReceiveCallback 只对之后 Create 的对象生效）
  bool Init(const UpgradeConfig &cfg) {
    pub_prefix_ = cfg.pub_prefix;
    channel_name_ = cfg.channel_name;
    if (cfg.protocol == "none") {
      Log("事件上报：protocol=none → 只打印不联网（离线模式）");
      return true;
    }

    mgr_ = &evs::comm::CommManager::getInstance();
    evs::comm::CommConfig base;
    base.type = evs::comm::CommType::kMqtt;
    base.role = evs::comm::CommRole::kClient;
    base.host = cfg.host;
    base.port = cfg.port;
    base.keepalive_sec = 30;
    base.reconnect_delay_ms = 1000;
    base.recv_timeout_ms = 200;

    // ① 命令通道：订阅 cmd/#，顺带作为 progress 的发布通道（省一条连接）
    evs::comm::CommConfig cmd = base;
    cmd.name = cfg.channel_name;
    cmd.role = evs::comm::CommRole::kServer;  // MQTT 的 server = 订阅端角色
    cmd.topic = cfg.sub_topic;
    cmd.topic_pub = cfg.pub_prefix + "progress";
    cmd.client_id = cfg.client_id;
    if (mgr_->Create(cmd) == nullptr) {
      Log("命令通道创建失败（降级为只打印）: %s", mgr_->last_error().c_str());
      return false;
    }

    // ② done ③ failed：只发布
    const char *kEvtTail[] = {"done", "failed"};
    for (size_t i = 0; i < sizeof(kEvtTail) / sizeof(kEvtTail[0]); ++i) {
      evs::comm::CommConfig pub = base;
      pub.name = cfg.channel_name + "-" + kEvtTail[i];
      pub.topic = cfg.pub_prefix + kEvtTail[i];
      pub.topic_pub = pub.topic;
      pub.client_id = cfg.client_id + "-" + kEvtTail[i];
      if (mgr_->Create(pub) == nullptr) {
        Log("evt/%s 通道创建失败（降级为只打印）: %s", kEvtTail[i],
            mgr_->last_error().c_str());
        return false;
      }
    }
    enabled_ = true;
    return true;
  }

  bool Start() {
    if (!enabled_) return true;
    if (!mgr_->StartAll()) {
      Log("部分通道启动失败: %s（后续上报会失败，但不影响升级流程）",
          mgr_->last_error().c_str());
      return false;
    }
    return true;
  }

  void Stop() {
    if (enabled_ && mgr_ != nullptr) mgr_->StopAll();
  }

  // evt = "progress" / "done" / "failed"（§9.1 的三个子主题）
  void Publish(const std::string &evt, nlohmann::json body) const {
    body["v"] = 1;
    body["ts"] = up::NowMs();
    body["src"] = "upgrade";
    const std::string text = body.dump();
    Log("evt/%s %s", evt.c_str(), text.c_str());
    if (!enabled_ || mgr_ == nullptr) return;
    evs::comm::CommBase *leaf = mgr_->Find(ChannelNameOf(evt));
    if (leaf == nullptr) return;
    if (!leaf->SendText(text))
      Log("上报 evt/%s 失败: %s", evt.c_str(), leaf->last_error().c_str());
  }

 private:
  std::string ChannelNameOf(const std::string &evt) const {
    if (evt == "progress") return channel_name_;
    return channel_name_ + "-" + evt;
  }

  evs::comm::CommManager *mgr_;
  bool enabled_;
  std::string channel_name_;
  std::string pub_prefix_;
};


// 升级会话：一次进程生命周期内的全部状态与动作。
//
// 线程模型（与 comm_api 的收包回调约束一致，见 comm_manager.h 注释）：
//   - 状态机 / 槽位操作只在**主线程**（RunOnce/RunDaemon 所在线程）执行；
//   - 收包回调跑在 MQTT 接收线程里 → **只把命令塞进队列**，
//     绝不在回调里碰磁盘、碰槽位、调 Stop（会在自己线程 join 自己）。
class Session {
 public:
  explicit Session(const UpgradeConfig &cfg);

  bool Init();  // EnsureLayout + 读状态文件 + 读 current
  int RunOnce(CliAction action, const Options &opt);
  int RunDaemon(const Options &opt);

 private:
  struct Command {
    std::string action;   // check / download / apply / rollback
    std::string url;      // 可选的显式包地址
    std::string reason;   // 回滚原因
    bool force = false;   // force:true 时允许从恢复模式/失败态继续
  };

  // 收包（MQTT 接收线程）：只入队
  void OnFrame(const std::string &channel, const evs::comm::CommFrame &frame);
  void DrainCommands();
  void HandleCommand(const Command &cmd);
  bool Busy() const;

  // 状态机动作
  bool DoCheck(std::string *new_version, up::Manifest *out);
  bool DoDownload(const std::string &pkg_url);
  bool DoApply();
  bool DoRollback(const std::string &reason, up::ErrorCode cause);
  bool OnMediaStarted();
  bool VerifyLocal(const std::string &manifest_path,
                   const std::string &sig_path);
  void PrintStatus() const;

  // 状态推进与上报
  void EnterPhase(up::Phase phase, int percent = -1);
  bool Fail(up::ErrorCode code, const std::string &message);
  void PublishProgress() const;
  void PublishDone(bool up_to_date, long long duration_ms) const;
  void PublishFailed(up::Phase phase, const std::string &code,
                     bool rolled_back, const std::string &message) const;
  void AppendAudit(const char *action, const char *result) const;

  UpgradeConfig cfg_;
  up::SlotManager slots_;
  up::Fetcher fetcher_;
  EventBus bus_;
  up::UpgradeState state_;
  up::Manifest manifest_;
  std::string current_version_;
  std::string from_version_;
  std::string pkg_path_;
  std::string pkg_url_override_;   // cmd 里带的显式包地址（空 = 按 §5.1 命名推导）
  bool persistence_ok_;
  bool dry_run_;
  long long action_start_ms_;

  std::mutex cmd_mutex_;
  std::deque<Command> cmds_;
};

Session::Session(const UpgradeConfig &cfg)
    : cfg_(cfg),
      slots_(cfg.slot),
      fetcher_(cfg.source),
      persistence_ok_(false),
      dry_run_(false),
      action_start_ms_(0) {}

bool Session::Init() {
  std::string err;
  if (!slots_.EnsureLayout(&err)) {
    // 非 root / 只读根文件系统时会走到这里。**不退出**：检查/下载/验签这些
    // 「不改线上」的动作仍然有意义，只是状态不落盘（掉电恢复能力降级）。
    Log("目录布局不完整（%s）→ 状态将不持久化", err.c_str());
    persistence_ok_ = false;
  } else {
    persistence_ok_ = true;
  }

  std::string ver;
  std::string rerr;
  if (!slots_.ReadCurrentVersion(&ver, &rerr)) {
    Log("读 current 失败: %s", rerr.c_str());
  }
  current_version_ = ver;
  from_version_ = ver;

  if (persistence_ok_) {
    std::string serr;
    if (!slots_.ReadState(&state_, &serr)) {
      Log("状态文件损坏（按 IDLE 处理）: %s", serr.c_str());
      state_ = up::UpgradeState();
    }
  }

  Log("install_root=%s data_root=%s", cfg_.slot.install_root.c_str(),
      cfg_.slot.data_root.c_str());
  Log("current=%s（previous 见 --status）",
      current_version_.empty() ? "<无>" : current_version_.c_str());
  Log("source=%s:%s（retry=%d timeout=%ds）", cfg_.source.type.c_str(),
      cfg_.source.base_url.c_str(), cfg_.source.retry, cfg_.source.timeout_sec);
  Log("OpenSSL 编译期=%s / 运行期=%s（两者不一致说明头库错配，见 §11.2）",
      up::OpenSslCompileTimeVersion(), up::OpenSslRuntimeVersion());
  return true;
}

// ------------------------------------------------------------------
// 状态推进与上报
// ------------------------------------------------------------------

void Session::EnterPhase(up::Phase phase, int percent) {
  if (phase == up::Phase::kChecking) {
    // 新的一次尝试：先清掉上一次的失败痕迹，否则会出现「成功了还挂着 E_SIG」
    state_.code.clear();
    state_.message.clear();
    state_.boot_count = 0;
  }
  const up::Phase previous = state_.phase;
  state_.phase = phase;
  if (percent >= 0) state_.percent = percent;
  state_.updated_at_ms = up::NowMs();
  if (state_.from.empty()) state_.from = current_version_;

  // 崩溃报告里带上升级阶段（§4.4）：崩在 STAGING 和崩在 IDLE 处理方式完全不同
  evs::common::SetCrashTag("upgrade_phase", up::ToString(phase));

  if (persistence_ok_) {
    std::string err;
    if (!slots_.WriteState(state_, &err))
      Log("写状态失败（继续执行，但掉电后无法恢复）: %s", err.c_str());
  }
  if (phase != previous && phase != up::Phase::kIdle)
    Log("阶段 %s → %s", up::ToString(previous), up::ToString(phase));
  if (phase != up::Phase::kIdle) PublishProgress();
}

bool Session::Fail(up::ErrorCode code, const std::string &message) {
  const up::Phase failed_at = state_.phase;
  state_.phase = up::Phase::kFailed;
  state_.code = up::ToString(code);
  state_.message = message;
  state_.updated_at_ms = up::NowMs();
  if (persistence_ok_) {
    std::string err;
    if (!slots_.WriteState(state_, &err)) Log("写失败态失败: %s", err.c_str());
  }
  Log("失败于 %s：%s（%s）", up::ToString(failed_at), state_.code.c_str(),
      message.c_str());
  PublishFailed(failed_at, state_.code, false, message);
  AppendAudit("upgrade", "failed");
  return false;
}

void Session::PublishProgress() const {
  nlohmann::json j;
  j["phase"] = up::PhaseToPayload(state_.phase);
  j["from"] = state_.from;
  j["to"] = state_.to;
  // percent 只在下载/解包阶段有意义（§9.2 字段规则）
  if (state_.phase == up::Phase::kDownloading ||
      state_.phase == up::Phase::kStaging)
    j["percent"] = state_.percent;
  bus_.Publish("progress", j);
}

void Session::PublishDone(bool up_to_date, long long duration_ms) const {
  nlohmann::json j;
  j["result"] = up_to_date ? "up-to-date" : "done";
  j["from"] = state_.from;
  j["to"] = up_to_date ? state_.from : state_.to;
  j["duration_ms"] = duration_ms;
  bus_.Publish("done", j);
}

void Session::PublishFailed(up::Phase phase, const std::string &code,
                            bool rolled_back, const std::string &message) const {
  nlohmann::json j;
  j["result"] = "failed";
  j["phase"] = up::PhaseToPayload(phase);
  j["code"] = code;
  j["rolled_back"] = rolled_back;
  j["message"] = message;
  j["from"] = state_.from;
  j["to"] = state_.to;
  bus_.Publish("failed", j);
}

// 审计日志（§8.3）：每次升级**结果**一行 JSON；临时错误只进 stdout，不进审计
void Session::AppendAudit(const char *action, const char *result) const {
  if (!persistence_ok_) return;
  std::string mkerr;
  if (!up::MakeDirs(slots_.logs_dir(), &mkerr)) return;

  nlohmann::json j;
  j["ts"] = up::NowMs();
  j["action"] = action;
  j["from"] = state_.from;
  j["to"] = state_.to;
  j["phase"] = up::PhaseToPayload(state_.phase);
  j["result"] = result;
  j["code"] = state_.code;
  j["size"] = manifest_.size_bytes;
  j["duration_ms"] = action_start_ms_ > 0 ? (up::NowMs() - action_start_ms_) : 0;
  j["operator"] = "local";  // 云侧下发的 operator 由 cmd 带，P2 补

  const std::string path = slots_.logs_dir() + "/upgrade.log";
  std::ofstream ofs(path.c_str(), std::ios::out | std::ios::app);
  if (!ofs.is_open()) {
    Log("审计日志写不进去: %s", path.c_str());
    return;
  }
  ofs << j.dump() << "\n";
}

// 是否已有升级在进行（§8.1「并发升级」一行；当前串行执行，留给 P2 异步下载）
bool Session::Busy() const {
  switch (state_.phase) {
    case up::Phase::kChecking:
    case up::Phase::kDownloading:
    case up::Phase::kVerifying:
    case up::Phase::kStaging:
    case up::Phase::kActivating:
    case up::Phase::kHealthChecking:
    case up::Phase::kRollingBack:
      return true;
    default:
      return false;
  }
}

// ------------------------------------------------------------------
// CHECKING：拉清单 → 验签 → 解析 → 兼容性/防降级（§6.2 第一行）
//
// 顺序不能换：**先验签、后解析**。清单没通过签名之前，里面的 version / path
// 都不能作为决策依据（§5.4 规则一）。
// ------------------------------------------------------------------
bool Session::DoCheck(std::string *new_version, up::Manifest *out) {
  if (new_version != nullptr) new_version->clear();
  if (action_start_ms_ == 0) action_start_ms_ = up::NowMs();
  EnterPhase(up::Phase::kChecking);

  // ① 取清单（原样字节；验签用的就是这串字节，所以不能先 parse 再序列化）
  std::string text;
  up::ErrorCode code = up::ErrorCode::kNone;
  std::string err;
  if (!fetcher_.FetchToString(cfg_.manifest_name, &text, &code, &err))
    return Fail(code, "取清单失败: " + err);

  // ② 取签名 + 验签
  std::string sig;
  if (!fetcher_.FetchToString(cfg_.signature_name, &sig, &code, &err))
    return Fail(up::ErrorCode::kSig, "取签名失败: " + err);
  if (!up::VerifyEd25519(cfg_.public_key, text, sig, &err))
    return Fail(up::ErrorCode::kSig, "清单验签失败: " + err);
  Log("清单验签通过（公钥 %s）", cfg_.public_key.c_str());

  // ③ 解析 + 字段校验
  up::Manifest m;
  if (!up::ParseManifest(text, &m, &code, &err))
    return Fail(code, "清单不合法: " + err);

  // ④ 兼容性 / 防降级（§5.5）
  up::ManifestContext ctx;
  ctx.current_version = current_version_;
  ctx.hw = cfg_.hw;
  ctx.abi = cfg_.abi;
  ctx.allow_downgrade = cfg_.allow_downgrade;
  up::ErrorCode ccode = up::ErrorCode::kNone;
  if (!up::CheckManifest(m, ctx, &ccode, &err)) {
    const bool same_version = !current_version_.empty() &&
                              up::CompareVersion(m.version, current_version_) == 0;
    if (ccode == up::ErrorCode::kDowngrade && same_version) {
      // 重复下发：幂等收尾，**不重复下载/切换**（§5.5 最后一行）
      Log("已是最新（当前 %s，清单 %s），按重复下发处理", current_version_.c_str(),
          m.version.c_str());
      state_.to = current_version_;
      state_.phase = up::Phase::kIdle;
      state_.updated_at_ms = up::NowMs();
      if (persistence_ok_) {
        std::string werr;
        slots_.WriteState(state_, &werr);
      }
      PublishDone(true, up::NowMs() - action_start_ms_);
      AppendAudit("check", "up-to-date");
      return true;  // new_version 为空 = 没有新版本
    }
    return Fail(ccode, err);
  }

  Log("发现新版本 %s：%s → %s（%lld 字节，%zu 个文件）", m.version.c_str(),
      current_version_.empty() ? "<首装>" : current_version_.c_str(),
      m.version.c_str(), m.size_bytes, m.files.size());
  Log("  git=%s built=%s hw=%s abi=%s min_from=%s", m.git_commit.c_str(),
      m.built_at.c_str(), m.hw.c_str(), m.abi.c_str(), m.min_from.c_str());

  state_.from = current_version_;
  state_.to = m.version;
  state_.updated_at_ms = up::NowMs();
  if (persistence_ok_) {
    std::string werr;
    if (!slots_.WriteState(state_, &werr))
      Log("写状态失败（继续）: %s", werr.c_str());
  }

  if (new_version != nullptr) *new_version = m.version;
  if (out != nullptr) *out = m;
  return true;
}

// ------------------------------------------------------------------
// DOWNLOADING + VERIFYING
// ------------------------------------------------------------------
bool Session::DoDownload(const std::string &pkg_url) {
  std::string err;
  if (!up::MakeDirs(slots_.download_dir(), &err))
    return Fail(up::ErrorCode::kSpace, "下载目录不可用: " + err);

  // 水位检查：下载前先算，空间不够就别浪费流量（§6.2 DOWNLOADING）
  const long long need_mb =
      manifest_.size_bytes / (1024LL * 1024LL) + cfg_.slot.free_space_min_mb;
  const long long free_mb = slots_.FreeSpaceMb(slots_.download_dir());
  if (free_mb >= 0 && free_mb < need_mb) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "磁盘不足：余量 %lld MB，需要 %lld MB（含水位 %d MB）",
                  free_mb, need_mb, cfg_.slot.free_space_min_mb);
    return Fail(up::ErrorCode::kSpace, buf);
  }
  if (free_mb >= 0) Log("磁盘余量 %lld MB（本次需要 %lld MB）", free_mb, need_mb);

  EnterPhase(up::Phase::kDownloading, 0);

  // 包名规则见 §5.1：evs-app-<version>-<hw>.tar.gz
  std::string name = pkg_url;
  if (name.empty())
    name = "evs-app-" + manifest_.version + "-" + manifest_.hw + ".tar.gz";
  pkg_path_ = slots_.download_dir() + "/" + BaseName(name);

  if (dry_run_) {
    Log("[dry-run] 将下载 %s → %s（跳过）", name.c_str(), pkg_path_.c_str());
    return true;
  }

  up::ErrorCode code = up::ErrorCode::kNone;
  if (!fetcher_.FetchToFile(name, pkg_path_, &code, &err))
    return Fail(code, "下载失败: " + err);

  // ---------- VERIFYING：size_bytes 先挡住截断，再算 payload_sha256 ----------
  EnterPhase(up::Phase::kVerifying);
  struct stat st;
  if (::stat(pkg_path_.c_str(), &st) != 0)
    return Fail(up::ErrorCode::kApply, "取不到包大小: " + pkg_path_);
  if (static_cast<long long>(st.st_size) != manifest_.size_bytes)
    return Fail(up::ErrorCode::kHash,
                "包大小不符：清单 " + std::to_string(manifest_.size_bytes) +
                    " 字节，实际 " + std::to_string(static_cast<long long>(st.st_size)));

  std::string hex;
  std::string herr;
  if (!up::Sha256File(pkg_path_, &hex, &herr))
    return Fail(up::ErrorCode::kHash, "算 sha256 失败: " + herr);
  if (!up::HexEqual(hex, manifest_.payload_sha256))
    return Fail(up::ErrorCode::kHash,
                "payload_sha256 不符：清单 " + manifest_.payload_sha256 + "，实际 " + hex);

  state_.percent = 100;
  Log("包体校验通过（%lld 字节, sha256=%s）", static_cast<long long>(st.st_size),
      hex.c_str());
  return true;
}

// ------------------------------------------------------------------
// 一条龙：CHECKING → DOWNLOADING → VERIFYING → STAGING → ACTIVATING
//        → HEALTH_CHECKING → COMMITTED（§6.1）
//
// 骨架期只到 STAGING 的桩就会停（ExtractPackage 返回 E_NOTIMPL）；
// 后面的代码是**照着设计写完的**，等 P2/P3 把桩换掉就能直接跑通。
// ------------------------------------------------------------------
bool Session::DoApply() {
  action_start_ms_ = up::NowMs();
  state_.message.clear();

  std::string new_version;
  up::Manifest m;
  if (!DoCheck(&new_version, &m)) return false;
  if (new_version.empty()) return true;  // 已是最新（DoCheck 内已发 done）
  manifest_ = m;

  if (!DoDownload(pkg_url_override_)) return false;

  // ---------- STAGING（桩）----------
  EnterPhase(up::Phase::kStaging);
  up::ErrorCode code = up::ErrorCode::kNone;
  std::string err;
  if (!slots_.ExtractPackage(pkg_path_, manifest_.version, &code, &err))
    return Fail(code, err);
  if (!slots_.VerifyStagedFiles(manifest_.version, manifest_.files, &code, &err))
    return Fail(code, err);

  // ---------- ACTIVATING：到这一步才动线上业务（§6.2）----------
  EnterPhase(up::Phase::kActivating);
  if (!slots_.ControlService(cfg_.media_unit, "stop", 5, &err))
    return Fail(up::ErrorCode::kApply, "停 " + cfg_.media_unit + " 失败: " + err);
  if (!slots_.SwitchCurrent(manifest_.version, true, &err))
    return Fail(up::ErrorCode::kApply, "切换 current 失败: " + err);
  Log("current → %s（previous 保留 %s）", manifest_.version.c_str(),
      from_version_.c_str());
  if (!slots_.ControlService(cfg_.media_unit, "start", 3, &err))
    return DoRollback("start-failed", up::ErrorCode::kApply);

  // ---------- HEALTH_CHECKING ----------
  EnterPhase(up::Phase::kHealthChecking);
  const up::HealthChecker health(cfg_.health);
  const up::HealthResult hr = health.CheckAll();
  Log("健康检查: %s（port_alive=%d fps=%d）", hr.detail.c_str(),
      hr.port_alive ? 1 : 0, hr.fps);
  if (!hr.ok) return DoRollback("unhealthy", up::ErrorCode::kHealth);

  // ---------- COMMITTED ----------
  state_.phase = up::Phase::kCommitted;
  state_.boot_count = 0;
  state_.percent = 100;
  state_.updated_at_ms = up::NowMs();
  if (persistence_ok_) {
    std::string werr;
    slots_.WriteState(state_, &werr);
  }
  std::string perr;
  if (!slots_.PruneOldReleases(&perr))
    Log("旧版本清理未做（非致命，下次启动重试）: %s", perr.c_str());

  current_version_ = manifest_.version;
  PublishDone(false, up::NowMs() - action_start_ms_);
  AppendAudit("apply", "done");

  // COMMITTED 是「已确认」的终态，落盘后回 IDLE，等下次命令
  state_.phase = up::Phase::kIdle;
  if (persistence_ok_) {
    std::string werr;
    slots_.WriteState(state_, &werr);
  }
  return true;
}

// ------------------------------------------------------------------
// ROLLING_BACK（§7.3）
//
// 顺序：停服务 → 切回 previous → 起服务 → 上报。
// ⚠ 关键防护：**控制不了服务就绝不切链**。切了链但服务没起来，比不回滚更糟
//   （会变成「链指向老版本、进程还是新版本的残留」这种半死状态）。
// ------------------------------------------------------------------
bool Session::DoRollback(const std::string &reason, up::ErrorCode cause) {
  EnterPhase(up::Phase::kRollingBack);

  std::string prev;
  std::string err;
  if (!slots_.ReadPreviousVersion(&prev, &err))
    return Fail(up::ErrorCode::kRollback, "读 previous 失败: " + err);
  if (prev.empty())
    return Fail(up::ErrorCode::kRollback,
                "没有 previous（首次安装或已清理），无法回滚；原因=" + reason);

  if (!slots_.ControlService(cfg_.media_unit, "is-active", 5, &err))
    return Fail(up::ErrorCode::kNotImpl,
                "回滚需要 systemctl 控制 " + cfg_.media_unit +
                    "（P3 实现），本次未做任何切换: " + err);

  if (dry_run_) {
    Log("[dry-run] 将回滚 %s → %s（跳过）", current_version_.c_str(), prev.c_str());
    return true;
  }

  if (!slots_.ControlService(cfg_.media_unit, "stop", 5, &err))
    return Fail(up::ErrorCode::kRollback, "停服务失败: " + err);
  if (!slots_.SwitchCurrent(prev, false, &err))
    return Fail(up::ErrorCode::kRollback, "切回 previous 失败: " + err);
  if (!slots_.ControlService(cfg_.media_unit, "start", 3, &err)) {
    // 回滚也起不来 → 恢复模式（§7.5）：只能留待人工介入
    state_.phase = up::Phase::kRecovery;
    state_.code = up::ToString(up::ErrorCode::kRecovery);
    state_.message = "回滚到 " + prev + " 后仍无法启动: " + err +
                     "；请人工介入（阶段/路径见 upgrade.json 与 upgrade.log）";
    state_.updated_at_ms = up::NowMs();
    if (persistence_ok_) {
      std::string werr;
      slots_.WriteState(state_, &werr);
    }
    Log("%s", state_.message.c_str());
    PublishFailed(up::Phase::kRollingBack, state_.code, false, state_.message);
    AppendAudit("rollback", "failed");
    return false;
  }

  current_version_ = prev;
  state_.phase = up::Phase::kIdle;
  state_.code = up::ToString(cause);
  state_.message = "已回滚到 " + prev + "（原因：" + reason + "）";
  state_.updated_at_ms = up::NowMs();
  if (persistence_ok_) {
    std::string werr;
    slots_.WriteState(state_, &werr);
  }
  Log("回滚完成：current → %s（原因 %s）", prev.c_str(), reason.c_str());
  PublishFailed(up::Phase::kRollingBack, state_.code, true, state_.message);
  AppendAudit("rollback", "done");
  return true;
}

// ------------------------------------------------------------------
// --on-media-started（§7.2）：只做 boot_count += 1
//
// 为什么由 media 的 ExecStartPost 调：媒体进程起不来时，常驻的 evs_upgrade
// 可能已被 systemd 一起停掉；ExecStartPost 是「真的启动成功」唯一不会漏的计数点。
//
// 约定：**永远返回 true**。这个钩子如果非 0 退出，会让 systemd 认为启动失败，
// 进而触发 OnFailure 回滚 —— 那是灾难。计数失败只打日志。
// ------------------------------------------------------------------
bool Session::OnMediaStarted() {
  if (!persistence_ok_) {
    Log("状态不可持久化，跳过 boot_count");
    return true;
  }
  up::UpgradeState st;
  std::string err;
  if (!slots_.ReadState(&st, &err)) {
    Log("读状态失败，跳过 boot_count: %s", err.c_str());
    return true;
  }
  if (st.phase != up::Phase::kHealthChecking) {
    Log("不在健康检查窗口（phase=%s），忽略 boot_count", up::ToString(st.phase));
    return true;
  }

  st.boot_count += 1;
  st.updated_at_ms = up::NowMs();
  if (!slots_.WriteState(st, &err)) Log("写 boot_count 失败: %s", err.c_str());
  Log("boot_count=%d（上限 %d）：%s → %s", st.boot_count, cfg_.boot_count_max,
      st.from.c_str(), st.to.c_str());

  if (st.boot_count > cfg_.boot_count_max) {
    Log("连续启动超过上限 → 强制回滚（E_HEALTH）");
    st.code = up::ToString(up::ErrorCode::kHealth);
    st.message = "新版本连续启动 " + std::to_string(st.boot_count) + " 次未确认";
    slots_.WriteState(st, &err);
    state_ = st;
    PublishFailed(up::Phase::kHealthChecking, st.code, false, st.message);
    DoRollback("boot_count", up::ErrorCode::kHealth);  // 返回值不影响退出码
  }
  return true;
}

// ------------------------------------------------------------------
// --verify=<manifest>：只验本地清单签名（发布机自检 / 排障）
// 不联网、不看 current，因此开发机上也能跑。
// ------------------------------------------------------------------
bool Session::VerifyLocal(const std::string &manifest_path,
                          const std::string &sig_path) {
  const std::string sig =
      sig_path.empty() ? (manifest_path + ".sig") : sig_path;
  Log("验签：清单 %s，签名 %s，公钥 %s", manifest_path.c_str(), sig.c_str(),
      cfg_.public_key.c_str());

  std::string err;
  if (!up::VerifyManifestSignature(cfg_.public_key, manifest_path, sig, &err)) {
    Log("验签失败: %s", err.c_str());
    return false;
  }

  std::string text;
  if (!up::ReadFileBytes(manifest_path, &text, &err)) {
    Log("读清单失败: %s", err.c_str());
    return false;
  }
  up::Manifest m;
  up::ErrorCode code = up::ErrorCode::kNone;
  if (!up::ParseManifest(text, &m, &code, &err)) {
    Log("清单不合法: %s", err.c_str());
    return false;
  }
  Log("签名 OK：version=%s git=%s built=%s", m.version.c_str(),
      m.git_commit.c_str(), m.built_at.c_str());
  Log("  target: hw=%s abi=%s min_from=%s", m.hw.c_str(), m.abi.c_str(),
      m.min_from.c_str());
  Log("  payload_sha256=%s size=%lld files=%zu", m.payload_sha256.c_str(),
      m.size_bytes, m.files.size());
  Log("  rollback: keep_versions=%d boot_count_max=%d", m.keep_versions,
      m.boot_count_max);

  // 兼容性只做提示（发布机自检时本机通常没有 current）
  up::ManifestContext ctx;
  ctx.current_version = current_version_;
  ctx.hw = cfg_.hw;
  ctx.abi = cfg_.abi;
  ctx.allow_downgrade = cfg_.allow_downgrade;
  up::ErrorCode ccode = up::ErrorCode::kNone;
  if (!up::CheckManifest(m, ctx, &ccode, &err))
    Log("兼容性提示（%s）: %s", up::ToString(ccode), err.c_str());
  return true;
}

// ------------------------------------------------------------------
// --status：把「盘上真实状态」打全（排障第一步永远先看这个）
// ------------------------------------------------------------------
void Session::PrintStatus() const {
  std::string cur;
  std::string prev;
  std::string err;
  slots_.ReadCurrentVersion(&cur, &err);
  slots_.ReadPreviousVersion(&prev, &err);

  Log("install_root = %s", cfg_.slot.install_root.c_str());
  Log("  current    -> %s", cur.empty() ? "<无>" : cur.c_str());
  Log("  previous   -> %s", prev.empty() ? "<无>" : prev.c_str());
  Log("  releases   = %s", slots_.releases_dir().c_str());
  Log("  staging    = %s", slots_.staging_dir().c_str());
  Log("data_root    = %s", cfg_.slot.data_root.c_str());
  const long long free_mb = slots_.FreeSpaceMb(slots_.download_dir());
  if (free_mb >= 0) {
    Log("  download   = %s（余量 %lld MB）", slots_.download_dir().c_str(), free_mb);
  } else {
    // 目录还不存在 / 不可写：statvfs 会失败，别显示成「-1 MB」误导排障
    Log("  download   = %s（余量未知：目录不存在或不可读）",
        slots_.download_dir().c_str());
  }
  Log("  state_file = %s", slots_.state_file().c_str());
  Log("  持久化     = %s", persistence_ok_ ? "开" : "关（目录不可写）");
  Log("state: phase=%s from=%s to=%s boot_count=%d percent=%d code=%s",
      up::ToString(state_.phase),
      state_.from.empty() ? "-" : state_.from.c_str(),
      state_.to.empty() ? "-" : state_.to.c_str(), state_.boot_count,
      state_.percent, state_.code.empty() ? "-" : state_.code.c_str());
  if (!state_.message.empty()) Log("       message=%s", state_.message.c_str());
  Log("       updated=%s", up::FormatTimeMs(state_.updated_at_ms).c_str());
  Log("source: %s:%s（manifest=%s sig=%s）", cfg_.source.type.c_str(),
      cfg_.source.base_url.c_str(), cfg_.manifest_name.c_str(),
      cfg_.signature_name.c_str());
  Log("verify: public_key=%s allow_downgrade=%d", cfg_.public_key.c_str(),
      cfg_.allow_downgrade ? 1 : 0);
  Log("target: hw=%s abi=%s", cfg_.hw.c_str(),
      cfg_.abi.empty() ? "<不校验>" : cfg_.abi.c_str());
  Log("health: rtsp=%s min_fps=%d stable_sec=%d ffprobe=%s",
      cfg_.health.rtsp_url.c_str(), cfg_.health.min_fps,
      cfg_.health.stable_sec,
      cfg_.health.ffprobe_path.empty() ? "<PATH 查找>" : cfg_.health.ffprobe_path.c_str());
  Log("comm: protocol=%s channel=%s sub=%s pub_prefix=%s", cfg_.protocol.c_str(),
      cfg_.channel_name.c_str(), cfg_.sub_topic.c_str(), cfg_.pub_prefix.c_str());
  Log("OpenSSL: 编译期 %s / 运行期 %s", up::OpenSslCompileTimeVersion(),
      up::OpenSslRuntimeVersion());
}

// ------------------------------------------------------------------
// 命令分发
//
// 收包线程只入队；真正的状态机动作全部在主线程执行（见类注释的线程模型）。
// ------------------------------------------------------------------
void Session::OnFrame(const std::string &channel,
                      const evs::comm::CommFrame &frame) {
  if (channel != cfg_.channel_name) return;  // done/failed 是只发通道

  // frame.data 只在回调期间有效（comm_types.h）→ 立刻拷成 std::string
  std::string text;
  if (frame.data != nullptr && frame.size > 0)
    text.assign(reinterpret_cast<const char *>(frame.data), frame.size);

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(text);
  } catch (const std::exception &e) {
    Log("丢弃坏包（peer=%s topic=%s）: %s", frame.peer.c_str(),
        frame.topic.c_str(), e.what());
    return;
  }
  if (!j.is_object()) {
    Log("丢弃：payload 不是对象（topic=%s）", frame.topic.c_str());
    return;
  }

  Command cmd;
  if (j.contains("action") && j["action"].is_string())
    cmd.action = j["action"].get<std::string>();
  else if (!frame.topic.empty()) {
    // 兜底：按主题最后一段路由（evs/v1/upgrade/cmd/check → check）
    const size_t pos = frame.topic.find_last_of('/');
    cmd.action =
        (pos == std::string::npos) ? frame.topic : frame.topic.substr(pos + 1);
  }
  if (j.contains("url") && j["url"].is_string())
    cmd.url = j["url"].get<std::string>();
  if (j.contains("reason") && j["reason"].is_string())
    cmd.reason = j["reason"].get<std::string>();
  if (j.contains("force") && j["force"].is_boolean())
    cmd.force = j["force"].get<bool>();

  if (cmd.action != "check" && cmd.action != "download" &&
      cmd.action != "apply" && cmd.action != "rollback") {
    Log("丢弃未知命令 '%s'（topic=%s）", cmd.action.c_str(), frame.topic.c_str());
    return;
  }

  std::lock_guard<std::mutex> lock(cmd_mutex_);
  if (cmds_.size() >= 8) {
    Log("命令队列已满（8），丢弃 '%s'", cmd.action.c_str());
    return;
  }
  cmds_.push_back(cmd);
  Log("收到命令 %s（队列 %zu）", cmd.action.c_str(), cmds_.size());
}

void Session::DrainCommands() {
  for (;;) {
    Command cmd;
    {
      std::lock_guard<std::mutex> lock(cmd_mutex_);
      if (cmds_.empty()) return;
      cmd = cmds_.front();
      cmds_.pop_front();
    }
    HandleCommand(cmd);
  }
}

void Session::HandleCommand(const Command &cmd) {
  if (Busy() && !cmd.force) {
    Log("已有升级在进行（%s），拒绝 %s", up::ToString(state_.phase),
        cmd.action.c_str());
    PublishFailed(state_.phase, up::ToString(up::ErrorCode::kBusy), false,
                  "已有升级在进行");
    return;
  }

  action_start_ms_ = up::NowMs();
  Log("执行命令: %s", cmd.action.c_str());
  bool ok = true;

  if (cmd.action == "check") {
    std::string v;
    up::Manifest m;
    ok = DoCheck(&v, &m);
    if (ok && !v.empty()) Log("check: 有新版本 %s（等 apply 命令）", v.c_str());
  } else if (cmd.action == "download") {
    std::string v;
    up::Manifest m;
    ok = DoCheck(&v, &m);
    if (ok && !v.empty()) {
      manifest_ = m;
      ok = DoDownload(cmd.url);
    }
  } else if (cmd.action == "apply") {
    pkg_url_override_ = cmd.url;
    ok = DoApply();
  } else if (cmd.action == "rollback") {
    ok = DoRollback(cmd.reason.empty() ? "cmd" : cmd.reason,
                    up::ErrorCode::kNone);
  }
  if (ok) Log("命令 %s 完成", cmd.action.c_str());
}

int Session::RunOnce(CliAction action, const Options &opt) {
  dry_run_ = opt.dry_run;

  switch (action) {
    case CliAction::kCheck: {
      action_start_ms_ = up::NowMs();
      std::string v;
      up::Manifest m;
      if (!DoCheck(&v, &m)) return 2;
      if (v.empty()) Log("check: 已是最新（%s）", current_version_.c_str());
      else Log("check: 发现新版本 %s", v.c_str());
      return 0;
    }
    case CliAction::kApply:
      return DoApply() ? 0 : 2;
    case CliAction::kRollback:
      return DoRollback(opt.reason.empty() ? "manual" : opt.reason,
                        up::ErrorCode::kNone)
                 ? 0
                 : 2;
    case CliAction::kOnMediaStarted:
      return OnMediaStarted() ? 0 : 2;  // 会永远返回 0（见函数注释）
    case CliAction::kVerifyLocal:
      if (opt.manifest_path.empty()) {
        Log("--verify 需要清单路径，例如 --verify=/tmp/manifest.json");
        return 1;
      }
      return VerifyLocal(opt.manifest_path, opt.signature_path) ? 0 : 2;
    case CliAction::kStatus:
      PrintStatus();
      return 0;
    case CliAction::kDaemon:
      return RunDaemon(opt);
  }
  return 1;
}

int Session::RunDaemon(const Options &opt) {
  (void)opt;
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  evs::comm::CommManager &mgr = evs::comm::CommManager::getInstance();
  // 回调必须在 Create 之前注册：SetReceiveCallback 只对之后创建的对象生效
  mgr.SetReceiveCallback(
      [this](const std::string &name, const evs::comm::CommFrame &frame) {
        OnFrame(name, frame);
      });
  mgr.SetStateCallback([](const std::string &name, evs::comm::CommState st) {
    Log("通道 %s 状态 %s", name.c_str(), evs::comm::ToString(st));
  });

  if (!bus_.Init(cfg_)) Log("事件上报降级为只打印（不影响状态机）");
  bus_.Start();

  PrintStatus();
  Log("常驻模式：等待 %s 下的命令（Ctrl+C 退出）", cfg_.sub_topic.c_str());

  while (g_run) {
    DrainCommands();
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 200L * 1000L * 1000L;  // 200ms，与 main_comm.cpp 一致
    nanosleep(&ts, nullptr);
  }

  Log("收到退出信号，停止通道");
  bus_.Stop();
  mgr.Clear();
  Log("有序退出完成");
  return 0;
}

}  // namespace

const char *Usage() {
  return
      "用法: evs_upgrade [配置文件] [动作]\n"
      "\n"
      "动作（不带 = 常驻守护，订阅 cmd/# 等命令）:\n"
      "  --check              拉清单 + 验签 + 版本/硬件/ABI 判定（不下包）\n"
      "  --apply              检查 → 下载 → 校验 → 解包 → 切换 → 健康检查\n"
      "  --rollback           手工回滚到 previous（需 systemctl，见设计 §7.3）\n"
      "  --on-media-started   evs-media 的 ExecStartPost：boot_count += 1（§7.2）\n"
      "  --verify=<manifest>  只验本地清单签名（发布机自检；签名默认 <manifest>.sig）\n"
      "  --sig=<file>         配合 --verify 指定签名文件\n"
      "  --status             打印槽位 / 状态文件 / 剩余空间\n"
      "\n"
      "其它:\n"
      "  --dry-run            只打印将要做什么，不落盘、不下载、不切链\n"
      "  --reason=<text>      --rollback 的原因，原样带进上报（如 start-limit）\n"
      "  -h, --help           显示本帮助\n"
      "\n"
      "配置文件：不给则依次找 config/upgrade.json、../config/upgrade.json\n"
      "退出码：0 成功 / 1 参数或配置错 / 2 动作失败（原因码见上报 code 字段）\n";
}

bool ParseArgs(int argc, char **argv, Options *out, std::string *err) {
  if (out == nullptr) return false;
  for (int i = 1; i < argc; ++i) {
    const std::string a(argv[i]);
    if (a == "--check") {
      out->action = CliAction::kCheck;
    } else if (a == "--apply") {
      out->action = CliAction::kApply;
    } else if (a == "--rollback") {
      out->action = CliAction::kRollback;
    } else if (a == "--on-media-started") {
      out->action = CliAction::kOnMediaStarted;
    } else if (a == "--status") {
      out->action = CliAction::kStatus;
    } else if (a == "--dry-run") {
      out->dry_run = true;
    } else if (a == "-h" || a == "--help") {
      out->help = true;
    } else if (a.compare(0, 9, "--verify=") == 0) {
      out->manifest_path = a.substr(9);
      out->action = CliAction::kVerifyLocal;
    } else if (a.compare(0, 6, "--sig=") == 0) {
      out->signature_path = a.substr(6);
    } else if (a.compare(0, 9, "--reason=") == 0) {
      out->reason = a.substr(9);
    } else if (!a.empty() && a[0] == '-') {
      *err = "不认识的选项: " + a;
      return false;
    } else if (out->config_path.empty()) {
      out->config_path = a;
    } else {
      *err = "多余的位置参数: " + a + "（只接受一个配置文件路径）";
      return false;
    }
  }
  return true;
}

int Run(const Options &options) {
  if (options.help) {
    std::printf("%s", Usage());
    return 0;
  }

  UpgradeConfig cfg;
  std::string err;
  const std::vector<std::string> candidates = CandidatePaths(options.config_path);
  std::string used_path;
  bool loaded = false;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (LoadConfig(candidates[i], &cfg, &err)) {
      used_path = candidates[i];
      loaded = true;
      break;
    }
  }
  if (!loaded) {
    std::fprintf(stderr, "[upgrade] 配置读取失败: %s\n", err.c_str());
    std::fprintf(stderr, "[upgrade] 已尝试:");
    for (size_t i = 0; i < candidates.size(); ++i)
      std::fprintf(stderr, " %s", candidates[i].c_str());
    std::fprintf(stderr, "\n[upgrade] 说明：升级进程不使用内置默认值（§10.2），配置错了比不启动更危险\n");
    return 1;
  }
  Log("配置来源: %s", used_path.c_str());
  evs::common::SetCrashTag("config", used_path.c_str());

  Session session(cfg);
  session.Init();  // 布局不可写只降级，不阻断（见函数注释）

  const int rc = session.RunOnce(options.action, options);
  if (options.action != CliAction::kDaemon)
    Log("动作结束：exit=%d", rc);
  return rc;
}

}  // namespace upgrade_service
}  // namespace evs
