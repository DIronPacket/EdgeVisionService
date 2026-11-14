// ============================================================
// 历史兼容入口（可执行名 EdgeVisionService）
//
// 进程拆分（2026-11-14）后，媒体处理编排全部搬到
// src/main/media/media_service.cpp，本文件只剩入口三件事：
//   取配置路径（argv[1]，空则交给 load_with_fallback）→ 填 app_name → Run()
//
// 新代码请用 src/main/media/main_media.cpp（可执行 evs_media），两者行为完全一致：
//   同一份 media_service，只是崩溃报告里的程序名不同。
// 保留这个可执行名是为了 `make run` 与既有 TESTLOG 记录继续有效；
// 三进程稳定后可删除（docs/design/DESIGN_PROCESS_SPLIT.md §9 待确认项 3）。
//
// 用法：
//   make run                                    # cwd=out/，自动找 ../config/node.json
//   ./out/bin/EdgeVisionService /abs/node.json  # 显式指定（显式路径不回退）
// ============================================================

#include <string>

#include "media_service.h"

int main(int argc, char **argv)
{
    evs::media::MediaServiceOptions opt;
    opt.app_name = "EdgeVisionService";
    if (argc > 1 && argv[1] != nullptr)
        opt.config_path = std::string(argv[1]);
    return evs::media::Run(opt);
}
