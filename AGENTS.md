# AGENTS.md

本仓库的接手入口只有一处：**`docs/HANDOFF.md`**（现状快照 + 红线规定 + 操作规程）。
不知道该读哪一篇 / 想找某个子目录的说明 → 先看 **`docs/README.md`**（文档总索引）+ 各目录 `README.md`（`docs/`、`scripts/`、`cmake/`、`config/`、`src/`）。

- **只想了解现状** → 读 §0（30 秒事实卡）+ §1.2（关键坐标）
- **要改任何代码** → 必读 §2 全部（2.1–2.9 红线、构建、C++、流水线、CMake、Git、命名、环境、自检）
- **不知道改哪个文件** → 查 §1.9（任务→文件索引）
- **要记测试 / 查回归结果 / 证明"已修复"** → 写 / 读 **`docs/history/TESTLOG.md`**（L1 冒烟一行 · L2 任务小节 · L3 里程碑）；改完代码没记录 ＝ 未验证
- **修掉"有原理含量"的 bug 后** → 必须追加 **`docs/history/BUG_CASES.md`** 案例（现象 → 位置 → 根因原理 → 修复 → 为什么难发现 → 面试怎么讲）并更新其 §1 索引表；与代码同一批提交
- **要敲命令（构建 / 跑程序 / 看内存 / 写一次性探针 / git 提交推送）** → 查 **`docs/ops/COMMANDS.md`**（分类命令手册 + 实测输出 + 踩坑表）
- **要准备面试 / 讲项目 / 判断"哪里碰过内核" / 找练 C 语言的落点** → 查 **`docs/interview/INTERVIEW.md`**（高频 · 核心 · 深入问答 + 内核边界 + C 落点 + 诚实清单）
- **程序崩了 / 卡死 / 要看崩溃时的变量** → 读 **`docs/ops/CRASH_DUMP.md`**（信号→handler→core 全链路 / core 命名格式符 / 抓不到的原因（按链路 7 段 27 条））+ §1.10（四层与四条约束）+ **`docs/ops/COMMANDS.md` §11**（体检 / 开 core / 符号化 / 解析 core / 归档改名 / gcore）+ `docs/history/BUG_CASES.md` B12/B13
- **想让程序开机自启** → `docs/ops/COMMANDS.md` §12（只改 systemd unit；`LimitCORE=infinity` 是能否抓到 core 的关键）
- **不要全仓扫描**：文件清单看 §1.4，构建/运行命令看 §0 与附录 A
- 行号会漂移，读之前先跑 `grep -nE '^#{1,2} ' docs/HANDOFF.md` 一次拿行号，再定向读区间

基线：分支 `rk3588` / 已推送至 `origin/rk3588`（HEAD `c6d040c`）/ 代码校准点 `f0b7899`（崩溃现场采集：`common/crash/crash_handler` 信号→报告→解除屏蔽重抛→core；构建默认 `RelWithDebInfo` + `-fno-omit-frame-pointer` + `-rdynamic`；`scripts/coredump/*` 全套诊断脚本；`scripts/run/{out,build}/start.sh`）/ 前一校准点 `45a070c`（顶层 `Makefile` 薄包装 + 默认构建目录 `out/` + 修 P1-2）/ 再前 `12f5480`（帧内存改由 MPP DRM 缓冲提供：`common/frame_mem.{h,cpp}` 唯一分配边界，`Allocate()` 走 `mpp_buffer_get`+`mmap`、`Release()` 走 `mpp_buffer_put`，`FreeMem()` 末尾 `FrameMemDeinit()`）/ 再前 `15cf975`（P0-9 / P0-11 修复 + P0-10 ①② RAII 收口）/ C++14 / aarch64 板端本机编译 / `make`（= `cmake -S . -B out && cmake --build out -j8`）。

Cline 专用入口（内容一致，自动加载）：`.clinerules/00-handoff.md`。

工作树追加（已入库 `f0b7899`）：崩溃现场采集 —— `src/common/crash/crash_handler.{h,cpp}`（信号 → 报告 → 解除屏蔽重抛 → core）+ `scripts/coredump/*.sh` + 构建默认 `RelWithDebInfo` / `-rdynamic`；`.gitignore` 例外放行 `scripts/run/{out,build}/`（否则 start.sh 被 `out/`、`build/` 规则整目录忽略）。`config/2.mp4`、`config/test.nv12` 为本机素材，**不要 `git add -A`**。
