# AGENTS.md

本仓库的接手入口只有一处：**`docs/HANDOFF.md`**（现状快照 + 红线规定 + 操作规程）。

- **只想了解现状** → 读 §0（30 秒事实卡）+ §1.2（关键坐标）
- **要改任何代码** → 必读 §2 全部（2.1–2.9 红线、构建、C++、流水线、CMake、Git、命名、环境、自检）
- **不知道改哪个文件** → 查 §1.9（任务→文件索引）
- **要记测试 / 查回归结果 / 证明"已修复"** → 写 / 读 **`docs/TESTLOG.md`**（L1 冒烟一行 · L2 任务小节 · L3 里程碑）；改完代码没记录 ＝ 未验证
- **修掉"有原理含量"的 bug 后** → 必须追加 **`docs/BUG_CASES.md`** 案例（现象 → 位置 → 根因原理 → 修复 → 为什么难发现 → 面试怎么讲）并更新其 §1 索引表；与代码同一批提交
- **要敲命令（构建 / 跑程序 / 看内存 / 写一次性探针 / git 提交推送）** → 查 **`docs/COMMANDS.md`**（分类命令手册 + 实测输出 + 踩坑表）
- **要准备面试 / 讲项目 / 判断"哪里碰过内核" / 找练 C 语言的落点** → 查 **`docs/INTERVIEW.md`**（高频 · 核心 · 深入问答 + 内核边界 + C 落点 + 诚实清单）
- **不要全仓扫描**：文件清单看 §1.4，构建/运行命令看 §0 与附录 A
- 行号会漂移，读之前先跑 `grep -nE '^#{1,2} ' docs/HANDOFF.md` 一次拿行号，再定向读区间

基线：分支 `rk3588` / 代码校准点 `12f5480`（帧内存改由 MPP DRM 缓冲提供：新增 `common/frame_mem.{h,cpp}` 作为唯一分配边界，`FrameBuffer::Allocate()` 走 `mpp_buffer_get`+`mmap`、`Release()` 走 `mpp_buffer_put`，`FreeMem()` 末尾 `FrameMemDeinit()` 释放缓冲组；非 rk3588 仍 `new[]`；前一校准点 `15cf975` = P0-9 / P0-11 修复 + P0-10 ①② RAII 收口，`owned_mem` + `~FrameBuffer(){Release();}` 且拷贝/赋值 `= delete`，`MemMeta` 槽位改为 `MemSlotBase*` + 虚析构自动销毁；再前 `114c94d` = P0-14 采集写块前重绑 `FrameBuffer` 视图，实际修复 `eee3b01`；再前 `c557d89` = P0-13 `MppEncoder` 按平面拷贝 NV12；再前 `eac4bb0` = T7 `ViNode` 按 `vi.src` 前缀选采集后端 + 修 P0-2）/ C++14 / aarch64 板端本机编译 / `make`（= `cmake -S . -B out && cmake --build out -j8`，默认构建目录 `out/`）。

Cline 专用入口（内容一致，自动加载）：`.clinerules/00-handoff.md`。
