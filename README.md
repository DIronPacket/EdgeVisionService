# EdgeVisionService

> **边缘视觉服务原型**：配置驱动装配 → 视频采集 → 统一内存池 → 节点流水线 → MPP 硬件编码 → RTSP 推流。
> 主平台 **RK3588**（aarch64 板端本机编译，MPP / V4L2 / RGA）；x86_64（FFmpeg）作为开发调试目标。
> 工程标准 **C++14** · CMake 3.22 · g++ 11.4 · 8 核 · 无 CTest / CI（回归靠 `scripts/tests/` 的可重跑脚本）。

## 1. 这是什么

| 项 | 值 |
| --- | --- |
| **能跑什么** | `config/node.json` 描述的 **4 路链路**（1 个摄像头 + 3 路 mp4）同时出帧并推 RTSP；`Ctrl+C` → 正序停线程 → 释放 ZLM 流 → `FreeMem` 归还 50 块 → 程序退出码 0。实测 `[print3] 实测=23.82fps` |
| **进程形态** | **两个进程 + 一个兼容壳**（2026-11-14 拆分）：`evs_media`（媒体）/ `evs_comm`（通信）/ `EdgeVisionService`（旧名字，与 `evs_media` 共用同一份 `media_service` 逻辑） |
| **通信** | `src/comm/` = `libcomm_api.so`：`CommBase` + `UdpComm` / `MqttComm` + 全局 `CommManager`，`scripts/tests/run_t6_comm.sh` 43 项断言。**媒体进程尚未接总线**（不建通道、不上报 `media/evt/*`） |
| **主要缺口** | 无日志 / 指标体系、无单测 / CI、扇出拓扑不可达（T9）、池块数与队列深度未接线（T10a）、x86 依赖缺失（P0-1）；**媒体与通信两进程之间还没有消息路由** |
| **P0 现状** | 共 14 项：已修 10 项、核实假阳性 1 项（P0-5）、剩 3 项（P0-1 / P0-8 / P0-10 残余）。明细见 [docs/STATUS.md](docs/STATUS.md) |

## 2. 30 秒跑起来

```bash
# 0) 依赖不入库：FFmpeg 5.x / Rockchip MPP / ZLMediaKit / mosquitto 2.0.22 需自备到 install/rk3588/{include,lib}
#    （OpenSSL 3.5.4 与 FreeType 可选；查找策略见 cmake/README.md）
make                     # 首次自动 configure；产物在 out/bin、out/lib
make run-media           # 4 路出帧；Ctrl+C → 有序停止 → FreeMem → 程序自身退出码 0
make run-comm            # 通信进程（无 broker 只报 Connection refused，不退出）
make info                # 看当前编哪些进程（EVS_PROCESS）与构建参数
```

- **只编一个进程**：`make media` / `make comm`；**改默认集**：`make proc PROC=comm`（写进 `out/CMakeCache.txt`，`PROC=all` 恢复）；等价 CMake 写法 `cmake -S . -B out -DEVS_PROCESS=media,comm`。
- **`make run` 与 `make run-media` 是同一个程序**：两者都调 `evs::media::Run()`，`EdgeVisionService` 只是历史兼容的可执行名，日志只差 ZLM 前缀（`[EdgeVisionService]` / `[evs_media]`）；通信进程请用 `make run-comm`（`[comm]` 前缀）。
- **退出码 130 ≠ 崩溃**：`make run-*` 里 Ctrl+C 的 SIGINT 会同时打到 make，所以 **make 自身返回 130**，程序仍是 0（尾部有 `进程正常退出`）。要看程序的 rc 用 `bash scripts/run/out/start.sh` 或 `(cd out && ./bin/evs_media ../config/node.json)`。2026-11-14 起运行配方末尾加了 `exec`，**关程序后不会再出现 `make: *** [Makefile:…] Interrupt` 那一行**。
- **播放**：`ffplay rtsp://<板端IP>:8550/live/ch0`（每路 `ch0`~`ch3`）。
- **自检**：`bash scripts/tests/run_t6_comm.sh`（其余见 [scripts/tests/README.md](scripts/tests/README.md)）。
- 命令细节、实测输出与踩坑：**[docs/ops/COMMANDS.md](docs/ops/COMMANDS.md)** §1 构建 / §2 运行 / §3 内存 / §11 崩溃 / §12 自启。

## 3. README 索引（每个目录一篇，改哪块先看哪篇）

| 位置 | README | 里面有什么 |
| --- | --- | --- |
| `docs/` | [docs/README.md](docs/README.md) | **文档总索引**：按场景导航 + 目录分区判据（不知道读哪篇从这里进） |
| `src/` | [src/README.md](src/README.md) | 编译出什么、每层职责、帧怎么流动、改哪类需求去哪个目录 |
| `src/comm/` | [src/comm/README.md](src/comm/README.md) | 通信层：server/client 语义、回调三条硬约束、已知限制 |
| `src/common/preprocess/` | [README.md](src/common/preprocess/README.md) | 预处理框架：能力边界、怎么调、哪里还没实现 |
| `config/` | [config/README.md](config/README.md) | `node.json`（v1 多链路）/ `push.json`（v0）/ `comm.json` 每个字段 |
| `cmake/` | [cmake/README.md](cmake/README.md) | `Find*.cmake` 查找策略、`TARGET_PLATFORM` 判定、交叉编译工具链 |
| `scripts/` | [scripts/README.md](scripts/README.md) | 脚本总览（约定：一律从仓库根执行） |
| `scripts/tests/` | [README.md](scripts/tests/README.md) | 五套回归脚本清单（T1 / T2 / T3 / T5 / T6，共 282 项断言） |
| `scripts/coredump/` | [README.md](scripts/coredump/README.md) | 崩溃现场采集与解析（core 命名、符号化、gcore） |
| `scripts/run/` | [README.md](scripts/run/README.md) | `out/` 与 `build/` 两个同名 `start.sh` |
| `scripts/deploy/` | [README.md](scripts/deploy/README.md) | systemd unit（开机自启） |
| `docs/design/` | [README.md](docs/design/README.md) | 设计与规划（多链路 / 三进程拆分 / ROADMAP） |
| `docs/history/` | [README.md](docs/history/README.md) | `TESTLOG.md`（测试证据）+ `BUG_CASES.md`（bug 原理复盘） |
| `docs/ops/` | [README.md](docs/ops/README.md) | `COMMANDS.md`（命令手册）+ `CRASH_DUMP.md`（崩溃链路） |
| `docs/interview/` | [README.md](docs/interview/README.md) | 面试问答（高频 / 核心 / 深入 + 内核边界 + C 落点） |
| 仓库根（AI 入口） | [AGENTS.md](AGENTS.md) · [.clinerules/00-handoff.md](.clinerules/00-handoff.md) | 自动加载的接手卡，二者都指向 `docs/HANDOFF.md` |

## 4. 四篇必读（按这个顺序）

| 顺序 | 文档 | 什么时候读 |
| --- | --- | --- |
| 1 | [docs/HANDOFF.md](docs/HANDOFF.md) | **接手第一天**：30 秒事实卡 + 红线 §2 + 任务→文件索引 §1.9 |
| 2 | [docs/STATUS.md](docs/STATUS.md) | **动手之前**：模块完成度、P0–P2 明细、优化建议 |
| 3 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | **改结构之前**：分层、数据流、内存池与线程模型、扩展方式 |
| 4 | [docs/history/TESTLOG.md](docs/history/TESTLOG.md) | **想声称"已修复"之前**：怎么验、验过什么（没有记录视为未验证） |

## 5. 红线摘要（全文见 [docs/HANDOFF.md](docs/HANDOFF.md) §2，违反即错误交付）

1. **C++14**：不用 `std::any` / `if constexpr` / 结构化绑定等 C++17+ 特性。
2. 不引入 `install/` 里没有的新第三方库；不重命名既有目录（`moudle_*` 保持原样）。
3. 内存池取块（`acquire` / `getBlock`）之后，**所有失败分支都必须 `release`**。
4. 不提交 `out/`、`build/`、`install/`、`3rdparty/`、`.vscode/`；不 `git push --force`、不 `git reset --hard`、不碰 `main`。
5. **`git push` 之前必须先问用户**（2026-11-14 起）：先报「要推哪几笔 + 哪个分支 + 哪些文件」，得到明确同意才推。
6. 测试必须留痕（`docs/history/TESTLOG.md`）；修"有原理含量"的 bug 必须写 `docs/history/BUG_CASES.md`。

---

最后更新：2026-11-15（补「两个进程 / 运行目标 / 退出码」语义；详细内容分别落在 `docs/`、`src/`、`scripts/`、`config/`、`cmake/` 各目录 README）
适用分支：`rk3588`（已推送 `origin/rk3588`）· 代码校准点：`0117907`（通信层 + 进程拆分，2026-11-14）· 工作树干净（只剩 `config/2.mp4`、`config/test.nv12` 两台本机素材未跟踪）
