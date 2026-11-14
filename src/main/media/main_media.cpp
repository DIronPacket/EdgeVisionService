// ============================================================
// 进程 1：媒体处理服务（可执行名 evs_media）
//
// 只做入口三件事：取配置路径 → 填 app_name（崩溃报告用）→ 调 evs::media::Run()。
// 真正的编排（配置 → 内存池 → 装配 → 启停 → FreeMem）在 media_service.cpp，
// 与历史入口 EdgeVisionService 共用同一份实现。
//
// 用法：
//   ./out/bin/evs_media                        # 自动找 config/node.json → 回退 push.json
//   ./out/bin/evs_media /abs/path/node.json    # 显式指定（显式路径不回退）
//   make run-media                             # = 上面那条（cwd 在 out/）
// ============================================================

#include <string>

#include "media_service.h"

int main(int argc, char **argv)
{
    evs::media::MediaServiceOptions opt;
    opt.app_name = "evs_media";
    if (argc > 1 && argv[1] != nullptr)
        opt.config_path = std::string(argv[1]);
    return evs::media::Run(opt);
}
