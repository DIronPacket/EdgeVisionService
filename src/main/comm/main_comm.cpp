// ============================================================
// 进程 2：通信服务（可执行名 evs_comm）
//
// 职责（当前是骨架，见 docs/design/DESIGN_PROCESS_SPLIT.md §4）：
//   1. 读 config/comm.json，把 channels[] 变成 CommConfig
//   2. 用全局 CommManager 创建通道（UDP / MQTT，server / client 由配置决定）
//   3. 统一收包回调：打印 通道名 / topic / peer / 长度 / 前若干字节
//   4. SIGINT / SIGTERM → StopAll + Clear → 退出 0
//
// 还没做（明确记在 src/comm/README.md §9）：拉起并守护 broker（fork/exec）、
// 消息分帧与编解码、命令路由、心跳超时判定、状态汇聚、TLS。
//
// 用法：
//   ./out/bin/evs_comm                        # 依次找 config/comm.json、../config/comm.json
//   ./out/bin/evs_comm /abs/path/comm.json    # 显式指定
//   make run-comm                             # = 上面那条（cwd 在 out/）
//
// 注意：没有配置文件 / 通道建不起来时**不退出**，只打印原因并等信号 ——
//       这样 systemd 里它不会因为 broker 没起来就反复重启（重连由 MqttComm 内部负责）。
// ============================================================

#include <csignal>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <json.hpp>

#include "comm_manager.h"
#include "comm_types.h"
#include "crash_handler.h"

namespace
{

volatile sig_atomic_t g_run = 1;

void OnSignal(int sig)
{
    (void)sig;
    g_run = 0;
}

// ---------- 配置读取 ----------

bool ReadWholeFile(const std::string &path, std::string &out)
{
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open())
        return false;
    std::ostringstream oss;
    oss << ifs.rdbuf();
    out = oss.str();
    return !out.empty();
}

// 候选路径：显式给的就只试它（显式路径不静默回退）；否则 config/ 与 ../config/
std::vector<std::string> CandidatePaths(const std::string &explicit_path)
{
    std::vector<std::string> paths;
    if (!explicit_path.empty())
    {
        paths.push_back(explicit_path);
        return paths;
    }
    paths.push_back("config/comm.json");
    paths.push_back("../config/comm.json");
    return paths;
}

// ---------- 字段读取（类型不符就当没写，不抛异常）----------

void ReadString(const nlohmann::json &o, const char *key, std::string &dst)
{
    if (o.is_object() && o.contains(key) && o[key].is_string())
        dst = o[key].get<std::string>();
}

void ReadInt(const nlohmann::json &o, const char *key, int &dst)
{
    if (o.is_object() && o.contains(key) && o[key].is_number_integer())
        dst = o[key].get<int>();
}

void ReadBool(const nlohmann::json &o, const char *key, bool &dst)
{
    if (o.is_object() && o.contains(key) && o[key].is_boolean())
        dst = o[key].get<bool>();
}

// channels[i] → CommConfig。解析失败返回 false 并写 err（带下标，直接指到数组哪一项）。
bool ParseChannel(const nlohmann::json &item, size_t index, evs::comm::CommConfig &cfg,
                  std::string &err)
{
    const std::string where = "channels[" + std::to_string(index) + "]";
    if (!item.is_object())
    {
        err = where + ": 必须是对象";
        return false;
    }

    std::string type_text;
    std::string role_text;
    ReadString(item, "type", type_text);
    ReadString(item, "role", role_text);

    if (type_text.empty())
    {
        err = where + ": 缺少 type（udp / mqtt）";
        return false;
    }
    if (!evs::comm::ParseCommType(type_text, &cfg.type))
    {
        err = where + ": type 取值非法 '" + type_text + "'（可选 udp / mqtt）";
        return false;
    }
    if (!role_text.empty() && !evs::comm::ParseCommRole(role_text, &cfg.role))
    {
        err = where + ": role 取值非法 '" + role_text + "'（可选 server / client）";
        return false;
    }

    ReadString(item, "name", cfg.name);
    ReadString(item, "host", cfg.host);
    ReadInt(item, "port", cfg.port);
    ReadString(item, "topic", cfg.topic);
    ReadString(item, "topic_pub", cfg.topic_pub);
    ReadString(item, "client_id", cfg.client_id);
    ReadString(item, "username", cfg.username);
    ReadString(item, "password", cfg.password);
    ReadInt(item, "keepalive_sec", cfg.keepalive_sec);
    ReadInt(item, "qos", cfg.qos);
    ReadBool(item, "retain", cfg.retain);
    ReadInt(item, "reconnect_delay_ms", cfg.reconnect_delay_ms);
    ReadBool(item, "udp_broadcast", cfg.udp_broadcast);
    ReadInt(item, "recv_timeout_ms", cfg.recv_timeout_ms);

    if (cfg.port < 0 || cfg.port > 65535)
    {
        err = where + ": port 超出 0..65535";
        return false;
    }
    if (cfg.qos < 0 || cfg.qos > 2)
    {
        err = where + ": qos 只能是 0 / 1 / 2";
        return false;
    }
    return true;
}

// 安全打印前面若干字节：非可打印字符显示为 '.'
std::string Preview(const uint8_t *data, size_t size, size_t max_len = 48)
{
    const size_t n = size < max_len ? size : max_len;
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
    {
        const unsigned char c = data[i];
        out.push_back((c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.');
    }
    if (size > n)
        out += "...";
    return out;
}

void OnFrame(const std::string &name, const evs::comm::CommFrame &frame)
{
    // 回调在接收线程里同步执行：只做打印，不重活、不等锁（src/comm/README.md §5）
    std::printf("[comm] 收包 通道=%s topic=%s peer=%s size=%zu body=\"%s\"\n",
                name.c_str(),
                frame.topic.empty() ? "-" : frame.topic.c_str(),
                frame.peer.empty() ? "-" : frame.peer.c_str(),
                frame.size,
                Preview(frame.data, frame.size).c_str());
    std::fflush(stdout);
}

void OnState(const std::string &name, evs::comm::CommState state)
{
    std::printf("[comm] 状态 通道=%s → %s\n", name.c_str(),
                evs::comm::ToString(state));
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    // ---------- 0. 崩溃现场：与媒体进程同一套（报告前缀是 evs_comm）----------
    evs::common::CrashConfig crash_cfg;
    crash_cfg.app_name = "evs_comm";
    crash_cfg.report_dir = "";
    crash_cfg.dump_core = true;
    evs::common::InstallCrashHandler(crash_cfg);

    // ---------- 1. 读配置 ----------
    const std::string explicit_path =
        (argc > 1 && argv[1] != nullptr) ? std::string(argv[1]) : std::string();
    const std::vector<std::string> candidates = CandidatePaths(explicit_path);

    std::string text;
    std::string used_path;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (ReadWholeFile(candidates[i], text))
        {
            used_path = candidates[i];
            break;
        }
    }

    std::vector<evs::comm::CommConfig> configs;
    if (used_path.empty())
    {
        std::cout << "[comm] 没有找到通信配置（试过：";
        for (size_t i = 0; i < candidates.size(); ++i)
            std::cout << (i ? "、" : "") << candidates[i];
        std::cout << "）—— 不创建任何通道，只等退出信号。" << std::endl;
        std::cout << "[comm] 示例见 config/comm.json；用法见 src/comm/README.md"
                  << std::endl;
    }
    else
    {
        std::cout << "[comm] 配置来源: " << used_path << std::endl;
        evs::common::SetCrashTag("config", used_path.c_str());

        nlohmann::json root;
        try
        {
            root = nlohmann::json::parse(text);
        }
        catch (const std::exception &e)
        {
            std::cerr << "[comm] JSON 语法错误: " << e.what() << std::endl;
            return 1;
        }

        const nlohmann::json *items = nullptr;
        if (root.is_array())
            items = &root; // 允许直接把配置写成数组
        else if (root.is_object() && root.contains("channels"))
        {
            if (!root["channels"].is_array())
            {
                std::cerr << "[comm] channels 必须是数组" << std::endl;
                return 1;
            }
            items = &root["channels"];
        }
        else
        {
            std::cerr << "[comm] 配置里既没有 channels 数组，本身也不是数组" << std::endl;
            return 1;
        }

        for (size_t i = 0; i < items->size(); ++i)
        {
            evs::comm::CommConfig cfg;
            std::string err;
            if (!ParseChannel((*items)[i], i, cfg, err))
            {
                std::cerr << "[comm] 配置错误: " << err << std::endl;
                return 1;
            }
            configs.push_back(cfg);
        }
        std::cout << "[comm] 通道 " << configs.size() << " 个" << std::endl;
    }

    // ---------- 2. 建通道 ----------
    evs::comm::CommManager &mgr = evs::comm::CommManager::getInstance();
    mgr.SetReceiveCallback(OnFrame);
    mgr.SetStateCallback(OnState);

    size_t created = 0;
    for (size_t i = 0; i < configs.size(); ++i)
    {
        evs::comm::CommBase *c = mgr.Create(configs[i]);
        if (c == nullptr)
        {
            // 单个通道失败不影响其他通道：与流水线「单链失败只告警」同一策略
            std::cerr << "[comm] 创建通道失败[" << i << "]: " << mgr.last_error()
                      << std::endl;
            continue;
        }
        ++created;
        std::printf("[comm] 通道就绪 %s（%s / %s / %s:%d）\n",
                    configs[i].name.empty() ? "<自动命名>" : configs[i].name.c_str(),
                    evs::comm::ToString(configs[i].type),
                    evs::comm::ToString(configs[i].role),
                    configs[i].host.c_str(), configs[i].port);
    }

    if (created > 0)
    {
        if (!mgr.StartAll())
            std::cerr << "[comm] 部分通道启动失败: " << mgr.last_error() << std::endl;
        else
            std::cout << "[comm] " << created << " 个通道已启动" << std::endl;
    }
    else
    {
        std::cout << "[comm] 没有可用通道，只等退出信号（Ctrl+C）" << std::endl;
    }

    // ---------- 3. 等信号（与媒体进程相同的 200ms 轮询）----------
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    std::cout << "[comm] 运行中（Ctrl+C 退出）" << std::endl;
    while (g_run)
    {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200L * 1000L * 1000L;
        nanosleep(&ts, nullptr);
    }

    // ---------- 4. 有序退出：先全停，再清注册表 ----------
    std::cout << "[comm] 收到退出信号，停止通道" << std::endl;
    mgr.StopAll();
    mgr.Clear();
    std::cout << "[comm] 通道已清理，进程正常退出" << std::endl;
    return 0;
}

