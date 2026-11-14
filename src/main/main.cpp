// ============================================================
// 真实主链路入口（可执行名 testnode）—— M1 / T6
//
// 这里只做「编排」，不出现任何节点名、不碰设备路径与像素参数：
//   1. 配置      NodeConfigManager（优先 v1 node.json，没有则 v0 push.json）
//   2. 进程资源  MemManager::MemInit()（块数仍是代码默认值，T10a 才配置化）
//   3. 装配      PipelineBuilder::build_all()（工厂 + 连线，单链失败不影响其他链）
//   4. 启停      PipelineBuilder::start_all() / stop_all()（终点先起、源头先停）
//   5. 退出      SIGINT / SIGTERM → 有序停止 → FreeMem → return 0
//
// 旧版那 20 行手写装配、`while(1)` 死循环、永不执行的 stop() 已删除：
// 加一路只改 config/node.json（见 docs/design/DESIGN_MULTI_PIPELINE.md §5.5）。
//
// 用法：
//   ./build/run.sh testnode                       # 自动找 node.json → 回退 push.json
//   ./build/run.sh testnode /abs/path/node.json   # 显式指定（显式路径不回退）
// ============================================================

#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "MemManager.h"
#include "crash_handler.h"
#include "node_config_manager.h"
#include "pipeline_builder.h"

namespace
{

    // 信号处理里只能碰 volatile sig_atomic_t（异步信号安全：不加锁、不打日志、不分配）
    volatile sig_atomic_t g_run = 1;

    void OnSignal(int sig)
    {
        (void)sig;
        g_run = 0;
    }

    // 配置摘要：「配置里写了」≠「已经生效」，所以对还没接线的字段单独标注，
    // 避免下一个人看到 pool.blocks=64 就以为池子真是 64 块。
    void print_config_summary(const evs::config::NodeConfigManager &cfg)
    {
        const evs::config::GlobalDesc &g = cfg.global();
        std::cout << "[main] global: rtsp 端口 " << g.media.port << " / app \""
                  << g.media.app << "\" / pool.blocks " << g.pool.blocks
                  << " / queue_depth " << g.pool.queue_depth << std::endl;
        std::cout << "[main] 注意: pool.blocks / queue_depth / quota_per_channel 尚未接线"
                     "（T10a），当前仍用 MemInit() 默认值"
                  << std::endl;
        if (cfg.is_v0())
            std::cout << "[main] 提示: 当前走 v0 push.json 适配（合成单链路）；"
                         "迁移完成后删除该文件（T8）"
                      << std::endl;
    }

} // namespace

int main(int argc, char **argv)
{
    // ---------- 0. 崩溃现场：尽早安装（早于任何线程创建）----------
    // 之后任何一次 SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT（或 kill -QUIT）都会：
    //   1) 写 ./crash/crash_EdgeVisionService_<pid>_<epoch>_sig<N>.log
    //      含程序名 / exe 路径 / pid / tid / 信号 / 故障地址 / 寄存器 /
    //      调用栈 / 业务 tag（EVS_CRASH_DIR 可改目录）
    //   2) 把信号按默认动作重抛一次，让内核按 core_pattern 落 core
    //      —— core 用于 gdb 查看崩溃时的局部变量
    evs::common::CrashConfig crash_cfg;
    crash_cfg.app_name = "EdgeVisionService";
    crash_cfg.report_dir = "";  // 空 = $EVS_CRASH_DIR，再空 = ./crash
    crash_cfg.dump_core = true;
    evs::common::InstallCrashHandler(crash_cfg);

    // ---------- 1. 配置（不碰任何节点）----------
    evs::config::NodeConfigManager &cfg = evs::config::NodeConfigManager::getInstance();
    const std::string explicit_path =
        (argc > 1 && argv[1]) ? std::string(argv[1]) : std::string();

    if (!cfg.load_with_fallback(explicit_path))
    {
        std::cerr << "[main] 配置加载失败: " << cfg.last_error() << std::endl;
        return 1;
    }
    std::cout << "[main] 配置来源: " << cfg.source_path()
              << (cfg.is_v0() ? "（v0 push.json → 合成单链路）" : "（v1 node.json）")
              << " / 链路 " << cfg.pipelines().size() << " 条" << std::endl;
    // 崩溃报告里带上配置来源：同一份二进制跑不同配置时，一眼能对上是哪一份
    evs::common::SetCrashTag("config", cfg.source_path().c_str());
    print_config_summary(cfg);

    // ---------- 2. 进程级资源（一次性，与链路数量无关）----------
    MemManager::getInstance().MemInit();

    // ---------- 3. 装配：配置 → 已连线链路，main 里不出现节点名 ----------
    PipelineBuilder builder(cfg);
    std::vector<PipelineSet> sets;
    if (!builder.build_all(sets))
    {
        std::cerr << "[main] 没有任何链路建成，退出: " << builder.last_error()
                  << std::endl;
        MemManager::getInstance().FreeMem();
        return 1;
    }
    for (size_t i = 0; i < sets.size(); ++i)
        std::cout << "[main] 链路[" << i << "] " << sets[i].describe() << std::endl;

    // N3：单链失败只告警，其余照跑（失败原因带字段路径，直接指向配置文件哪一行）
    if (builder.failed_count() > 0)
    {
        std::cerr << "[main] 警告: " << builder.failed_count()
                  << " 条链路构建失败（其余链路继续运行）" << std::endl;
        const std::vector<std::string> &errs = builder.errors();
        for (size_t i = 0; i < errs.size(); ++i)
            std::cerr << "[main]   失败原因: " << errs[i] << std::endl;
    }

    // ---------- 4. 信号 + 启动 ----------
    // 信号必须在 start_all 之前装好：启动瞬间按 Ctrl+C 不能变成「默认动作直接杀进程」
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    PipelineBuilder::start_all(sets);
    std::cout << "[main] " << sets.size() << " 条链路已启动（Ctrl+C 退出）"
              << std::endl;

    while (g_run)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ---------- 5. 有序退出：与启动相反，进程级资源最后释放 ----------
    std::cout << "[main] 收到退出信号，开始有序退出" << std::endl;

    PipelineBuilder::stop_all(sets); // 正序：源头先停，下游把队列里已到的块处理完
    sets.clear();                    // 释放节点（析构里的 stop() 此时是空操作）
    MemManager::getInstance().FreeMem();

    std::cout << "[main] FreeMem 完成，进程正常退出" << std::endl;
    return 0;
}
